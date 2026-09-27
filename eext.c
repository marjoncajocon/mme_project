/*
** eext.c - extensions: VS Code's, from a .vsix file, and those VS Code has
**
** A VS Code extension is a folder with a package.json; its JavaScript runs
** in VS Code's extension host (Node and the vscode API), which a program
** in C has not. What mme takes from an extension is what its package.json
** "contributes" as data: color themes, snippets, and languages (their file
** extensions and comments). Grammars, commands, debuggers, views ... are
** listed on its page but do nothing here.
**
** mme never goes to a marketplace: an extension is a .vsix file the user
** downloaded, installed with Install from VSIX... into mme-data/extensions,
** as VS Code keeps them: <publisher>.<name>-<version>/. tar (Windows'
** bsdtar) or unzip takes the .vsix (a zip) apart. Those VS Code installed
** (~/.vscode/extensions) are read too, never changed, when
** "mme.extensions.useVSCodeExtensions" is true.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** The installed ones
** ===================================================================
*/

typedef struct ETheme {
  char *label, *path;
  int light;	/* uiTheme: 0 "vs-dark", 1 "vs", 2 "hc-black", 3 "hc-light" */
} ETheme;

typedef struct EIcon {	/* a file icon theme: workbench.iconTheme names its id (eicons.c) */
  char *id, *label, *path;
} EIcon;

typedef struct ESnip {
  char *lang;	/* NULL: a .code-snippets for every language */
  char *path;
} ESnip;

typedef struct ELang {
  char *id, *alias, *config;
  Vec exts, names;	/* ".foo", "Foofile" */
  int read;	/* config read: */
  char *line, *open, *close;	/* its comments */
} ELang;

typedef struct Ext {
  char *id;	/* publisher.name, lower case */
  char *name, *publisher, *version, *display, *desc;
  char *dir;
  int vscode;	/* VS Code's: read, never changed */
  ETheme *theme;
  size_t ntheme;
  ESnip *snip;
  size_t nsnip;
  ELang *lang;
  size_t nlang;
  int ngrammar, ncommand, nkey, ndebug, nicon, nconfig;	/* what mme does not use */
  EIcon *icons;	/* nicon of them */
  int code;	/* it has code (package.json's main): it can be turned on and off */
  Vec onlang;	/* the languages its activationEvents name (onLanguage:go): what it serves, with lang's */
} Ext;

static Ext *g_ext;
static size_t g_next, g_capext;
static int g_scanned;	/* 1 + the useVSCodeExtensions it was for */


static void ext_free (Ext *e) {
  size_t i;
  free(e->id);
  free(e->name);
  free(e->publisher);
  free(e->version);
  free(e->display);
  free(e->desc);
  free(e->dir);
  for (i = 0; i < e->ntheme; i++) {
    free(e->theme[i].label);
    free(e->theme[i].path);
  }
  free(e->theme);
  for (i = 0; i < (size_t)e->nicon; i++) {
    free(e->icons[i].id);
    free(e->icons[i].label);
    free(e->icons[i].path);
  }
  free(e->icons);
  for (i = 0; i < e->nsnip; i++) {
    free(e->snip[i].lang);
    free(e->snip[i].path);
  }
  free(e->snip);
  for (i = 0; i < e->nlang; i++) {
    ELang *l = &e->lang[i];
    free(l->id);
    free(l->alias);
    free(l->config);
    vec_free(&l->exts);
    vec_free(&l->names);
    free(l->line);
    free(l->open);
    free(l->close);
  }
  free(e->lang);
  vec_free(&e->onlang);
}


static char *lower_dup (const char *s) {
  char *d = xstrdup(s), *p;
  for (p = d; *p; p++)
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
  return d;
}


/* "%displayName%": the text package.nls.json has for it */
static const char *nls (const Json *tab, const char *s) {
  size_t n, i;
  if (s == NULL || tab == NULL || s[0] != '%' || (n = strlen(s)) < 3 || s[n - 1] != '%') return s;
  for (i = 0; i < tab->n; i++) {
    const Json *k = tab->kid[i];
    if (k->key && strlen(k->key) == n - 2 && strncmp(k->key, s + 1, n - 2) == 0) {
      if (k->type == J_STR) return k->str;
      if (k->type == J_OBJ) return json_str(json_get(k, "message"), s);
    }
  }
  return s;
}


/* a file of the extension: "./themes/x.json" */
static char *ext_file (const char *dir, const char *rel) {
  while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) rel += 2;
  return path_join(dir, rel);
}


static Json *read_json (const char *path) {
  size_t len;
  char *s = read_file(path, &len);
  Json *j;
  if (s == NULL) return NULL;
  j = json_parse(s, len);
  free(s);
  return j;
}


static size_t count (const Json *j) {
  return j && j->type == J_ARR ? j->n : 0;
}


/* its package.json read into e; -1 when it is not an extension */
static int ext_read (Ext *e, const char *dir, int vscode) {
  char *f = path_join(dir, "package.json");
  Json *pk = read_json(f), *nl;
  const Json *c, *a;
  const char *name, *pub;
  size_t i;
  free(f);
  memset(e, 0, sizeof(*e));
  if (pk == NULL) return -1;
  name = json_str(json_get(pk, "name"), NULL);
  pub = json_str(json_get(pk, "publisher"), NULL);
  if (name == NULL || pub == NULL) {
    json_free(pk);
    return -1;
  }
  f = path_join(dir, "package.nls.json");
  nl = read_json(f);
  free(f);
  {
    char *id = xstrcat3(pub, ".", name);
    e->id = lower_dup(id);
    free(id);
  }
  e->name = xstrdup(name);
  e->publisher = xstrdup(pub);
  e->version = xstrdup(json_str(json_get(pk, "version"), "0.0.0"));
  e->display = xstrdup(nls(nl, json_str(json_get(pk, "displayName"), name)));
  e->desc = xstrdup(nls(nl, json_str(json_get(pk, "description"), "")));
  e->dir = xstrdup(dir);
  e->vscode = vscode;
  c = json_get(pk, "contributes");
  a = json_get(c, "themes");
  if (count(a)) {
    e->theme = (ETheme *)xmalloc(a->n * sizeof(ETheme));
    for (i = 0; i < a->n; i++) {
      const char *p = json_str(json_get(a->kid[i], "path"), NULL);
      const char *ui = json_str(json_get(a->kid[i], "uiTheme"), "vs-dark");
      size_t pl;
      if (p == NULL || ((pl = strlen(p)) > 8 && m_stricmp(p + pl - 8, ".tmTheme") == 0)) continue;	/* TextMate's XML: not read */
      e->theme[e->ntheme].label = xstrdup(nls(nl, json_str(json_get(a->kid[i], "label"),
                                                           json_str(json_get(a->kid[i], "id"), e->display))));
      e->theme[e->ntheme].path = ext_file(dir, p);
      e->theme[e->ntheme].light = strcmp(ui, "vs") == 0 ? 1 : strcmp(ui, "hc-black") == 0 ? 2 : strcmp(ui, "hc-light") == 0 ? 3 : 0;
      e->ntheme++;
    }
  }
  a = json_get(c, "snippets");
  if (count(a)) {
    e->snip = (ESnip *)xmalloc(a->n * sizeof(ESnip));
    for (i = 0; i < a->n; i++) {
      const char *p = json_str(json_get(a->kid[i], "path"), NULL), *l = json_str(json_get(a->kid[i], "language"), NULL);
      if (p == NULL) continue;
      e->snip[e->nsnip].lang = l ? xstrdup(l) : NULL;
      e->snip[e->nsnip].path = ext_file(dir, p);
      e->nsnip++;
    }
  }
  a = json_get(c, "languages");
  if (count(a)) {
    e->lang = (ELang *)xmalloc(a->n * sizeof(ELang));
    for (i = 0; i < a->n; i++) {
      const Json *x = a->kid[i], *v;
      const char *id = json_str(json_get(x, "id"), NULL), *cf = json_str(json_get(x, "configuration"), NULL);
      ELang *l;
      size_t k;
      if (id == NULL) continue;
      l = &e->lang[e->nlang++];
      memset(l, 0, sizeof(*l));
      l->id = xstrdup(id);
      v = json_get(x, "aliases");
      l->alias = xstrdup(count(v) && v->kid[0]->type == J_STR ? v->kid[0]->str : id);
      l->config = cf ? ext_file(dir, cf) : NULL;
      vec_init(&l->exts);
      vec_init(&l->names);
      v = json_get(x, "extensions");
      for (k = 0; k < count(v); k++)
        if (v->kid[k]->type == J_STR) vec_push(&l->exts, xstrdup(v->kid[k]->str));
      v = json_get(x, "filenames");
      for (k = 0; k < count(v); k++)
        if (v->kid[k]->type == J_STR) vec_push(&l->names, xstrdup(v->kid[k]->str));
    }
  }
  e->ngrammar = (int)count(json_get(c, "grammars"));
  e->ncommand = (int)count(json_get(c, "commands"));
  e->nkey = (int)count(json_get(c, "keybindings"));
  e->ndebug = (int)count(json_get(c, "debuggers"));
  a = json_get(c, "iconThemes");
  if (count(a)) {
    e->icons = (EIcon *)xmalloc(a->n * sizeof(EIcon));
    for (i = 0; i < a->n; i++) {
      const char *id = json_str(json_get(a->kid[i], "id"), NULL), *p = json_str(json_get(a->kid[i], "path"), NULL);
      if (id == NULL || p == NULL) continue;
      e->icons[e->nicon].id = xstrdup(id);
      e->icons[e->nicon].label = xstrdup(nls(nl, json_str(json_get(a->kid[i], "label"), id)));
      e->icons[e->nicon].path = ext_file(dir, p);
      e->nicon++;
    }
  }
  e->nconfig = json_get(c, "configuration") != NULL;
  e->code = json_get(pk, "main") != NULL || json_get(pk, "browser") != NULL;
  vec_init(&e->onlang);
  {
    const Json *ev = json_get(pk, "activationEvents");
    for (i = 0; i < count(ev); i++) {
      const char *s = json_str(ev->kid[i], "");
      if (strncmp(s, "onLanguage:", 11) == 0 && s[11]) vec_push(&e->onlang, xstrdup(s + 11));
    }
  }
  json_free(nl);
  json_free(pk);
  return 0;
}


/* "1.10.2" after "1.9.0"? */
static int newer (const char *a, const char *b) {
  while (*a || *b) {
    long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
    if (x != y) return x > y;
    while (*a && *a != '.') a++;
    while (*b && *b != '.') b++;
    if (*a == '.') a++;
    if (*b == '.') b++;
    if (*a == '\0' && *b == '\0') break;
  }
  return 0;
}


static int find_ext (const char *id) {
  size_t i;
  for (i = 0; i < g_next; i++)
    if (strcmp(g_ext[i].id, id) == 0) return (int)i;
  return -1;
}


/* the extensions found, for ehost.c: which of them run is its business */
size_t ext_count (void) {
  return g_next;
}


static unsigned g_gen;	/* one more at every scan */

unsigned ext_generation (void) {
  return g_gen;
}


const char *ext_id_at (size_t i) {
  return i < g_next ? g_ext[i].id : "";
}


const char *ext_dir_at (size_t i) {
  return i < g_next ? g_ext[i].dir : "";
}


int ext_is_vscode (size_t i) {
  return i < g_next && g_ext[i].vscode;
}


static Vec g_removing;	/* the folders an uninstall deletes (or could not delete all of): no extension is read there */

static int hidden (const char *dir) {
  size_t i;
  for (i = 0; i < g_removing.n; i++)
    if (strcmp(g_removing.v[i], dir) == 0) return 1;
  return 0;
}


