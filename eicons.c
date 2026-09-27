/*
** eicons.c - file icon themes (workbench.iconTheme)
**
** An installed extension's icon theme (vscode-icons, Material Icon Theme)
** gives the files and folders their icons, as in VS Code: by the file's
** name, its extensions (the longest first: "d.ts" before "ts"), its
** language, else the theme's file icon; a folder by its name, open or
** closed. The icons are its SVG files, drawn by esvg.c at a cell's size.
**
** Only mme-sdl draws pictures in cells: in a terminal, and for a theme
** that draws with a font (Seti) or PNG files, mme's own icons stay. An
** icon of the theme is a code point from ICON_CP on (plane 15's private
** use area): file_icon returns it, sdl2_port/esdl.c draws its picture.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct IDef {
  char *id;
  char *path;	/* the SVG file; NULL: none mme can draw (a font's character, a PNG) */
  uint32_t *px;	/* drawn at size (0xAARRGGBB), or NULL */
  int size, bad;	/* bad: the file could not be read or drawn */
} IDef;

typedef struct IMap {
  char *key;	/* lower case */
  int def;
} IMap;

enum { M_EXT, M_NAME, M_LANG, M_FOLDER, M_FOLDER_OPEN, M_N };

typedef struct ISet {	/* the theme's maps, or its "light" ones */
  IMap *m[M_N];
  size_t n[M_N];
  int file, folder, folder_open, root, root_open;	/* -1: none */
} ISet;

static struct {
  char *want;	/* the setting it was loaded for */
  int on;	/* a theme is in use */
  IDef *def;
  size_t ndef;
  ISet dark, light;
  int pictures;	/* the window draws pictures in cells (mme-sdl) */
  const char *preview;	/* the picker's theme shown now, over the setting; NULL: none */
} I;


void icons_pictures (int on) {
  I.pictures = on;
}


static void set_free (ISet *s) {
  int k;
  size_t i;
  for (k = 0; k < M_N; k++) {
    for (i = 0; i < s->n[k]; i++) free(s->m[k][i].key);
    free(s->m[k]);
  }
  memset(s, 0, sizeof(*s));
  s->file = s->folder = s->folder_open = s->root = s->root_open = -1;
}


static void unload (void) {
  size_t i;
  for (i = 0; i < I.ndef; i++) {
    free(I.def[i].id);
    free(I.def[i].path);
    free(I.def[i].px);
  }
  free(I.def);
  I.def = NULL;
  I.ndef = 0;
  set_free(&I.dark);
  set_free(&I.light);
  I.on = 0;
}


static int def_by_id (const char *id) {
  size_t i;
  for (i = 0; id && i < I.ndef; i++)
    if (strcmp(I.def[i].id, id) == 0) return (int)i;
  return -1;
}


static int cmp_map (const void *a, const void *b) {
  return strcmp(((const IMap *)a)->key, ((const IMap *)b)->key);
}


static char *lower (const char *s) {
  char *d = xstrdup(s), *p;
  for (p = d; *p; p++)
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
  return d;
}


/* a map of the theme ("fileExtensions": {"ts": "_f_ts"}) into s's kind k, sorted for looking up */
static void read_map (ISet *s, int k, const Json *o) {
  size_t i;
  if (o == NULL || o->type != J_OBJ) return;
  s->m[k] = (IMap *)xmalloc((o->n + 1) * sizeof(IMap));
  for (i = 0; i < o->n; i++) {
    int d = def_by_id(json_str(o->kid[i], NULL));
    if (d < 0 || o->kid[i]->key == NULL) continue;
    s->m[k][s->n[k]].key = lower(o->kid[i]->key);
    s->m[k][s->n[k]++].def = d;
  }
  qsort(s->m[k], s->n[k], sizeof(IMap), cmp_map);
}


static void read_set (ISet *s, const Json *j) {
  set_free(s);
  if (j == NULL) return;
  read_map(s, M_EXT, json_get(j, "fileExtensions"));
  read_map(s, M_NAME, json_get(j, "fileNames"));
  read_map(s, M_LANG, json_get(j, "languageIds"));
  read_map(s, M_FOLDER, json_get(j, "folderNames"));
  read_map(s, M_FOLDER_OPEN, json_get(j, "folderNamesExpanded"));
  s->file = def_by_id(json_str(json_get(j, "file"), NULL));
  s->folder = def_by_id(json_str(json_get(j, "folder"), NULL));
  s->folder_open = def_by_id(json_str(json_get(j, "folderExpanded"), NULL));
  s->root = def_by_id(json_str(json_get(j, "rootFolder"), NULL));
  s->root_open = def_by_id(json_str(json_get(j, "rootFolderExpanded"), NULL));
}


