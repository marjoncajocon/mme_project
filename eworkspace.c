/*
** eworkspace.c - multi-root workspaces: VS Code's .code-workspace files
**
** {"folders": [{"path": "."}, {"path": "../lib", "name": "Library"}],
**  "settings": {"editor.tabSize": 2}}: each folder is a root of the
** Explorer, Go to File and Search look in all of them, and the settings
** apply over the user's. "Add Folder to Workspace..." on a plain folder
** makes an untitled workspace, "Save Workspace As..." writes it.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static struct {
  int on;	/* a workspace is open */
  char *file;	/* its .code-workspace; NULL: untitled */
  char *name;
  Vec paths, names;	/* the folders (realpaths) and their names ("" : the folder's own) */
  Json *settings;	/* the file's "settings", NULL: none */
  char title[300];
} WS;


static void clear (void) {
  if (WS.on) {
    vec_free(&WS.paths);
    vec_free(&WS.names);
  }
  free(WS.file);
  free(WS.name);
  json_free(WS.settings);
  memset(&WS, 0, sizeof(WS));
}


int ws_active (void) {
  return WS.on;
}


const char *ws_file (void) {
  return WS.file;
}


/* "mme (Workspace)", like VS Code's title */
const char *ws_title (void) {
  snprintf(WS.title, sizeof(WS.title), "%s (Workspace)", WS.name ? WS.name : "Untitled");
  return WS.title;
}


const Json *ws_settings (void) {
  return WS.on ? WS.settings : NULL;
}


/* the folders: how many, and each one's path and name (the name shown for it) */
int ws_count (void) {
  return WS.on ? (int)WS.paths.n : 1;
}


const char *ws_folder (int i) {
  return WS.on ? WS.paths.v[i] : side_root();
}


const char *ws_folder_name (int i) {
  if (!WS.on) return path_basename(side_root());
  return WS.names.v[i][0] ? WS.names.v[i] : path_basename(WS.paths.v[i]);
}


static void show (void) {
  side_open_roots(WS.paths.v, WS.names.v, (int)WS.paths.n, ws_title());
}


/* a path in the file: from the file's folder when not absolute */
static char *resolve (const char *dir, const char *p) {
  char *j, *r;
  const char *q = p;
  if (strncmp(q, "file://", 7) == 0) q += 7;
  if (path_is_sep(q[0]) || (q[0] && q[1] == ':')) j = xstrdup(q);
  else j = path_join(dir, q);
  r = os_realpath(j);
  if (r) {
    free(j);
    return r;
  }
  return j;
}


/* opens a .code-workspace; -1: not one that can be read */
int ws_open (const char *file) {
  size_t len, i;
  char *s = read_file(file, &len), *dir, *real;
  Json *j;
  const Json *folders;
  if (s == NULL) return -1;
  j = json_parse(s, len);
  free(s);
  folders = json_get(j, "folders");
  if (j == NULL || folders == NULL || folders->type != J_ARR) {
    json_free(j);
    return -1;
  }
  real = os_realpath(file);
  clear();
  WS.on = 1;
  WS.file = real ? real : xstrdup(file);
  WS.name = xstrdup(path_basename(WS.file));
  if (strlen(WS.name) > 15 && strcmp(WS.name + strlen(WS.name) - 15, ".code-workspace") == 0)
    WS.name[strlen(WS.name) - 15] = '\0';
  vec_init(&WS.paths);
  vec_init(&WS.names);
  dir = path_dirname(WS.file);
  for (i = 0; i < folders->n; i++) {
    const char *p = json_str(json_get(folders->kid[i], "path"), json_str(json_get(folders->kid[i], "uri"), NULL));
    char *f;
    OsStat st;
    if (p == NULL) continue;
    f = resolve(dir, p);
    if (os_stat(f, &st) != 0 || !st.is_dir) {	/* VS Code shows it missing; mme leaves it out */
      toast(1, "The folder '%s' of the workspace is not there", p);
      free(f);
      continue;
    }
    vec_push(&WS.paths, f);
    vec_push(&WS.names, xstrdup(json_str(json_get(folders->kid[i], "name"), "")));
  }
  free(dir);
  if (json_get(j, "settings")) {	/* kept as its own tree */
    Buf b;
    buf_init(&b);
    json_write(&b, json_get(j, "settings"));
    WS.settings = json_parse(b.s, b.len);
    buf_free(&b);
  }
  json_free(j);
  if (WS.paths.n == 0) {
    clear();
    toast(1, "The workspace has no folder that can be opened");
    return -1;
  }
  show();
  return 0;
}