static void unhide (const char *dir) {
  size_t i;
  for (i = 0; i < g_removing.n; i++)
    if (strcmp(g_removing.v[i], dir) == 0) {
      free(g_removing.v[i]);
      g_removing.v[i] = g_removing.v[--g_removing.n];
      return;
    }
}


/* every extension folder in root; VS Code's .obsolete ones left out */
static void scan_root (const char *root, int vscode) {
  Vec v;
  size_t i;
  char *f = path_join(root, ".obsolete");
  Json *obs = read_json(f);
  free(f);
  vec_init(&v);
  if (os_listdir(root, &v) == 0) {
    vec_sort(&v);
    for (i = 0; i < v.n; i++) {
      char *dir;
      Ext e;
      int at;
      if (v.v[i][0] == '.') continue;
      if (obs && obs->type == J_OBJ) {
        size_t k;
        int gone = 0;
        for (k = 0; k < obs->n && !gone; k++) gone = obs->kid[k]->key && strcmp(obs->kid[k]->key, v.v[i]) == 0;
        if (gone) continue;
      }
      dir = path_join(root, v.v[i]);
      if (hidden(dir)) {	/* being uninstalled: gone already, for everyone */
        free(dir);
        continue;
      }
      if (ext_read(&e, dir, vscode) == 0) {
        at = find_ext(e.id);
        if (at < 0) {
          if (g_next == g_capext) {
            g_capext = g_capext ? g_capext * 2 : 32;
            g_ext = (Ext *)xrealloc(g_ext, g_capext * sizeof(Ext));
          }
          g_ext[g_next++] = e;
        }
        else if (g_ext[at].vscode == vscode && newer(e.version, g_ext[at].version)) {	/* two versions: the newer */
          ext_free(&g_ext[at]);
          g_ext[at] = e;
        }
        else ext_free(&e);
      }
      free(dir);
    }
  }
  vec_free(&v);
  json_free(obs);
}


char *ext_dir (void) {
  char *d = data_path("extensions");
  mkdir_p(d);
  return d;
}


static int load_theme (void *arg, uint32_t *color);

/* the extensions read again: mme's, then VS Code's; their themes and snippets registered */
void ext_rescan (void) {
  size_t i, k;
  char *d = ext_dir();
  for (i = 0; i < g_next; i++) ext_free(&g_ext[i]);
  g_next = 0;
  g_gen++;
  scan_root(d, 0);
  free(d);
  if (opt.vscode_ext) {	/* VS Code's: ~/.vscode/extensions, a portable one's data/extensions (evscode.c) */
    Vec vd;
    vscode_ext_dirs(&vd);
    for (i = 0; i < vd.n; i++) scan_root(vd.v[i], 1);
    vec_free(&vd);
  }
  theme_clear_added();
  for (i = 0; i < g_next; i++)
    for (k = 0; k < g_ext[i].ntheme; k++)
      theme_add(g_ext[i].theme[k].label, g_ext[i].theme[k].light, load_theme, &g_ext[i].theme[k]);
  snip_reload();
  g_scanned = 1 + opt.vscode_ext;
}


/* after settings.json is read: the first time, or when useVSCodeExtensions changed */
void ext_init (void) {
  if (g_scanned != 1 + opt.vscode_ext) ext_rescan();
}


/* the snippet files extensions give lang (and every language) */
const char *const *ext_snippets (const char *lang, size_t *n) {
  static Vec v;
  size_t i, k;
  vec_free(&v);
  vec_init(&v);
  for (i = 0; i < g_next; i++)
    for (k = 0; k < g_ext[i].nsnip; k++)
      if (g_ext[i].snip[k].lang == NULL || (lang && strcmp(g_ext[i].snip[k].lang, lang) == 0))
        vec_push(&v, xstrdup(g_ext[i].snip[k].path));
  *n = v.n;
  return (const char *const *)v.v;
}


static ELang *lang_by_id (const char *id) {
  size_t i, k;
  for (i = 0; id && i < g_next; i++)
    for (k = 0; k < g_ext[i].nlang; k++)
      if (strcmp(g_ext[i].lang[k].id, id) == 0) return &g_ext[i].lang[k];
  return NULL;
}


/* the language an extension gives a file ("foo" for x.foo), or NULL */
const char *ext_lang_for (const char *path) {
  const char *base;
  size_t i, k, j, bl;
  if (path == NULL) return NULL;
  base = path_basename(path);
  bl = strlen(base);
  for (i = 0; i < g_next; i++)
    for (k = 0; k < g_ext[i].nlang; k++) {
      const ELang *l = &g_ext[i].lang[k];
      for (j = 0; j < l->names.n; j++)
        if (m_fncmp(l->names.v[j], base) == 0) return l->id;
      for (j = 0; j < l->exts.n; j++) {
        size_t el = strlen(l->exts.v[j]);
        if (el <= bl && m_stricmp(base + bl - el, l->exts.v[j]) == 0) return l->id;
      }
    }
  return NULL;
}


/* what the status bar calls it: "Foo" */
const char *ext_lang_name (const char *id) {
  const ELang *l = lang_by_id(id);
  return l ? l->alias : id;
}


/* a file's language for the status bar: "Plain Text" when no extension knows it */
const char *ext_lang_label (const char *path) {
  const char *id = ext_lang_for(path);
  return id ? ext_lang_name(id) : "Plain Text";
}


/* its comments, from its language-configuration.json; 0: none known */
/* the file of an extension's color theme, by its name; NULL: not one */
/* the installed extensions' file icon themes (eicons.c): i of ext_icon_theme_count */
static const EIcon *icon_theme (int i) {
  size_t k;
  for (k = 0; k < g_next; k++) {
    if (i < g_ext[k].nicon) return &g_ext[k].icons[i];
    i -= g_ext[k].nicon;
  }
  return NULL;
}

int ext_icon_theme_count (void) {
  size_t k;
  int n = 0;
  for (k = 0; k < g_next; k++) n += g_ext[k].nicon;
  return n;
}

const char *ext_icon_theme_id (int i) {
  const EIcon *t = icon_theme(i);
  return t ? t->id : "";
}

const char *ext_icon_theme_label (int i) {
  const EIcon *t = icon_theme(i);
  return t ? t->label : "";
}

/* the theme file of the icon theme with that id; NULL: none installed */
const char *ext_icon_theme_path (const char *id) {
  int i, n = ext_icon_theme_count();
  for (i = 0; i < n; i++)
    if (strcmp(icon_theme(i)->id, id) == 0) return icon_theme(i)->path;
  return NULL;
}


const char *ext_theme_path (const char *label) {
  size_t i, k;
  for (i = 0; label && i < g_next; i++)
    for (k = 0; k < g_ext[i].ntheme; k++)
      if (strcmp(g_ext[i].theme[k].label, label) == 0) return g_ext[i].theme[k].path;
  return NULL;
}


int ext_comment (const char *id, const char **line, const char **open, const char **close) {
  ELang *l = lang_by_id(id);
  if (l == NULL) return 0;
  if (!l->read) {
    Json *j = l->config ? read_json(l->config) : NULL;
    const Json *b = json_get(j, "comments.blockComment");
    const char *lc = json_str(json_get(j, "comments.lineComment"), NULL);
    l->read = 1;
    if (lc) l->line = xstrdup(lc);
    if (count(b) == 2 && b->kid[0]->type == J_STR && b->kid[1]->type == J_STR) {
      l->open = xstrdup(b->kid[0]->str);
      l->close = xstrdup(b->kid[1]->str);
    }
    json_free(j);
  }
  *line = l->line;
  *open = l->open;
  *close = l->close;
  return l->line || l->open;
}

/* }================================================================== */


/*
** {==================================================================
** Color themes: VS Code's colors and token colors on mme's slots
** ===================================================================
*/

static const struct {
  const char *key;
  int slot;
} colormap[] = {
  {"editor.foreground", C_EDITOR_FG}, {"editor.foreground", C_TOK + T_TEXT},
  {"editor.lineHighlightBackground", C_LINE_BG}, {"editor.selectionBackground", C_SEL_BG},
  {"editor.findMatchHighlightBackground", C_MATCH_BG}, {"editor.findMatchBackground", C_MATCH_BG},
  {"editorLineNumber.foreground", C_GUTTER}, {"editorLineNumber.activeForeground", C_GUTTER_ON},
  {"editorWhitespace.foreground", C_WS}, {"editorWhitespace.foreground", C_TOK + T_WS},
  {"editorWhitespace.foreground", C_CTRL},
  {"statusBar.background", C_STATUS_BG}, {"statusBar.foreground", C_STATUS_FG},
  {"statusBarItem.hoverBackground", C_STATUS_ITEM},
  {"titleBar.activeBackground", C_TITLE_BG}, {"titleBar.activeForeground", C_TITLE_FG},
  {"menubar.selectionBackground", C_TITLE_ON},
  {"menu.background", C_MENU_BG}, {"menu.foreground", C_MENU_FG},
  {"menu.selectionBackground", C_MENU_SEL_BG}, {"menu.selectionForeground", C_MENU_SEL_FG},
  {"menu.separatorBackground", C_MENU_SEP}, {"quickInput.background", C_MENU_BG},
  {"activityBar.background", C_ACT_BG}, {"activityBar.foreground", C_ACT_FG},
  {"activityBar.inactiveForeground", C_ACT_DIM},
  {"sideBar.background", C_SIDE_BG}, {"sideBar.foreground", C_SIDE_FG},
  {"sideBarTitle.foreground", C_SIDE_HEAD}, {"sideBarSectionHeader.foreground", C_SIDE_HEAD},
  {"list.activeSelectionBackground", C_LIST_SEL_BG}, {"list.activeSelectionForeground", C_LIST_SEL_FG},
  {"list.hoverBackground", C_LIST_CUR_BG}, {"list.inactiveSelectionBackground", C_LIST_CUR_BG},
  {"list.highlightForeground", C_HIT}, {"descriptionForeground", C_DIM},
  {"sideBar.border", C_BORDER}, {"editorGroup.border", C_BORDER}, {"panel.border", C_BORDER},
  {"input.background", C_INPUT_BG}, {"input.foreground", C_INPUT_FG},
  {"input.placeholderForeground", C_INPUT_HINT}, {"inputOption.activeBackground", C_TOGGLE_BG},
  {"notifications.background", C_TOAST_BG},
  {"editorError.foreground", C_ERROR}, {"editorWarning.foreground", C_WARNING},
  {"editorInfo.foreground", C_INFO},
  {"gitDecoration.modifiedResourceForeground", C_GIT_M}, {"gitDecoration.addedResourceForeground", C_GIT_A},
  {"gitDecoration.deletedResourceForeground", C_GIT_D}, {"gitDecoration.untrackedResourceForeground", C_GIT_U},
  {"diffEditor.insertedLineBackground", C_DIFF_ADD}, {"diffEditor.insertedTextBackground", C_DIFF_ADD_HI},
  {"diffEditor.removedLineBackground", C_DIFF_DEL}, {"diffEditor.removedTextBackground", C_DIFF_DEL_HI},
  {"editorGroupHeader.tabsBackground", C_TABS_BG}, {"tab.inactiveBackground", C_TAB_BG},
  {"tab.inactiveForeground", C_TAB_FG}, {"tab.activeBackground", C_TAB_ON_BG},
  {"tab.activeForeground", C_TAB_ON_FG}, {"breadcrumb.foreground", C_CRUMB},
  {"focusBorder", C_ACCENT}, {"activityBarBadge.background", C_ACCENT},
  {"editorCursor.foreground", C_MCURSOR}, {"editorLightBulb.foreground", C_LIGHTBULB},
  {"minimapSlider.background", C_MINIMAP_SLIDER}, {"scrollbarSlider.background", C_THUMB},
  {"scrollbarSlider.hoverBackground", C_THUMB_ON},
  {"editorBracketHighlight.foreground1", C_BRACKET1}, {"editorBracketHighlight.foreground2", C_BRACKET2},
  {"editorBracketHighlight.foreground3", C_BRACKET3}, {"editorBracketMatch.background", C_BRACKET_MATCH},
  {"editorIndentGuide.background", C_GUIDE}, {"editorIndentGuide.background1", C_GUIDE},
  {"editorIndentGuide.activeBackground", C_GUIDE_ON}, {"editorIndentGuide.activeBackground1", C_GUIDE_ON},
  {"editorRuler.foreground", C_RULER}, {"editorInlayHint.foreground", C_INLAY_FG},
  {"editorInlayHint.background", C_INLAY_BG}, {"editorCodeLens.foreground", C_LENS},
  {"editorGhostText.foreground", C_GHOST},
  {"panel.background", C_TERM_BG}, {"terminal.background", C_TERM_BG}, {"terminal.foreground", C_TERM_FG},
  {"terminal.ansiBlack", C_ANSI + 0}, {"terminal.ansiRed", C_ANSI + 1}, {"terminal.ansiGreen", C_ANSI + 2},
  {"terminal.ansiYellow", C_ANSI + 3}, {"terminal.ansiBlue", C_ANSI + 4}, {"terminal.ansiMagenta", C_ANSI + 5},
  {"terminal.ansiCyan", C_ANSI + 6}, {"terminal.ansiWhite", C_ANSI + 7},
  {"terminal.ansiBrightBlack", C_ANSI + 8}, {"terminal.ansiBrightRed", C_ANSI + 9},
  {"terminal.ansiBrightGreen", C_ANSI + 10}, {"terminal.ansiBrightYellow", C_ANSI + 11},
  {"terminal.ansiBrightBlue", C_ANSI + 12}, {"terminal.ansiBrightMagenta", C_ANSI + 13},
  {"terminal.ansiBrightCyan", C_ANSI + 14}, {"terminal.ansiBrightWhite", C_ANSI + 15}
};

