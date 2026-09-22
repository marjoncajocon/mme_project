/*
** eimport.c - Preferences: Import VS Code Settings
**
** VS Code keeps the user's settings.json, keybindings.json and snippets in
** its User folder (%APPDATA%\Code\User, ~/.config/Code/User,
** ~/Library/Application Support/Code/User; Insiders and VSCodium too; a
** portable VS Code's data/user-data/User, see evscode.c).
** They are only read: the settings mme has, the keys of commands mme has
** and the snippet files go into mme-data.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* VS Code's user folders that are there (a portable VS Code's first) */
int import_find (Vec *dirs) {
  return vscode_user_dirs(dirs);
}


static Json *read_json (const char *dir, const char *name) {
  char *f = path_join(dir, name), *s;
  size_t len;
  Json *j = NULL;
  s = read_file(f, &len);
  free(f);
  if (s) j = json_parse(s, len);
  free(s);
  return j;
}


/* the snippet files of dir/snippets */
static void snippet_files (const char *dir, Vec *out) {
  char *sd = path_join(dir, "snippets");
  Vec v;
  size_t i;
  vec_init(out);
  vec_init(&v);
  if (os_listdir(sd, &v) == 0)
    for (i = 0; i < v.n; i++) {
      size_t n = strlen(v.v[i]);
      if ((n > 5 && strcmp(v.v[i] + n - 5, ".json") == 0) || (n > 14 && strcmp(v.v[i] + n - 14, ".code-snippets") == 0))
        vec_push(out, xstrdup(v.v[i]));
    }
  vec_sort(out);
  vec_free(&v);
  free(sd);
}


/* keybindings.json's entries mme has a command for */
static int keys_usable (const Json *list) {
  size_t i;
  int n = 0;
  for (i = 0; list && list->type == J_ARR && i < list->n; i++) {
    const char *c = json_str(json_get(list->kid[i], "command"), NULL);
    if (c && cmd_by_id(c[0] == '-' ? c + 1 : c) != CMD_NONE) n++;
  }
  return n;
}


/* what an import from dir would take, for the dialog */
char *import_preview (const char *dir) {
  Json *set = read_json(dir, "settings.json"), *keys = read_json(dir, "keybindings.json");
  Vec sn;
  Buf b;
  size_t i;
  int n = 0, shown = 0;
  buf_init(&b);
  for (i = 0; set && set->type == J_OBJ && i < set->n; i++)
    if (settings_known(set->kid[i]->key)) n++;
  buf_printf(&b, "%d setting%s", n, n == 1 ? "" : "s");
  for (i = 0; set && set->type == J_OBJ && i < set->n && shown < 4; i++)
    if (settings_known(set->kid[i]->key)) buf_printf(&b, "%s%s", shown++ ? ", " : " (", set->kid[i]->key);
  if (shown) buf_puts(&b, n > shown ? ", ...)" : ")");
  n = keys_usable(keys);
  snippet_files(dir, &sn);
  buf_printf(&b, ", %d keybinding%s, %lu snippet file%s", n, n == 1 ? "" : "s",
             (unsigned long)sn.n, sn.n == 1 ? "" : "s");
  buf_putc(&b, '\0');
  vec_free(&sn);
  json_free(set);
  json_free(keys);
  return buf_take(&b);
}


/* VS Code's theme names to mme's: "Default Dark Modern" is "Dark Modern" */
static const char *theme_of (const char *vs) {
  if (strncmp(vs, "Default ", 8) == 0) return vs + 8;
  if (strcmp(vs, "Visual Studio Dark") == 0) return "Dark+";
  if (strcmp(vs, "Visual Studio Light") == 0) return "Light+";
  return vs;
}


static int g_offered = -1;	/* the Welcome page's link: -1 not known yet */


static char *marker (void) {
  return data_path("vscode-imported");
}


/* the import: counts of what was taken */
void import_run (const char *dir, int *settings, int *keys, int *snippets) {
  Json *set = read_json(dir, "settings.json"), *kb = read_json(dir, "keybindings.json");
  Vec sn;
  size_t i;
  char *sd, *md, *m;
  int fd;
  *settings = *keys = *snippets = 0;
  for (i = 0; set && set->type == J_OBJ && i < set->n; i++) {
    const Json *v = set->kid[i];
    Buf b;
    if (!settings_known(v->key)) continue;
    if (strcmp(v->key, "workbench.colorTheme") == 0 && v->type == J_STR) {
      settings_put(v->key, theme_of(v->str));
      (*settings)++;
      continue;
    }
    buf_init(&b);
    json_write(&b, v);
    buf_putc(&b, '\0');
    settings_put_json(v->key, b.s);
    buf_free(&b);
    (*settings)++;
  }
  *keys = keys_import(kb);
  snippet_files(dir, &sn);
  sd = path_join(dir, "snippets");
  md = snip_dir();
  for (i = 0; md && i < sn.n; i++) {	/* the ones mme has already stay mme's */
    char *from = path_join(sd, sn.v[i]), *to = path_join(md, sn.v[i]), *s;
    OsStat st;
    size_t len;
    if (os_stat(to, &st) != 0 || !st.exists) {
      if ((s = read_file(from, &len)) != NULL && (fd = os_open(to, OS_WRITE)) >= 0) {
        os_write(fd, s, len);
        os_close(fd);
        (*snippets)++;
      }
      free(s);
    }
    free(from);
    free(to);
  }
  free(sd);
  free(md);
  vec_free(&sn);
  json_free(set);
  json_free(kb);
  g_offered = 0;
  m = marker();	/* the Welcome page stops offering it */
  if ((fd = os_open(m, OS_WRITE)) >= 0) {
    os_write(fd, dir, strlen(dir));
    os_close(fd);
  }
  free(m);
}


int import_offered (void) {
  Vec d;
  char *m;
  OsStat st;
  if (g_offered >= 0) return g_offered;
  m = marker();
  vec_init(&d);
  g_offered = !(os_stat(m, &st) == 0 && st.exists) && import_find(&d) > 0;
  free(m);
  vec_free(&d);
  return g_offered;
}