/* the theme named by workbench.iconTheme, when it changed */
static void load (void) {
  const char *want = I.preview ? I.preview : json_str(settings_get("workbench\\.iconTheme"), "");
  const char *path;
  char *s, *dir;
  size_t len, i, nsvg = 0;
  Json *j;
  const Json *defs;
  if (I.want && strcmp(I.want, want) == 0) return;
  free(I.want);
  I.want = xstrdup(want);
  unload();
  if (!*want || strcmp(want, "vs-seti") == 0 || strcmp(want, "vs-minimal") == 0) return;	/* mme's own: Seti's */
  if ((path = ext_icon_theme_path(want)) == NULL) {
    out_log("Extensions", "[warning] workbench.iconTheme: no installed extension has the icon theme \"%s\"", want);
    return;
  }
  if ((s = read_file(path, &len)) == NULL) return;
  j = json_parse(s, len);
  free(s);
  if (j == NULL) {
    out_log("Extensions", "[error] the icon theme %s is not good JSON", path);
    return;
  }
  dir = path_dirname(path);
  defs = json_get(j, "iconDefinitions");
  if (defs && defs->type == J_OBJ) {
    I.def = (IDef *)xmalloc((defs->n + 1) * sizeof(IDef));
    for (i = 0; i < defs->n; i++) {
      const char *ip = json_str(json_get(defs->kid[i], "iconPath"), "");
      size_t il = strlen(ip);
      IDef *d = &I.def[I.ndef++];
      memset(d, 0, sizeof(*d));
      d->id = xstrdup(defs->kid[i]->key ? defs->kid[i]->key : "");
      if (il > 4 && m_stricmp(ip + il - 4, ".svg") == 0) {	/* "../../icons/file_type_c.svg" */
        char *p = path_join(dir, ip), *r = os_realpath(p);	/* the ".." out */
        d->path = r ? r : p;
        if (r) free(p);
        nsvg++;
      }
    }
  }
  read_set(&I.dark, j);
  read_set(&I.light, json_get(j, "light"));
  I.on = nsvg > 0;
  if (!I.on)
    out_log("Extensions", "[info] the icon theme \"%s\" draws with a font or PNG files: mme shows its own icons", want);
  free(dir);
  json_free(j);
}


/* a theme's pictures show (the explorer makes room for the folders' icons) */
int icons_active (void) {
  load();
  return I.on && I.pictures;
}


/* the settings changed */
void icons_settings (void) {
  load();
}


static int find (const ISet *s, int k, const char *key) {
  size_t lo = 0, hi = s->n[k];
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = strcmp(s->m[k][mid].key, key);
    if (c == 0) return s->m[k][mid].def;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return -1;
}


static int light_ui (void) {
  uint32_t c = ui_color(C_EDITOR_BG);
  return ((c >> 16 & 255) * 3 + (c >> 8 & 255) * 6 + (c & 255)) / 10 > 128;
}


static uint32_t cp_of (int d) {
  if (d < 0 || (size_t)d >= I.ndef || I.def[d].path == NULL || I.def[d].bad) return 0;
  return ICON_CP + (uint32_t)d;
}