/* a token's TextMate scopes, the most telling first: the rule with the longest match wins */
static const char *const tokscope[T_N][3] = {
  [T_KEYWORD] = {"keyword.control.flow", "keyword"},
  [T_STORAGE] = {"storage.type.built-in", "keyword"},
  [T_TYPE] = {"entity.name.type.class", "support.type", "support.class"},
  [T_FUNC] = {"entity.name.function", "support.function"},
  [T_STRING] = {"string.quoted.double"},
  [T_NUMBER] = {"constant.numeric.decimal", "constant"},
  [T_COMMENT] = {"comment.line.double-slash"},
  [T_VAR] = {"variable.other.readwrite", "variable"},
  [T_CONST] = {"variable.other.constant", "constant.language", "constant"},
  [T_ESCAPE] = {"constant.character.escape"},
  [T_HEADING] = {"markup.heading", "entity.name.section"}
};

/* the theme in use by VS Code's names, for an extension's webview: {"kind":1 light / 2 dark,"colors":{...}} */
void ext_theme_colors (Buf *r) {
  size_t m;
  uint32_t bg = ui_color(C_EDITOR_BG);
  int lum = (int)(((bg >> 16) & 255) * 299 + ((bg >> 8) & 255) * 587 + (bg & 255) * 114) / 1000;
  buf_printf(r, "{\"kind\":%d,\"colors\":{\"editor.background\":\"#%06x\"", lum > 128 ? 1 : 2, (unsigned)bg);
  for (m = 0; m < sizeof(colormap) / sizeof(colormap[0]); m++) {
    if (m > 0 && strcmp(colormap[m].key, colormap[m - 1].key) == 0) continue;	/* one key on several slots: the first */
    buf_printf(r, ",\"%s\":\"#%06x\"", colormap[m].key, (unsigned)ui_color(colormap[m].slot));
  }
  buf_puts(r, "}}");
}


typedef struct Load {
  unsigned char set[C_N];
  int score[T_N][3];	/* the best rule for each of a token's scopes */
  uint32_t tok[T_N][3];
  uint32_t gfg, gbg;	/* a rule without scope: the editor's */
  int has_gfg, has_gbg;
} Load;


static int hexv (int c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}


/* "#rgb", "#rgba", "#rrggbb", "#rrggbbaa": a translucent one over under */
static int parse_color (const char *s, uint32_t under, uint32_t *out) {
  unsigned v[8];
  size_t n, i;
  unsigned r, g, b, a = 255;
  if (s == NULL || s[0] != '#') return 0;
  s++;
  n = strlen(s);
  if (n != 3 && n != 4 && n != 6 && n != 8) return 0;
  for (i = 0; i < n; i++) {
    int h = hexv((unsigned char)s[i]);
    if (h < 0) return 0;
    v[i] = (unsigned)h;
  }
  if (n <= 4) {
    r = v[0] * 17;
    g = v[1] * 17;
    b = v[2] * 17;
    if (n == 4) a = v[3] * 17;
  }
  else {
    r = v[0] * 16 + v[1];
    g = v[2] * 16 + v[3];
    b = v[4] * 16 + v[5];
    if (n == 8) a = v[6] * 16 + v[7];
  }
  if (a < 255) {
    unsigned ur = under >> 16, ug = (under >> 8) & 255, ub = under & 255;
    r = (r * a + ur * (255 - a)) / 255;
    g = (g * a + ug * (255 - a)) / 255;
    b = (b * a + ub * (255 - a)) / 255;
  }
  *out = (uint32_t)((r << 16) | (g << 8) | b);
  return 1;
}


static uint32_t mix (uint32_t a, uint32_t b, unsigned pct) {	/* pct of b into a */
  unsigned r = ((a >> 16) * (100 - pct) + (b >> 16) * pct) / 100;
  unsigned g = (((a >> 8) & 255) * (100 - pct) + ((b >> 8) & 255) * pct) / 100;
  unsigned bl = ((a & 255) * (100 - pct) + (b & 255) * pct) / 100;
  return (uint32_t)((r << 16) | (g << 8) | bl);
}


/* how well rule selector sel (one, "a.b" or "x y.z") fits scope; -1 not */
static int selector_fits (const char *sel, size_t n, const char *scope) {
  const char *last = sel, *p;
  size_t ln;
  int deep = 0;
  for (p = sel; p < sel + n; p++)
    if (*p == ' ') {
      last = p + 1;
      deep = 1;
    }
  ln = (size_t)(sel + n - last);
  if (ln == 0 || memchr(sel, '-', n) == sel) return -1;
  if (strncmp(scope, last, ln) != 0 || (scope[ln] != '\0' && scope[ln] != '.')) return -1;
  return (int)ln * 2 - (deep ? 1 : 0);	/* a plain one before one inside something */
}


static void token_rule (Load *ld, const Json *rule) {
  const Json *sc = json_get(rule, "scope"), *st = json_get(rule, "settings");
  const char *fg = json_str(json_get(st, "foreground"), NULL);
  uint32_t rgb;
  int t, k;
  if (sc == NULL) {	/* the editor's colors, as TextMate themes give them */
    if (parse_color(fg, 0, &rgb)) {
      ld->gfg = rgb;
      ld->has_gfg = 1;
    }
    if (parse_color(json_str(json_get(st, "background"), NULL), 0, &rgb)) {
      ld->gbg = rgb;
      ld->has_gbg = 1;
    }
    return;
  }
  if (!parse_color(fg, ld->has_gbg ? ld->gbg : 0, &rgb)) return;
  for (t = 0; t < T_N; t++)
    for (k = 0; k < 3 && tokscope[t][k]; k++) {
      size_t i, nsel = sc->type == J_ARR ? sc->n : 1;
      for (i = 0; i < nsel; i++) {
        const char *s = sc->type == J_ARR ? json_str(sc->kid[i], "") : json_str(sc, "");
        while (*s) {	/* "a, b.c" */
          const char *e = strchr(s, ',');
          size_t n;
          int sco;
          while (*s == ' ') s++;
          n = e ? (size_t)(e - s) : strlen(s);
          while (n > 0 && s[n - 1] == ' ') n--;
          sco = selector_fits(s, n, tokscope[t][k]);
          if (sco >= 0 && sco + 1 >= ld->score[t][k]) {	/* later rules win a tie */
            ld->score[t][k] = sco + 1;
            ld->tok[t][k] = rgb;
          }
          if (e == NULL) break;
          s = e + 1;
        }
      }
    }
}


/* a theme file into color (its "include" first) */
static int theme_file (const char *path, uint32_t *color, Load *ld, int depth) {
  Json *j = read_json(path);
  const Json *cs, *tc;
  const char *inc;
  size_t i, m;
  uint32_t rgb;
  if (j == NULL) return -1;
  inc = json_str(json_get(j, "include"), NULL);
  if (inc && depth < 5) {
    char *dir = path_dirname(path), *f = ext_file(dir, inc);
    theme_file(f, color, ld, depth + 1);
    free(dir);
    free(f);
  }
  cs = json_get(j, "colors");
  if (cs && cs->type == J_OBJ) {
    if (parse_color(json_str(json_get(cs, "editor\\.background"), NULL), 0, &rgb)) {	/* first: the others go over it */
      color[C_EDITOR_BG] = rgb;
      ld->set[C_EDITOR_BG] = 1;
    }
    for (i = 0; i < cs->n; i++) {
      const Json *k = cs->kid[i];
      if (k->key == NULL || k->type != J_STR) continue;
      for (m = 0; m < sizeof(colormap) / sizeof(colormap[0]); m++)
        if (strcmp(colormap[m].key, k->key) == 0 && parse_color(k->str, color[C_EDITOR_BG], &rgb)) {
          color[colormap[m].slot] = rgb;
          ld->set[colormap[m].slot] = 1;
        }
    }
  }
  tc = json_get(j, "tokenColors");
  if (tc && tc->type == J_ARR)
    for (i = 0; i < tc->n; i++) token_rule(ld, tc->kid[i]);
  json_free(j);
  return 0;
}


/*
** The colors of one "colors" object onto the slots: the theme files and
** workbench.colorCustomizations both hold VS Code's keys.
*/
static void colors_onto (const Json *cs, uint32_t *color) {
  size_t i, m;
  uint32_t rgb;
  if (cs == NULL || cs->type != J_OBJ) return;
  if (parse_color(json_str(json_get(cs, "editor\\.background"), NULL), 0, &rgb))
    color[C_EDITOR_BG] = rgb;	/* first: a translucent color blends over it */
  for (i = 0; i < cs->n; i++) {
    const Json *k = cs->kid[i];
    if (k->key == NULL || k->type != J_STR) continue;
    for (m = 0; m < sizeof(colormap) / sizeof(colormap[0]); m++)
      if (strcmp(colormap[m].key, k->key) == 0 && parse_color(k->str, color[C_EDITOR_BG], &rgb))
        color[colormap[m].slot] = rgb;
  }
}


/* editor.tokenColorCustomizations' named groups, onto mme's token classes */
static const struct {
  const char *key;
  int tok;
} tokgroup[] = {
  {"comments", T_COMMENT}, {"strings", T_STRING}, {"keywords", T_KEYWORD},
  {"numbers", T_NUMBER}, {"types", T_TYPE}, {"functions", T_FUNC},
  {"variables", T_VAR}
};