/* Add Folder to Workspace: a plain folder becomes an untitled workspace first */
void ws_add_folder (const char *dir) {
  char *real = os_realpath(dir);
  size_t i;
  if (!WS.on) {
    WS.on = 1;
    vec_init(&WS.paths);
    vec_init(&WS.names);
    vec_push(&WS.paths, xstrdup(side_root()));
    vec_push(&WS.names, xstrdup(""));
  }
  for (i = 0; i < WS.paths.n; i++)
    if (m_fncmp(WS.paths.v[i], real ? real : dir) == 0) {
      free(real);
      return;
    }
  vec_push(&WS.paths, real ? real : xstrdup(dir));
  vec_push(&WS.names, xstrdup(""));
  show();
}


/* Save Workspace As: the folders (from the file's folder when under it) and the settings */
int ws_save_as (const char *file) {
  Buf b;
  size_t i;
  int fd;
  char *dir = path_dirname(file), *rdir = os_realpath(dir);
  const char *base = rdir ? rdir : dir;
  size_t bl = strlen(base);
  if (!WS.on) ws_add_folder(side_root());
  buf_init(&b);
  buf_puts(&b, "{\n  \"folders\": [");
  for (i = 0; i < WS.paths.n; i++) {
    const char *p = WS.paths.v[i];
    char *rel = NULL, *c;
    if (m_fnncmp(p, base, bl) == 0 && (p[bl] == '\0' || path_is_sep(p[bl])))
      rel = xstrdup(p[bl] ? p + bl + 1 : ".");
    for (c = rel; c && *c; c++)
      if (*c == '\\') *c = '/';
    buf_puts(&b, i ? ",\n    {\"path\": " : "\n    {\"path\": ");
    json_put_str(&b, rel ? rel : p, strlen(rel ? rel : p));
    if (WS.names.v[i][0]) {
      buf_puts(&b, ", \"name\": ");
      json_put_str(&b, WS.names.v[i], strlen(WS.names.v[i]));
    }
    buf_putc(&b, '}');
    free(rel);
  }
  buf_puts(&b, "\n  ],\n  \"settings\": ");
  if (WS.settings) json_write(&b, WS.settings);
  else buf_puts(&b, "{}");
  buf_puts(&b, "\n}\n");
  fd = os_open(file, OS_WRITE);
  if (fd >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  buf_free(&b);
  free(dir);
  free(rdir);
  if (fd < 0) return -1;
  free(WS.file);
  WS.file = os_realpath(file);
  if (WS.file == NULL) WS.file = xstrdup(file);
  free(WS.name);
  WS.name = xstrdup(path_basename(WS.file));
  if (strlen(WS.name) > 15 && strcmp(WS.name + strlen(WS.name) - 15, ".code-workspace") == 0)
    WS.name[strlen(WS.name) - 15] = '\0';
  show();
  return 0;
}


/* Close Workspace: the first folder alone again */
void ws_close (void) {
  char *first;
  if (!WS.on) return;
  first = xstrdup(WS.paths.v[0]);
  clear();
  side_open(first);
  free(first);
}


/* a plain folder opens: no workspace any more */
void ws_forget (void) {
  clear();
}