/* a file's icon in the theme; 0: no theme, or none it can draw (mme's own then) */
uint32_t icons_file (const char *name) {
  const ISet *sets[2];
  char *low;
  int n, k, d = -1;
  const char *lang = NULL;
  load();
  if (!I.on || !I.pictures || name == NULL) return 0;
  name = path_basename(name);
  low = lower(name);
  n = 0;
  if (light_ui() && (I.light.n[M_NAME] || I.light.n[M_EXT] || I.light.file >= 0)) sets[n++] = &I.light;
  sets[n++] = &I.dark;
  for (k = 0; k < n && d < 0; k++) d = find(sets[k], M_NAME, low);
  if (d < 0) {	/* "a.test.ts": "test.ts", then "ts" */
    const char *e = strchr(low, '.');
    while (e && d < 0) {
      if (e[1])
        for (k = 0; k < n && d < 0; k++) d = find(sets[k], M_EXT, e + 1);
      e = strchr(e + 1, '.');
    }
  }
  if (d < 0) {
    const Syntax *sx = syntax_for(name);
    lang = sx ? syntax_id(syntax_name(sx)) : NULL;
    if (lang == NULL) lang = ext_lang_for(name);
    for (k = 0; lang && k < n && d < 0; k++) d = find(sets[k], M_LANG, lang);
  }
  for (k = 0; k < n && d < 0; k++) d = sets[k]->file;
  free(low);
  return cp_of(d);
}


/* a folder's icon (open or closed; root: a workspace's top folder); 0: none in the theme */
uint32_t icons_folder (const char *name, int open, int root) {
  const ISet *sets[2];
  char *low;
  int n = 0, k, d = -1;
  load();
  if (!I.on || !I.pictures || name == NULL) return 0;
  low = lower(path_basename(name));
  if (light_ui() && (I.light.n[M_FOLDER] || I.light.folder >= 0)) sets[n++] = &I.light;
  sets[n++] = &I.dark;
  for (k = 0; k < n && d < 0; k++) d = find(sets[k], open ? M_FOLDER_OPEN : M_FOLDER, low);
  if (d < 0 && open)
    for (k = 0; k < n && d < 0; k++) d = find(sets[k], M_FOLDER, low);	/* no open one: the closed one */
  for (k = 0; root && k < n && d < 0; k++) d = open ? sets[k]->root_open : sets[k]->root;
  for (k = 0; k < n && d < 0; k++) d = open ? sets[k]->folder_open : sets[k]->folder;
  for (k = 0; open && k < n && d < 0; k++) d = sets[k]->folder;
  free(low);
  return cp_of(d);
}


/* the theme hides the explorer's chevrons (hidesExplorerArrows)? not followed: mme keeps them */

/* the picture of icon cp, size by size pixels (0xAARRGGBB); NULL: none */
const uint32_t *icons_pixels (uint32_t cp, int size) {
  IDef *d;
  if (cp < ICON_CP || (size_t)(cp - ICON_CP) >= I.ndef || size <= 0 || size > 256) return NULL;
  d = &I.def[cp - ICON_CP];
  if (d->bad || d->path == NULL) return NULL;
  if (d->px == NULL || d->size != size) {
    size_t len;
    char *s = read_file(d->path, &len);
    free(d->px);
    d->px = s ? svg_render(s, len, size, size) : NULL;
    d->size = size;
    free(s);
    if (d->px == NULL) {
      d->bad = 1;
      out_log("Extensions", "[warning] icon %s: could not draw %s", d->id, d->path);
    }
  }
  return d->px;
}


/* Preferences: File Icon Theme: the installed extensions' icon themes, and mme's own; Up / Down show them */
static void preview (int i) {
  I.preview = i <= 0 ? "vs-seti" : ext_icon_theme_id(i - 1);
}


void icons_pick (void) {
  Pick p;
  int i, r, now = 0, n = ext_icon_theme_count();
  const char *cur = json_str(settings_get("workbench\\.iconTheme"), "");
  pick_init(&p, "Select File Icon Theme (Up/Down Keys to Preview)");
  pick_add(&p, "Seti (mme's own)", !*cur || strcmp(cur, "vs-seti") == 0 ? "current" : NULL, 0xEB29);
  for (i = 0; i < n; i++) {
    int is = strcmp(cur, ext_icon_theme_id(i)) == 0;
    pick_add(&p, ext_icon_theme_label(i), is ? "current" : NULL, 0xEB29);
    if (is) now = i + 1;
  }
  p.keep_order = 1;
  p.start = now;
  p.on_move = preview;
  r = pick_run(&p);
  pick_free(&p);
  I.preview = NULL;
  if (r < 0) return;
  settings_put("workbench.iconTheme", r == 0 ? "vs-seti" : ext_icon_theme_id(r - 1));
  if (!I.pictures && r > 0) toast(0, "Icon themes' pictures show in mme-sdl; here mme's own icons stay");
}