/*
** One textMateRules entry onto the classes its scope names. The theme
** loader scores its rules because a theme holds hundreds that overlap;
** these are applied last and in order, so the later rule simply wins.
*/
static void tok_rule_onto (const Json *rule, uint32_t *color) {
  const Json *sc = json_get(rule, "scope"), *st = json_get(rule, "settings");
  uint32_t rgb;
  int t, k;
  if (sc == NULL || !parse_color(json_str(json_get(st, "foreground"), NULL),
                                 color[C_EDITOR_BG], &rgb)) return;
  for (t = 0; t < T_N; t++)
    for (k = 0; k < 3 && tokscope[t][k]; k++) {
      size_t i, nsel = sc->type == J_ARR ? sc->n : 1;
      for (i = 0; i < nsel; i++) {
        const char *s = sc->type == J_ARR ? json_str(sc->kid[i], "") : json_str(sc, "");
        while (*s) {	/* "a, b.c" */
          const char *e = strchr(s, ',');
          size_t n;
          while (*s == ' ') s++;
          n = e ? (size_t)(e - s) : strlen(s);
          while (n > 0 && s[n - 1] == ' ') n--;
          if (selector_fits(s, n, tokscope[t][k]) >= 0) color[C_TOK + t] = rgb;
          if (e == NULL) break;
          s = e + 1;
        }
      }
    }
}


/* the groups and the rules of one tokenColorCustomizations object */
static void tokens_onto (const Json *tc, uint32_t *color) {
  const Json *rules;
  size_t i;
  uint32_t rgb;
  if (tc == NULL || tc->type != J_OBJ) return;
  for (i = 0; i < sizeof(tokgroup) / sizeof(tokgroup[0]); i++) {
    const Json *v = json_get(tc, tokgroup[i].key);
    if (v == NULL) continue;	/* a group may be a color, or {"foreground": ...} */
    if (parse_color(v->type == J_OBJ ? json_str(json_get(v, "foreground"), NULL)
                                     : json_str(v, NULL), color[C_EDITOR_BG], &rgb))
      color[C_TOK + tokgroup[i].tok] = rgb;
  }
  rules = json_get(tc, "textMateRules");
  if (rules != NULL && rules->type == J_ARR)
    for (i = 0; i < rules->n; i++) tok_rule_onto(rules->kid[i], color);
}


/* "[Dark+]" or "[Dark+][Monokai]": does the key name this theme? "*" is all */
static int scoped_to (const char *key, const char *theme) {
  const char *p = key;
  while (*p == '[') {
    const char *e = strchr(p, ']');
    size_t n;
    if (e == NULL) return 0;
    n = (size_t)(e - p) - 1;
    if (n == 1 && p[1] == '*') return 1;
    if (theme != NULL && n == strlen(theme) && m_strnicmp(p + 1, theme, n) == 0) return 1;
    p = e + 1;
  }
  return 0;
}


/*
** workbench.colorCustomizations over the theme's own colors: the ones for
** every theme first, then the ones named for this one, which win.
*/
void theme_customize (uint32_t *color, const char *theme) {
  const Json *cc = settings_get("workbench\\.colorCustomizations");
  const Json *tc = settings_get("editor\\.tokenColorCustomizations");
  size_t i;
  if (cc != NULL && cc->type == J_OBJ) {
    colors_onto(cc, color);
    for (i = 0; i < cc->n; i++) {	/* then this theme's own, which win */
      const Json *k = cc->kid[i];
      if (k->key != NULL && k->type == J_OBJ && k->key[0] == '[' && scoped_to(k->key, theme))
        colors_onto(k, color);
    }
  }
  if (tc != NULL && tc->type == J_OBJ) {	/* the code's colors, the same way */
    tokens_onto(tc, color);
    for (i = 0; i < tc->n; i++) {
      const Json *k = tc->kid[i];
      if (k->key != NULL && k->type == J_OBJ && k->key[0] == '[' && scoped_to(k->key, theme))
        tokens_onto(k, color);
    }
  }
}


/* a registered theme of an extension: its colors over the base mme has */
static int load_theme (void *arg, uint32_t *color) {
  const ETheme *th = (const ETheme *)arg;
  Load *ld = (Load *)calloc(1, sizeof(Load));
  int t, k;
  if (ld == NULL) return -1;
  if (theme_file(th->path, color, ld, 0) != 0) {
    free(ld);
    toast(1, "Could not read the theme %s", path_basename(th->path));
    return -1;
  }
  if (!ld->set[C_EDITOR_BG] && ld->has_gbg) color[C_EDITOR_BG] = ld->gbg, ld->set[C_EDITOR_BG] = 1;
  if (!ld->set[C_EDITOR_FG] && ld->has_gfg) {
    color[C_EDITOR_FG] = color[C_TOK + T_TEXT] = ld->gfg;
    ld->set[C_EDITOR_FG] = 1;
  }
  if (ld->set[C_EDITOR_BG]) {	/* what VS Code takes from the editor when a theme says nothing */
    uint32_t bg = color[C_EDITOR_BG], fg = color[C_EDITOR_FG];
    if (!ld->set[C_LINE_BG]) color[C_LINE_BG] = mix(bg, fg, 6);
    if (!ld->set[C_TAB_ON_BG]) color[C_TAB_ON_BG] = bg;
    if (!ld->set[C_TERM_BG]) color[C_TERM_BG] = bg;
    if (!ld->set[C_TERM_FG]) color[C_TERM_FG] = fg;
    if (!ld->set[C_SIDE_BG]) color[C_SIDE_BG] = mix(bg, 0, 12);
    if (!ld->set[C_TABS_BG]) color[C_TABS_BG] = color[C_SIDE_BG];
    if (!ld->set[C_TAB_BG]) color[C_TAB_BG] = color[C_TABS_BG];
    if (!ld->set[C_ACT_BG]) color[C_ACT_BG] = color[C_SIDE_BG];
    if (!ld->set[C_TITLE_BG]) color[C_TITLE_BG] = color[C_SIDE_BG];
    if (!ld->set[C_MENU_BG]) color[C_MENU_BG] = color[C_SIDE_BG];
    if (!ld->set[C_TOAST_BG]) color[C_TOAST_BG] = color[C_MENU_BG];
    if (!ld->set[C_STATUS_BG]) color[C_STATUS_BG] = color[C_SIDE_BG];
    if (!ld->set[C_INPUT_BG]) color[C_INPUT_BG] = mix(bg, fg, 10);
    if (!ld->set[C_GUIDE]) color[C_GUIDE] = mix(bg, fg, 16);
    if (!ld->set[C_GUIDE_ON]) color[C_GUIDE_ON] = mix(bg, fg, 40);
    if (!ld->set[C_RULER]) color[C_RULER] = mix(bg, fg, 22);
    if (!ld->set[C_INLAY_BG]) color[C_INLAY_BG] = mix(bg, fg, 6);
    if (!ld->set[C_INLAY_FG]) color[C_INLAY_FG] = mix(bg, fg, 55);
    if (!ld->set[C_LENS]) color[C_LENS] = mix(bg, fg, 55);
    if (!ld->set[C_GHOST]) color[C_GHOST] = mix(bg, fg, 42);
  }
  if (!ld->set[C_STATUS_ITEM]) color[C_STATUS_ITEM] = mix(color[C_STATUS_BG], color[C_STATUS_FG], 12);
  if (!ld->set[C_TITLE_ON]) color[C_TITLE_ON] = mix(color[C_TITLE_BG], color[C_TITLE_FG], 12);
  if (!ld->set[C_TOK + T_TEXT]) color[C_TOK + T_TEXT] = color[C_EDITOR_FG];
  for (t = 0; t < T_N; t++)
    for (k = 0; k < 3 && tokscope[t][k]; k++)
      if (ld->score[t][k] > 0) {
        color[C_TOK + t] = ld->tok[t][k];
        break;
      }
  free(ld);
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Programs in the background: tar (or unzip)
** ===================================================================
*/

enum { JOB_NONE, JOB_EXTRACT, JOB_REMOVE };

typedef struct Job {
  int kind;
  OsProc proc;
  int fd;	/* its output, -1: not read */
  Buf out;
  char *id;	/* the extension it is for */
  char *file, *dir;	/* the .vsix, the folder it goes to */
  int keep;	/* the .vsix is the user's, not ours to delete */
} Job;

static Job g_inst;	/* an install */


static char *program (const char *name) {
#ifdef _WIN32
  if (strcmp(name, "tar") == 0) {	/* Windows' own (bsdtar reads zips; git's tar does not) */
    char *root = os_getenv("SystemRoot"), *sys, *p, *exe;
    if (root) {
      sys = path_join(root, "System32");
      exe = xstrcat3(name, ".exe", "");
      p = path_join(sys, exe);
      free(root);
      free(sys);
      free(exe);
      if (os_is_exec(p)) return p;
      free(p);
    }
  }
#endif
  return find_program(name);
}


/* argv started; its output kept when capture */
static int start (Job *j, int kind, char **argv, int capture) {
  int fds[2] = {-1, -1}, io[3], null;
  long pid;
#ifdef _WIN32
  null = os_open("NUL", OS_WRITE);
#else
  null = os_open("/dev/null", OS_WRITE);
#endif
  if (capture && os_pipe(fds) != 0) {
    if (null >= 0) os_close(null);
    return -1;
  }
  io[0] = -1;
  io[1] = capture ? fds[1] : null;
  io[2] = null;
  if (os_spawn(argv[0], argv, NULL, io, 3, &j->proc, &pid) != 0) {
    if (capture) {
      os_close(fds[0]);
      os_close(fds[1]);
    }
    if (null >= 0) os_close(null);
    return -1;
  }
  if (capture) os_close(fds[1]);
  if (null >= 0) os_close(null);
  j->fd = capture ? fds[0] : -1;
  j->kind = kind;
  buf_init(&j->out);
  return 0;
}


/* 1 when it finished (*code its exit code), 0 running */
static int poll_job (Job *j, int *code) {
  if (j->kind == JOB_NONE) return 0;
  if (j->fd >= 0) {
    char chunk[16384];
    while (os_wait_readable(j->fd, 0) == 1) {
      long n = os_read(j->fd, chunk, sizeof(chunk));
      if (n <= 0) {
        os_close(j->fd);
        j->fd = -1;
        *code = os_wait(j->proc);
        return 1;
      }
      buf_putn(&j->out, chunk, (size_t)n);
    }
    return 0;
  }
  return os_poll_proc(j->proc, code) == 1;
}


static void end_job (Job *j) {
  buf_free(&j->out);
  free(j->id);
  free(j->file);
  free(j->dir);
  memset(j, 0, sizeof(*j));
  j->fd = -1;
}


/* rm -rf, as the system does it: its words in argv (8 at most); the program, to free; NULL: none */
static char *remove_argv (char **argv, const char *dir) {
  int n = 0;
  char *exe;
#ifdef _WIN32
  exe = os_getenv("ComSpec");
  if (exe == NULL) exe = xstrdup("cmd.exe");
  argv[n++] = exe;
  argv[n++] = (char *)"/c";
  argv[n++] = (char *)"rmdir";
  argv[n++] = (char *)"/s";
  argv[n++] = (char *)"/q";
#else
  exe = find_program("rm");
  if (exe == NULL) return NULL;
  argv[n++] = exe;
  argv[n++] = (char *)"-rf";
#endif
  argv[n++] = (char *)dir;
  argv[n] = NULL;
  return exe;
}


static void remove_tree (const char *dir) {
  char *argv[8], *exe = remove_argv(argv, dir);
  Buf out;
  if (exe == NULL) return;
  buf_init(&out);
  run_capture(argv, &out);
  buf_free(&out);
  free(exe);
}


static int is_dir (const char *p) {
  OsStat st;
  return os_stat(p, &st) == 0 && st.exists && st.is_dir;
}

/* }================================================================== */


/*
** {==================================================================
** Installing and uninstalling
** ===================================================================
*/

static void rows (void);

/* the .vsix file taken apart into a folder under extensions/.tmp */
static int extract (const char *vsix, const char *id, int keep) {
  char *ed = ext_dir(), *tmp = path_join(ed, ".tmp"), *dir, *exe, *argv[10];
  int n = 0, r;
  char name[160];
  mkdir_p(tmp);
  snprintf(name, sizeof(name), "%s", id);
  dir = path_join(tmp, name);
  remove_tree(dir);
  mkdir_p(dir);
  free(ed);
  free(tmp);
#ifdef _WIN32
  exe = program("tar");
#else
  exe = find_program("unzip");
  if (exe == NULL) exe = find_program("bsdtar");
  if (exe == NULL) exe = find_program("tar");
#endif
  if (exe == NULL) {
    toast(1, "No tar or unzip to take the .vsix apart");
    free(dir);
    return -1;
  }
  argv[n++] = exe;
  if (strstr(path_basename(exe), "unzip")) {
    argv[n++] = (char *)"-q";
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)vsix;
    argv[n++] = (char *)"-d";
    argv[n++] = dir;
  }
  else {
    argv[n++] = (char *)"-xf";
    argv[n++] = (char *)vsix;
    argv[n++] = (char *)"-C";
    argv[n++] = dir;
  }
  argv[n] = NULL;
  r = start(&g_inst, JOB_EXTRACT, argv, 0);
  free(exe);
  if (r != 0) {
    toast(1, "Could not take the .vsix apart");
    free(dir);
    return -1;
  }
  g_inst.id = xstrdup(id);
  g_inst.file = xstrdup(vsix);
  g_inst.dir = dir;
  g_inst.keep = keep;
  return 0;
}


/* the folder taken apart becomes <publisher>.<name>-<version> */
static void place (void) {
  char *src = path_join(g_inst.dir, "extension"), *ed = ext_dir(), *final;
  Ext e;
  char name[300];
  size_t i;
  if (ext_read(&e, src, 0) != 0) {
    toast(1, "This .vsix has no extension/package.json");
    free(src);
    free(ed);
    return;
  }
  snprintf(name, sizeof(name), "%s-%s", e.id, e.version);
  final = path_join(ed, name);
  for (i = 0; i < g_next; i++)	/* an older one of it goes */
    if (!g_ext[i].vscode && strcmp(g_ext[i].id, e.id) == 0) remove_tree(g_ext[i].dir);
  if (is_dir(final)) remove_tree(final);
  if (rename(src, final) != 0) toast(1, "Could not move the extension into %s", final);
  else {
    int themes = e.ntheme > 0;
    char *disp = xstrdup(e.display);
    unhide(final);	/* installed again where an uninstall could not delete it all */
    ext_rescan();
    rows();
    if (themes) toast(0, "Installed %s: Ctrl+K Ctrl+T picks its color theme", disp);
    else toast(0, "Installed %s", disp);
    out_log("Extensions", "[info] Installed %s into %s", disp, final);
    free(disp);
  }
  ext_free(&e);
  free(final);
  free(src);
  free(ed);
}


/* Extensions: Install from VSIX... */
void ext_install_vsix (const char *path) {
  char id[128];
  const char *base = path_basename(path);
  size_t n = strlen(base);
  if (g_inst.kind != JOB_NONE) {
    toast(0, g_inst.kind == JOB_REMOVE ? "An extension is being uninstalled" : "Another extension is being installed");
    return;
  }
  if (n < 6 || m_stricmp(base + n - 5, ".vsix") != 0) {
    toast(1, "Not a .vsix file: %s", base);
    return;
  }
  snprintf(id, sizeof(id), "%.*s", (int)(n - 5), base);
  if (extract(path, id, 1) == 0) toast(0, "Installing %s...", base);
}


static void set_lang_servers (const Ext *e, int on);

/* its folder deleted in the background; 0: started (g_inst), else nothing started */
static int remove_start (const char *dir, const char *disp) {
  char *argv[8], *exe = remove_argv(argv, dir);
  int r = exe != NULL ? start(&g_inst, JOB_REMOVE, argv, 0) : -1;	/* rmdir of a big one takes a while: not here */
  free(exe);
  if (r != 0) return -1;
  g_inst.dir = xstrdup(dir);
  g_inst.file = xstrdup(disp);
  toast(0, "Uninstalling %s...", disp);
  return 0;
}


static void removed (const char *dir, const char *disp, int code);

static void remove_retry (void *ud, int choice) {	/* "Retry" on the failure's notification */
  char *dir = (char *)ud, *disp = strchr(dir, '\n');
  *disp++ = '\0';
  if (choice == 0) {
    if (g_inst.kind != JOB_NONE) toast(0, "Wait for the extension being installed or uninstalled");
    else if (remove_start(dir, disp) != 0) {
      remove_tree(dir);
      removed(dir, disp, 0);
    }
  }
  free(dir);
}


/* the folder of an uninstall is gone, or not (a file of it in use): said; what is left stays hidden, so
** the host does not run half an extension, until Retry deletes it (or mme starts again and shows it) */
static void removed (const char *dir, const char *disp, int code) {
  if (is_dir(dir)) {	/* rmdir says 0 even when it could not delete everything */
    static const char *const act[] = {"Retry"};
    char msg[400];
    snprintf(msg, sizeof(msg), "Could not uninstall %s completely: some of its files are in use. Close what "
             "uses them, then retry.", disp);
    out_log("Extensions", "[error] Could not delete all of %s (exit code %d)", dir, code);
    toast_ask(2, "Extensions", msg, act, 1, remove_retry, xstrcat3(dir, "\n", disp));
  }
  else {
    unhide(dir);
    toast(0, "Uninstalled %s", disp);
    out_log("Extensions", "[info] Uninstalled %s", disp);
  }
  ext_rescan();
  rows();
}


/*
** Uninstall: its languages go back to mme's own servers; the extension
** host lets it go first (its deactivate runs, and its .node, .exe files are
** no longer in use), then its folder is deleted in the background, and
** whether that worked is checked.
*/
static void uninstall (int i) {
  char *dir, *id, *disp;
  int hosted;
  if (i < 0 || (size_t)i >= g_next) return;
  if (g_ext[i].vscode) {
    toast(0, "%s is VS Code's: uninstall it in VS Code", g_ext[i].display);
    return;
  }
  if (g_inst.kind != JOB_NONE) {
    toast(0, "Wait for the extension being installed or uninstalled");
    return;
  }
  dir = xstrdup(g_ext[i].dir);
  id = xstrdup(g_ext[i].id);
  disp = xstrdup(g_ext[i].display);
  hosted = lsp_running(EXT_LANG) && ehost_state(id) != NULL;
  set_lang_servers(&g_ext[i], 1);	/* mme's own servers of its languages back, as Enable gives them */
  if (!hidden(dir)) vec_push(&g_removing, xstrdup(dir));
  ext_rescan();	/* not there any more, for the host too */
  if (hosted) ehost_restart();	/* the host without it: the old one ends (its deactivate) before its files go */
  rows();
  if (remove_start(dir, disp) != 0) {	/* could not start it by itself: here, then */
    remove_tree(dir);
    removed(dir, disp, 0);
  }
  free(dir);
  free(id);
  free(disp);
}

/* }================================================================== */


/*
** {==================================================================
** The view: a box that filters the installed ones, then they
** ===================================================================
*/

typedef struct Item {
  int ext;	/* g_ext's index */
  char *id, *ns, *name, *display, *publisher, *desc, *version;
} Item;

static Item *g_item;
static size_t g_nitem, g_capitem;
static char g_q[256];	/* the filter */
static size_t g_sel, g_top;
static int g_h = 1;
static int g_bx0 = -1, g_bx1 = -1;	/* the button's columns in a row, from the view's left */

#define HEAD	4	/* "EXTENSIONS", the filter, the section, Install from VSIX... */
#define VSIX_ROW	3	/* the row of Install from VSIX... */
#define ROWS	3	/* each extension's */


static void item_free (Item *it) {
  free(it->id);
  free(it->ns);
  free(it->name);
  free(it->display);
  free(it->publisher);
  free(it->desc);
  free(it->version);
}


static void add_item (Item **v, size_t *n, size_t *cap, const Item *it) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 32;
    *v = (Item *)xrealloc(*v, *cap * sizeof(Item));
  }
  (*v)[(*n)++] = *it;
}


static Item dup_item (const Item *s) {
  Item d;
  d.ext = s->ext;
  d.id = xstrdup(s->id ? s->id : "");
  d.ns = xstrdup(s->ns ? s->ns : "");
  d.name = xstrdup(s->name ? s->name : "");
  d.display = xstrdup(s->display ? s->display : "");
  d.publisher = xstrdup(s->publisher ? s->publisher : "");
  d.desc = xstrdup(s->desc ? s->desc : "");
  d.version = xstrdup(s->version ? s->version : "");
  return d;
}


/* is w in s, whatever the case? */
static int has_word (const char *s, const char *w) {
  size_t n = strlen(w);
  for (; s && *s; s++)
    if (m_strnicmp(s, w, n) == 0) return 1;
  return 0;
}


/* does extension e have every word of the filter in its names or its description? */
static int passes (const Ext *e) {
  const char *q = g_q;
  while (*q) {
    char w[64];
    size_t n = 0;
    while (*q == ' ') q++;
    while (*q && *q != ' ' && n + 1 < sizeof(w)) w[n++] = *q++;
    w[n] = '\0';
    if (n == 0) break;
    if (!has_word(e->display, w) && !has_word(e->name, w) && !has_word(e->publisher, w) && !has_word(e->id, w) &&
        !has_word(e->desc, w))
      return 0;
  }
  return 1;
}


/* the list shown: the installed ones the filter lets through */
static void rows (void) {
  size_t i;
  for (i = 0; i < g_nitem; i++) item_free(&g_item[i]);
  g_nitem = 0;
  for (i = 0; i < g_next; i++) {
    Item it;
    if (!passes(&g_ext[i])) continue;
    memset(&it, 0, sizeof(it));
    it.ext = (int)i;
    it.id = g_ext[i].id;
    it.name = g_ext[i].name;
    it.ns = g_ext[i].publisher;
    it.display = g_ext[i].display;
    it.publisher = g_ext[i].publisher;
    it.desc = g_ext[i].desc;
    it.version = g_ext[i].version;
    it = dup_item(&it);
    add_item(&g_item, &g_nitem, &g_capitem, &it);
  }
  if (g_sel >= g_nitem) g_sel = g_nitem ? g_nitem - 1 : 0;
}

/* }================================================================== */


/*
** {==================================================================
** An extension's page, in an editor tab
** ===================================================================
*/

static char *page_path (const char *id) {
  char *ed = ext_dir(), *pd = path_join(ed, ".pages"), *f, name[200];
  mkdir_p(pd);
  snprintf(name, sizeof(name), "%s.md", id);
  f = path_join(pd, name);
  free(ed);
  free(pd);
  return f;
}


/* a JSON value, short, for a setting's default */
static void short_json (Buf *b, const Json *v) {
  Buf t;
  buf_init(&t);
  if (v) json_write(&t, v);
  else buf_puts(&t, "(none)");
  if (t.len > 60) buf_printf(b, "%.57s...", t.s);
  else buf_putn(b, t.s, t.len);
  buf_free(&t);
}


/* one line of text for a list: its first line, no more than n bytes */
static void one_line (Buf *b, const char *s, size_t n) {
  size_t k = strcspn(s, "\r\n");
  if (k > n) k = n;
  buf_putn(b, s, k);
  if (s[k] && k == n) buf_puts(b, "...");
}


/* while it runs: what the host knows (its activation, its log, the APIs it asked for that mme lacks) */
static void page_runtime (Buf *b, const Ext *e) {
  Json *in;
  const Json *a;
  const char *hs = ehost_state(e->id);
  size_t i;
  buf_puts(b, "\n## Status\n\n");
  if (!e->code) {
    buf_puts(b, "- no code of its own: mme reads what it contributes, nothing runs\n");
    return;
  }
  if (ehost_disabled(e->id)) buf_puts(b, "- disabled (mme.extensions.disabled): its code does not run, its languages are off\n");
  else if (e->vscode && !ehost_in_run(e->id) && hs == NULL)
    buf_puts(b, "- not enabled: VS Code's extensions run when named in mme.extensions.run (the Enable button)\n");
  else if (hs == NULL || strcmp(hs, "starting") == 0) buf_puts(b, "- enabled: it starts when one of its activation events comes\n");
  else buf_printf(b, "- %s\n", strcmp(hs, "error") == 0 ? "failed to start" : hs);
  in = hs ? ehost_ext_info(e->id) : NULL;
  if (in == NULL || !json_bool(json_get(in, "known"), 0)) {
    if (hs) buf_puts(b, "- the extension host did not answer (Output > Extension Host has its log)\n");
    json_free(in);
    return;
  }
  if (json_str(json_get(in, "error"), "")[0]) buf_printf(b, "- error: %s\n", json_str(json_get(in, "error"), ""));
  if (json_str(json_get(in, "event"), "")[0]) buf_printf(b, "- started on `%s`\n", json_str(json_get(in, "event"), ""));
  if (json_get(in, "activateMs")) {
    buf_printf(b, "- activation: %.0f ms", json_num(json_get(in, "activateMs"), 0));
    if (json_get(in, "busyMs")) buf_printf(b, ", of which the host was busy %.0f ms (loading its code, running activate())",
                                           json_num(json_get(in, "busyMs"), 0));
    if (json_get(in, "heapMB")) buf_printf(b, "; about %.0f MB of memory then", json_num(json_get(in, "heapMB"), 0));
    buf_putc(b, '\n');
  }
  a = json_get(in, "needs");
  for (i = 0; a && a->type == J_ARR && i < a->n; i++)
    buf_printf(b, "- needs the extension %s, which is not installed\n", json_str(a->kid[i], ""));
  a = json_get(in, "missing");
  if (count(a)) {
    buf_puts(b, "\n### VS Code APIs it asked for that mme does not have\n\n");
    for (i = 0; i < a->n; i++) buf_printf(b, "- `%s`\n", json_str(a->kid[i], ""));
  }
  a = json_get(in, "lines");
  if (count(a)) {
    buf_puts(b, "\n### Its log (the extension host's lines about it)\n\n```\n");
    for (i = 0; i < a->n; i++) {
      one_line(b, json_str(a->kid[i], ""), 300);
      buf_putc(b, '\n');
    }
    buf_puts(b, "```\n");
  }
  a = json_get(in, "events");
  if (count(a) || json_num(json_get(in, "implied"), 0) > 0) {
    buf_puts(b, "\n### When it starts (activation events)\n\n");
    for (i = 0; i < count(a) && i < 40; i++) buf_printf(b, "%s`%s`", i ? ", " : "", json_str(a->kid[i], ""));
    if (count(a) > 40) buf_printf(b, " and %lu more", (unsigned long)(a->n - 40));
    if (json_num(json_get(in, "implied"), 0) > 0)
      buf_printf(b, "%s%.0f implied by what it contributes (its commands, languages, views)", count(a) ? "; and " : "",
                 json_num(json_get(in, "implied"), 0));
    buf_putc(b, '\n');
  }
  json_free(in);
}


/* what its package.json contributes: commands, keys, settings, what it depends on */
static void page_contributes (Buf *b, const Ext *e) {
  char *f = path_join(e->dir, "package.json");
  Json *pk = read_json(f), *nl;
  const Json *c, *a;
  size_t i;
  free(f);
  if (pk == NULL) return;
  f = path_join(e->dir, "package.nls.json");
  nl = read_json(f);
  free(f);
  c = json_get(pk, "contributes");
  a = json_get(pk, "extensionDependencies");
  if (count(a)) {
    buf_puts(b, "\n## Depends on\n\n");
    for (i = 0; i < a->n; i++) {
      const char *d = json_str(a->kid[i], "");
      char *dir = strncmp(d, "vscode.", 7) == 0 ? NULL : vscode_find_ext(d);
      buf_printf(b, "- %s%s\n", d, strncmp(d, "vscode.", 7) == 0 ? " (VS Code's own)" : dir ? "" : " **not installed**");
      free(dir);
    }
  }
  a = json_get(pk, "extensionPack");
  if (count(a)) {
    buf_puts(b, "\n## Extension pack\n\n");
    for (i = 0; i < a->n; i++) buf_printf(b, "- %s\n", json_str(a->kid[i], ""));
  }
  a = json_get(c, "commands");
  if (count(a)) {
    buf_printf(b, "\n## Commands (%lu)\n\n", (unsigned long)a->n);
    for (i = 0; i < a->n; i++) {
      const Json *t = json_get(a->kid[i], "title"), *cat = json_get(a->kid[i], "category");
      const char *title = t && t->type == J_OBJ ? json_str(json_get(t, "value"), "") : nls(nl, json_str(t, ""));
      const char *ct = cat && cat->type == J_OBJ ? json_str(json_get(cat, "value"), "") : nls(nl, json_str(cat, ""));
      buf_printf(b, "- %s%s%s  `%s`\n", ct && *ct ? ct : "", ct && *ct ? ": " : "", title ? title : "",
                 json_str(json_get(a->kid[i], "command"), ""));
    }
  }
  if (count(json_get(c, "keybindings"))) {
    Buf k;
    buf_init(&k);
    ehost_key_report(e->id, &k);	/* the running host's: the keys worked out for this OS, mme's taken marked */
    buf_printf(b, "\n## Keybindings (%lu)\n\n", (unsigned long)json_get(c, "keybindings")->n);
    if (k.len) buf_putn(b, k.s, k.len);
    else {
      a = json_get(c, "keybindings");
      for (i = 0; i < a->n; i++) {
        const Json *x = a->kid[i];
#ifdef _WIN32
        const char *key = json_str(json_get(x, "win"), json_str(json_get(x, "key"), ""));
#elif defined(__APPLE__)
        const char *key = json_str(json_get(x, "mac"), json_str(json_get(x, "key"), ""));
#else
        const char *key = json_str(json_get(x, "linux"), json_str(json_get(x, "key"), ""));
#endif
        buf_printf(b, "- `%s`: %s\n", key, json_str(json_get(x, "command"), ""));
      }
      buf_puts(b, "\n(while it runs, a key that takes one of mme's is marked here)\n");
    }
    buf_free(&k);
  }
  {
    const Json *cf = json_get(c, "configuration");
    size_t n = 0, j;
    for (j = 0; cf && j < (cf->type == J_ARR ? cf->n : 1); j++) n += json_get(cf->type == J_ARR ? cf->kid[j] : cf, "properties") ?
                                                                      json_get(cf->type == J_ARR ? cf->kid[j] : cf, "properties")->n : 0;
    if (n) {
      buf_printf(b, "\n## Settings (%lu, in the Settings editor: Ctrl+,)\n\n", (unsigned long)n);
      for (j = 0; cf && j < (cf->type == J_ARR ? cf->n : 1); j++) {
        const Json *ps = json_get(cf->type == J_ARR ? cf->kid[j] : cf, "properties");
        for (i = 0; ps && ps->type == J_OBJ && i < ps->n; i++) {
          const Json *p = ps->kid[i];
          const char *d = nls(nl, json_str(json_get(p, "markdownDescription"), json_str(json_get(p, "description"), "")));
          buf_printf(b, "- `%s` = ", p->key ? p->key : "");
          short_json(b, json_get(p, "default"));
          if (d && *d) {
            buf_puts(b, ": ");
            one_line(b, d, 140);
          }
          buf_putc(b, '\n');
        }
      }
    }
  }
  a = json_get(c, "debuggers");
  if (count(a)) {
    buf_puts(b, "\n## Debuggers\n\n");
    for (i = 0; i < a->n; i++)
      buf_printf(b, "- %s (`%s`)\n", nls(nl, json_str(json_get(a->kid[i], "label"), "")), json_str(json_get(a->kid[i], "type"), ""));
  }
  json_free(nl);
  json_free(pk);
}


static void page_write (const Item *it, const Ext *e, const char *readme, size_t rlen) {
  Buf b;
  char *f = page_path(it->id);
  int fd;
  size_t i;
  buf_init(&b);
  buf_printf(&b, "# %s\n\n", it->display);
  buf_printf(&b, "%s  |  %s  |  v%s", it->publisher, it->id, it->version);
  buf_printf(&b, "\n\n%s\n\n", it->desc);
  if (e) {
    buf_printf(&b, "Installed in %s%s\n", e->dir, e->vscode ? " (VS Code's, read only)" : "");
    page_runtime(&b, e);
    if (e->ntheme + e->nsnip + e->nlang + e->ngrammar + e->nicon) buf_puts(&b, "\n## What mme reads from it\n\n");
    for (i = 0; i < e->ntheme; i++) buf_printf(&b, "- Color theme: %s (Ctrl+K Ctrl+T)\n", e->theme[i].label);
    for (i = 0; i < e->nsnip; i++) buf_printf(&b, "- Snippets: %s\n", e->snip[i].lang ? e->snip[i].lang : "every language");
    for (i = 0; i < e->nlang; i++) {
      size_t k;
      buf_printf(&b, "- Language: %s", e->lang[i].alias);
      for (k = 0; k < e->lang[i].exts.n; k++) buf_printf(&b, "%s%s", k ? " " : " (", e->lang[i].exts.v[k]);
      buf_puts(&b, e->lang[i].exts.n ? ")\n" : "\n");
    }
    if (e->ngrammar) buf_printf(&b, "- %d grammar(s): mme highlights with its own\n", e->ngrammar);
    for (i = 0; i < (size_t)e->nicon; i++)
      buf_printf(&b, "- File icon theme: %s (Preferences: File Icon Theme; workbench.iconTheme \"%s\")\n", e->icons[i].label,
                 e->icons[i].id);
    page_contributes(&b, e);
  }
  if (readme && rlen) {
    buf_puts(&b, "\n---\n\n");
    buf_putn(&b, readme, rlen);
    if (readme[rlen - 1] != '\n') buf_putc(&b, '\n');
  }
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
    on_ext_page(f);
  }
  buf_free(&b);
  free(f);
}


/* its page, from its package.json and its README.md */
static void show_page (const Item *it) {
  const Ext *e = &g_ext[it->ext];
  char *f = path_join(e->dir, "README.md");
  size_t len = 0;
  char *s = read_file(f, &len);
  Item x = *it;
  x.version = e->version;
  page_write(&x, e, s, len);
  free(s);
  free(f);
}

/* }================================================================== */


/*
** {==================================================================
** Idle: the programs' answers
** ===================================================================
*/

int ext_idle (void) {
  int code, changed = 0;
  if (poll_job(&g_inst, &code)) {
    changed = 1;
    if (g_inst.kind == JOB_EXTRACT) {
      if (code != 0) {
        toast(1, "Could not take %s apart", path_basename(g_inst.file));
        out_log("Extensions", "[error] Could not extract %s (exit code %d)", g_inst.file, code);
      }
      else place();
      remove_tree(g_inst.dir);
      if (!g_inst.keep) os_unlink(g_inst.file);
      end_job(&g_inst);
      rows();
    }
    else if (g_inst.kind == JOB_REMOVE) {
      char *dir = g_inst.dir, *disp = g_inst.file;
      g_inst.dir = g_inst.file = NULL;
      end_job(&g_inst);
      removed(dir, disp, code);	/* may start it again (Retry comes later, from the notification) */
      free(dir);
      free(disp);
    }
    else end_job(&g_inst);
  }
  return changed;
}

/* }================================================================== */


/*
** {==================================================================
** Drawing, keys and the mouse
** ===================================================================
*/

static int busy_for (const char *id) {
  return g_inst.kind != JOB_NONE && g_inst.id && strcmp(g_inst.id, id) == 0;
}


/* its code runs (or will): a VS Code extension named in mme.extensions.run, an installed one not disabled */
static int is_on (const Item *it) {
  if (ehost_disabled(it->id)) return 0;
  return g_ext[it->ext].vscode ? ehost_in_run(it->id) : 1;
}


/* what the button of an item says */
static const char *button (const Item *it) {
  if (busy_for(it->id)) return "Installing";
  if (g_ext[it->ext].vscode) return "VS Code";
  return "Uninstall";
}


/* its Enable / Disable button's columns in a view w wide (from its left, *t0 up to *t1); 0: it has none */
static int toggle_cols (const Item *it, int w, int *t0, int *t1) {
  int bw = (int)strlen(button(it)) + 2, tw;
  if (!g_ext[it->ext].code || w - bw - 11 < 6) return 0;	/* the publisher keeps a few columns */
  tw = (int)strlen(is_on(it) ? "Disable" : "Enable") + 2;
  *t1 = w - bw - 2;
  *t0 = *t1 - tw;
  return 1;
}


void ext_draw (int x, int y, int w, int h, int focus) {
  int row, iw = w - 2;
  size_t k;
  char sec[160];
  scr_box(x, y, w, h, S_SIDE);
  if (h <= HEAD) return;
  scr_puts(x + 2, y, "EXTENSIONS", S_SIDE_HEAD);
  scr_fill(x + 1, y + 1, iw, S_INPUT);
  if (g_q[0]) scr_putsw(x + 2, y + 1, iw - 2, g_q, S_INPUT_ON);
  else scr_putsw(x + 2, y + 1, iw - 2, "Filter Installed Extensions", S_INPUT_HINT);
  if (focus) {
    int cx = x + 2 + (int)str_cols(g_q);
    scr_cursor(cx < x + iw ? cx : x + iw - 1, y + 1);
  }
  snprintf(sec, sizeof(sec), "INSTALLED  %lu", (unsigned long)g_nitem);
  scr_putsw(x + 1, y + 2, w - 2, sec, S_SIDE_HEAD);
  scr_put(x + 1, y + VSIX_ROW, 0xEAC2, S_SIDE_DIM);	/* codicon cloud-download: a .vsix you downloaded */
  {	/* a link, in the accent color */
    int lw = scr_putsw(x + 3, y + VSIX_ROW, w - 4, g_inst.kind == JOB_REMOVE ? "Uninstalling..." : g_inst.kind != JOB_NONE ? "Installing..." : "Install from VSIX...", S_SIDE), i;
    for (i = 0; i < lw; i++) scr_set_fg(x + 3 + i, y + VSIX_ROW, ui_color(C_ACCENT));
  }
  g_h = (h - HEAD) / ROWS;
  if (g_h < 1) g_h = 1;
  if (g_sel < g_top) g_top = g_sel;
  if (g_sel >= g_top + (size_t)g_h) g_top = g_sel - (size_t)g_h + 1;
  for (row = 0, k = g_top; k < g_nitem && row + ROWS <= h - HEAD; k++, row += ROWS) {
    const Item *it = &g_item[k];
    int sy = y + HEAD + row, st = (k == g_sel && focus) ? S_SIDE_SEL : (k == g_sel ? S_SIDE_CUR : S_SIDE);
    int dim = st == S_SIDE ? S_SIDE_DIM : st, i, cx;
    const char *bt = button(it);
    int bw = (int)strlen(bt) + 2;
    for (i = 0; i < ROWS; i++) scr_fill(x, sy + i, w, st);
    scr_put(x + 1, sy, 0xEB29, st == S_SIDE ? S_ICON_BLUE : st);	/* codicon package */
    cx = x + 3;
    {	/* its name, and its code's state after it: the state keeps its room, a long name is cut */
      const char *hs = g_ext[it->ext].code ? ehost_state(it->id) : NULL, *tag = NULL;
      uint32_t rgb = 0;
      char t[48];
      int room = x + w - cx - 1, tagw = 0;
      if (!g_ext[it->ext].code) tag = NULL;
      else if (!is_on(it) && hs && strcmp(hs, "running") == 0 && !ehost_disabled(it->id)) {
        tag = "running (needed)";	/* not enabled itself: one that runs needs it (Python for Python Debugger) */
        rgb = 0x73C991;
      }
      else if (!is_on(it)) tag = "disabled";
      else if (hs == NULL || strcmp(hs, "starting") == 0) tag = "enabled";	/* its activation event has not come */
      else if (strcmp(hs, "running") == 0) {	/* "running 0.4s": how long the host was busy starting it */
        int busy, ms = ehost_start_ms(it->id, &busy);
        static char rt[32];
        if (ms >= 0 && busy >= 500) snprintf(rt, sizeof(rt), "running %d.%ds", busy / 1000, busy % 1000 / 100);
        tag = ms >= 0 && busy >= 500 ? rt : "running";
        rgb = busy >= 1000 ? 0xCCA700 : 0x73C991;	/* a second or more: yellow, it holds the others back */
      }
      else if (strcmp(hs, "error") == 0) {
        tag = "failed";
        rgb = 0xF14C4C;
      }
      else {
        tag = hs;	/* "needs dart-code" */
        rgb = 0xCCA700;
      }
      if (tag) {
        snprintf(t, sizeof(t), "  %s", tag);
        tagw = (int)str_cols(t);
        if (tagw > room - 6) tagw = room - 6 > 0 ? room - 6 : 0;	/* a very narrow bar: the name first */
      }
      if ((int)str_cols(it->display) > room - tagw && room - tagw > 1) {	/* "Claude Code for V…  running" */
        cx += scr_putsw(cx, sy, room - tagw - 1, it->display, st == S_SIDE ? S_SIDE_TITLE : st);
        cx += scr_put(cx, sy, 0x2026, st == S_SIDE ? S_SIDE_TITLE : st);
      }
      else cx += scr_putsw(cx, sy, room, it->display, st == S_SIDE ? S_SIDE_TITLE : st);
      if (tag && tagw > 0) {
        int n = scr_putsw(cx, sy, x + w - cx - 1, t, dim), i;
        if (rgb && st == S_SIDE)
          for (i = 2; i < n; i++) scr_set_fg(cx + i, sy, rgb);
      }
    }
    scr_putsw(x + 3, sy + 1, w - 4, it->desc, dim);
    g_bx0 = w - bw - 1;
    g_bx1 = w - 1;
    {	/* Enable / Disable, VS Code's button, before the other: only for an extension with code */
      int t0, t1, tw = 0;
      if (toggle_cols(it, w, &t0, &t1)) {
        int on = is_on(it);
        tw = t1 - t0;
        scr_fill(x + t0, sy + 2, tw, on ? S_INPUT : S_TOGGLE_ON);
        scr_puts(x + t0 + 1, sy + 2, on ? "Disable" : "Enable", on ? S_INPUT : S_TOGGLE_ON);
      }
      scr_putsw(x + 3, sy + 2, w - bw - tw - 6, it->publisher, dim);
    }
    scr_fill(x + g_bx0, sy + 2, bw, !busy_for(it->id) ? S_INPUT : S_TOGGLE_ON);
    scr_puts(x + g_bx0 + 1, sy + 2, bt, !busy_for(it->id) ? S_INPUT : S_TOGGLE_ON);
  }
  if (g_nitem == 0 && h > HEAD + 2) {
    if (g_q[0]) scr_putsw(x + 2, y + HEAD + 1, w - 3, "No installed extension matches", S_SIDE_DIM);
    else {
      scr_putsw(x + 2, y + HEAD + 1, w - 3, "No extensions installed.", S_SIDE_DIM);
      scr_putsw(x + 2, y + HEAD + 2, w - 3, "Download a .vsix, then Install from VSIX...", S_SIDE_DIM);
    }
  }
  side_bar(x, y + HEAD, w, h - HEAD, g_nitem * (size_t)ROWS, g_top * (size_t)ROWS, (size_t)g_h * ROWS);	/* the rows of the whole ones shown */
}


/* the button: uninstall */
static void press (size_t k) {
  const Item *it;
  if (k >= g_nitem) return;
  it = &g_item[k];
  if (busy_for(it->id)) return;
  uninstall(it->ext);
}


static void set_run (const char *id, int on);
static void set_disabled (const char *id, int off);


static int served (const Ext *e, const char *lang) {	/* one of the languages the extension is for */
  size_t i;
  for (i = 0; i < e->nlang; i++)
    if (e->lang[i].id && strcmp(e->lang[i].id, lang) == 0) return 1;
  for (i = 0; i < e->onlang.n; i++)
    if (strcmp(e->onlang.v[i], lang) == 0) return 1;
  return 0;
}


/*
** The user's choice: an extension disabled takes its languages with it - mme's own server of each
** (mme.languageServers) is "" - and enabled gives them back (the extension's own runs them). Only the
** languages mme has a server for; the running server of one turned off stops.
*/
static void set_lang_servers (const Ext *e, int on) {
  const Json *cur = settings_get("mme\\.languageServers");
  const Json *saved = settings_get("mme\\.extensions\\.savedServers");	/* what Disable turned off, to give back */
  Buf b, sv;
  Vec langs, changed;
  size_t i;
  int first = 1, sfirst = 1;
  vec_init(&langs);
  vec_init(&changed);
  for (i = 0; i < e->nlang; i++)
    if (e->lang[i].id) vec_push(&langs, xstrdup(e->lang[i].id));
  for (i = 0; i < e->onlang.n; i++) vec_push(&langs, xstrdup(e->onlang.v[i]));
  buf_init(&b);
  buf_putc(&b, '{');
  buf_init(&sv);
  buf_putc(&sv, '{');
  for (i = 0; saved && saved->type == J_OBJ && i < saved->n; i++) {	/* the saved ones of other languages stay */
    const Json *k = saved->kid[i];
    if (k->key == NULL || served(e, k->key)) continue;
    buf_puts(&sv, sfirst ? "" : ", ");
    json_put_str(&sv, k->key, strlen(k->key));
    buf_puts(&sv, ": ");
    json_write(&sv, k);
    sfirst = 0;
  }
  for (i = 0; cur && cur->type == J_OBJ && i < cur->n; i++) {	/* the others as they are */
    const Json *k = cur->kid[i];
    if (k->key == NULL || served(e, k->key)) continue;
    buf_puts(&b, first ? "" : ", ");
    json_put_str(&b, k->key, strlen(k->key));
    buf_puts(&b, ": ");
    json_write(&b, k);
    first = 0;
  }
  for (i = 0; i < langs.n; i++) {
    const char *l = langs.v[i], *def = settings_server_default(l), *now;
    const Json *had = cur && cur->type == J_OBJ ? json_get(cur, l) : NULL;
    size_t j;
    int dup = 0;
    for (j = 0; j < i && !dup; j++) dup = strcmp(langs.v[j], l) == 0;
    if (dup) continue;
    now = settings_server(l);
    if (!on && now && now[0]) {	/* its server: off, what it was kept */
      buf_puts(&b, first ? "" : ", ");
      json_put_str(&b, l, strlen(l));
      buf_puts(&b, ": \"\"");
      buf_puts(&sv, sfirst ? "" : ", ");
      json_put_str(&sv, l, strlen(l));
      buf_puts(&sv, ": ");
      json_put_str(&sv, now, strlen(now));
      sfirst = 0;
      vec_push(&changed, xstrdup(l));
    }
    else if (on && now && !now[0] && (json_str(saved ? json_get(saved, l) : NULL, NULL) || (def && def[0]))) {
      const char *back = json_str(saved ? json_get(saved, l) : NULL, NULL);	/* back: as it was, else mme's own */
      if (back == NULL) back = def;
      buf_puts(&b, first ? "" : ", ");
      json_put_str(&b, l, strlen(l));
      buf_puts(&b, ": ");
      json_put_str(&b, back, strlen(back));
      vec_push(&changed, xstrdup(l));
    }
    else if (had) {	/* as it was */
      buf_puts(&b, first ? "" : ", ");
      json_put_str(&b, l, strlen(l));
      buf_puts(&b, ": ");
      json_write(&b, had);
    }
    else continue;
    first = 0;
  }
  buf_putc(&b, '}');
  buf_putc(&b, '\0');
  buf_putc(&sv, '}');
  buf_putc(&sv, '\0');
  if (changed.n) {
    settings_put_json("mme.extensions.savedServers", sv.s);
    settings_put_json("mme.languageServers", b.s);
    if (!on)
      for (i = 0; i < changed.n; i++) lsp_restart(changed.v[i]);	/* its running server stops (enabled: the extension's own serves it) */
  }
  buf_free(&b);
  buf_free(&sv);
  vec_free(&langs);
  vec_free(&changed);
}

/* its code on or off (a VS Code extension: mme.extensions.run; the others: mme.extensions.disabled), its
** languages with it; the extension host starts again without it, or with it. The button and the menu's
** Enable, Disable, Run, Stop all come here */
static void code_set (const Item *it, int on) {
  if (!on) {
    set_lang_servers(&g_ext[it->ext], 0);	/* its languages go with it (mme's own servers of them too) */
    if (g_ext[it->ext].vscode) set_run(it->id, 0);
    else set_disabled(it->id, 1);
  }
  else {
    set_lang_servers(&g_ext[it->ext], 1);	/* and come back */
    if (ehost_disabled(it->id)) set_disabled(it->id, 0);
    if (g_ext[it->ext].vscode && !ehost_in_run(it->id)) set_run(it->id, 1);
  }
}


/* the Enable / Disable button */
static void toggle (size_t k) {
  const Item *it;
  if (k >= g_nitem) return;
  it = &g_item[k];
  if (!g_ext[it->ext].code) return;
  code_set(it, !is_on(it));
}


static const Ext *g_themed;

static void theme_preview (int i) {
  theme_set(g_themed->theme[i].label);
}


/* Set Color Theme: one of an extension's themes, each shown while it is selected */
static void pick_ext_theme (const Ext *e) {
  Pick p;
  size_t i;
  int r;
  char was[64];
  snprintf(was, sizeof(was), "%s", opt.theme);
  pick_init(&p, "Set Color Theme");
  for (i = 0; i < e->ntheme; i++) pick_add(&p, e->theme[i].label, e->theme[i].light == 1 ? "light" : e->theme[i].light == 2 ? "high contrast" : e->theme[i].light == 3 ? "high contrast light" : "dark", 0xEB5C);
  g_themed = e;
  p.keep_order = 1;
  p.on_move = theme_preview;
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) {
    theme_set(was);
    return;
  }
  snprintf(opt.theme, sizeof(opt.theme), "%s", e->theme[r].label);
  theme_set(opt.theme);
  settings_put("workbench.colorTheme", opt.theme);
}


/* the list setting key with id in it (on) or not: the extension host starts again with it */
static void set_in (const char *key, const char *id, int on) {
  char esc[128], *q;
  const Json *list;
  Buf b;
  size_t i;
  int n = 0;
  snprintf(esc, sizeof(esc), "%s", key);	/* "mme.extensions.run" looked up as "mme\\.extensions\\.run" */
  {
    Buf k;
    buf_init(&k);
    for (q = esc; *q; q++) {
      if (*q == '.') buf_putc(&k, '\\');
      buf_putc(&k, *q);
    }
    buf_putc(&k, '\0');
    list = settings_get(k.s);
    buf_free(&k);
  }
  buf_init(&b);
  buf_putc(&b, '[');
  for (i = 0; list && list->type == J_ARR && i < list->n; i++) {
    const char *s = json_str(list->kid[i], "");
    if (m_stricmp(s, id) == 0) continue;
    buf_puts(&b, n++ ? ", " : "");
    json_put_str(&b, s, strlen(s));
  }
  if (on) {
    buf_puts(&b, n++ ? ", " : "");
    json_put_str(&b, id, strlen(id));
  }
  buf_putc(&b, ']');
  buf_putc(&b, '\0');
  settings_put_json(key, b.s);
  buf_free(&b);
  mme_settings_changed();
}


static void set_run (const char *id, int on) {
  set_in("mme.extensions.run", id, on);
  toast(0, on ? "%s: its code runs now (the extension host)" : "%s: its code no longer runs", id);
}


static void set_disabled (const char *id, int off) {
  set_in("mme.extensions.disabled", id, off);
  toast(0, off ? "%s: disabled (its code does not run)" : "%s: enabled", id);
}


/* Enter: what can be done with it */
static void actions (size_t k) {
  Pick p;
  const Item *it;
  int acts[8], n = 0, r;
  enum { A_PAGE, A_UNINSTALL, A_THEME, A_RUN, A_STOP, A_DISABLE, A_ENABLE };
  if (k >= g_nitem) return;
  it = &g_item[k];
  pick_init(&p, it->display);
  p.keep_order = 1;
  if (g_ext[it->ext].ntheme) {
    acts[n++] = A_THEME;
    pick_add(&p, "Set Color Theme", NULL, 0xEB5C);
  }
  if (ehost_disabled(it->id)) {	/* its code turned off: on again */
    acts[n++] = A_ENABLE;
    pick_add(&p, "Enable", "its code runs again", 0xEB2C);
  }
  else if (!g_ext[it->ext].vscode && ehost_state(it->id) != NULL) {	/* mme-data's run by themselves: Disable turns it off */
    acts[n++] = A_DISABLE;
    pick_add(&p, "Disable", "its code does not run (its themes, snippets still work)", 0xEAD7);
  }
  if (g_ext[it->ext].vscode && !ehost_disabled(it->id)) {	/* VS Code's run only when named in mme.extensions.run */
    if (ehost_state(it->id) == NULL) {
      acts[n++] = A_RUN;
      pick_add(&p, "Run This Extension", "its code, in the extension host", 0xEB2C);
    }
    else {
      acts[n++] = A_STOP;
      pick_add(&p, "Stop Running It", NULL, 0xEAD7);
    }
  }
  acts[n++] = A_PAGE;
  pick_add(&p, "Show Details", NULL, 0xEA74);
  if (!g_ext[it->ext].vscode) {
    acts[n++] = A_UNINSTALL;
    pick_add(&p, "Uninstall", NULL, 0xEA81);
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) return;
  switch (acts[r]) {
    case A_UNINSTALL: press(k); break;
    case A_THEME: pick_ext_theme(&g_ext[it->ext]); break;
    case A_RUN: code_set(it, 1); break;	/* the button's way: its languages with it */
    case A_DISABLE: code_set(it, 0); break;
    case A_ENABLE: code_set(it, 1); break;
    case A_STOP: code_set(it, 0); break;
    default: show_page(it); break;
  }
}


static void typed (void) {	/* the filter changed: the list at once, from its top */
  g_sel = g_top = 0;
  rows();
}


/* Install from VSIX...: mme.c's command (its file dialog) */
static void vsix (SideAct *act) {
  act->what = SA_CMD;
  act->cmd = CMD_EXT_VSIX;
}


int ext_key (int k, SideAct *act) {
  int code = KEY_CODE(k);
  size_t len = strlen(g_q);
  switch (code) {
    case K_UP: if (g_sel > 0) g_sel--; return 1;
    case K_DOWN: if (g_sel + 1 < g_nitem) g_sel++; return 1;
    case K_PGUP: g_sel = g_sel > (size_t)g_h ? g_sel - (size_t)g_h : 0; return 1;
    case K_PGDN:
      g_sel += (size_t)g_h;
      if (g_sel >= g_nitem) g_sel = g_nitem ? g_nitem - 1 : 0;
      return 1;
    case K_HOME: g_sel = 0; return 1;
    case K_END: g_sel = g_nitem ? g_nitem - 1 : 0; return 1;
    case K_ENTER:
      if (g_nitem == 0 && g_q[0] == '\0') vsix(act);	/* nothing installed: the way to install */
      else actions(g_sel);
      return 1;
    case K_ESC:
      if (g_q[0] == '\0') return 0;
      g_q[0] = '\0';
      typed();
      return 1;
    case K_BS:
      while (len > 0 && ((unsigned char)g_q[len - 1] & 0xC0) == 0x80) len--;
      if (len > 0) g_q[len - 1] = '\0';
      typed();
      return 1;
    case K_PASTE: case CTRL('v'): {
      Buf b;
      size_t i;
      buf_init(&b);
      paste_take(k, &b);
      for (i = 0; i < b.len && b.s[i] != '\n' && len + 1 < sizeof(g_q); i++) g_q[len++] = b.s[i];
      g_q[len] = '\0';
      buf_free(&b);
      typed();
      return 1;
    }
  }
  if (IS_TEXT(k) && len + 4 < sizeof(g_q)) {
    len += (size_t)utf8_encode((uint32_t)k, g_q + len);
    g_q[len] = '\0';
    typed();
    return 1;
  }
  return 0;
}


void ext_click (int row, int col, SideAct *act) {
  size_t k;
  if (row == VSIX_ROW) {
    if (g_inst.kind == JOB_NONE) vsix(act);
    return;
  }
  if (row < HEAD) return;
  k = g_top + (size_t)((row - HEAD) / ROWS);
  if (k >= g_nitem) return;
  g_sel = k;
  {
    int t0, t1, bw = (int)strlen(button(&g_item[k])) + 2, w = side_width();
    if ((row - HEAD) % ROWS == 2 && col >= w - bw - 1 && col < w - 1) press(k);
    else if ((row - HEAD) % ROWS == 2 && toggle_cols(&g_item[k], w, &t0, &t1) && col >= t0 && col < t1) toggle(k);
    else show_page(&g_item[k]);
  }
}


void ext_wheel (int d) {
  size_t st = (size_t)wheel_step(0);
  if (d < 0) g_top = g_top > st ? g_top - st : 0;
  else if (g_nitem > (size_t)g_h && g_top + (size_t)g_h < g_nitem)
    g_top = g_top + st + (size_t)g_h <= g_nitem ? g_top + st : g_nitem - (size_t)g_h;
  if (g_sel < g_top) g_sel = g_top;
  if (g_sel >= g_top + (size_t)g_h) g_sel = g_top + (size_t)g_h - 1;
}


/* its scrollbar dragged: top is in rows (ROWS for each extension); the selection stays in sight */
void ext_scroll_to (size_t top) {
  g_top = top / ROWS;
  if (g_nitem > (size_t)g_h && g_top > g_nitem - (size_t)g_h) g_top = g_nitem - (size_t)g_h;
  if (g_sel < g_top) g_sel = g_top;
  if (g_sel >= g_top + (size_t)g_h) g_sel = g_top + (size_t)g_h - 1;
}


/* the view is shown: the installed list up to date */
void ext_show (void) {
  rows();
}

/* }================================================================== */
