/*
** evscode.c - where VS Code is: its install, its user folder, its extensions
**
** VS Code (and Insiders, VSCodium) is found by the `code` in the PATH, and
** in the places its installers use. A portable one has a `data` folder
** next to Code.exe: then its settings are in data/user-data/User and its
** extensions in data/extensions, else in %APPDATA%\Code\User (~/.config/
** Code/User ...) and ~/.vscode/extensions. The extensions it comes with
** are in <install>/resources/app/extensions (<install>/<commit>/... since
** 1.9x). mme only reads them: nothing there is ever changed.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int is_dir (const char *p) {
  OsStat st;
  return p && os_stat(p, &st) == 0 && st.exists && st.is_dir;
}


/* d once in v (by its real path); d is taken */
static void add_dir (Vec *v, char *d) {
  char *real;
  size_t i;
  if (!is_dir(d)) {
    free(d);
    return;
  }
  real = os_realpath(d);
  if (real) {
    free(d);
    d = real;
  }
  for (i = 0; i < v->n; i++)
    if (m_fncmp(v->v[i], d) == 0) {
      free(d);
      return;
    }
  vec_push(v, d);
}


static char *env (const char *name) {
  char *s = os_getenv(name);
  if (s && *s == '\0') {
    free(s);
    s = NULL;
  }
  return s;
}


/* a folder with VS Code's program in it? */
static int is_install (const char *dir) {
  static const char *const exe[] = {"Code.exe", "Code - Insiders.exe", "VSCodium.exe", "code", "code-insiders",
                                    "codium", "resources"};
  size_t i;
  for (i = 0; i < sizeof(exe) / sizeof(exe[0]); i++) {
    char *p = path_join(dir, exe[i]);
    OsStat st;
    int ok = os_stat(p, &st) == 0 && st.exists;
    free(p);
    if (ok) return 1;
  }
  return 0;
}


/* the VS Code installs there are: the one of `code` in the PATH first */
int vscode_installs (Vec *out) {
  static const char *const progs[] = {"code", "code-insiders", "codium"};
  size_t i;
  vec_init(out);
  for (i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {	/* <install>/bin/code */
    char *p = find_program(progs[i]), *real, *bin, *inst;
    if (p == NULL) continue;
    real = os_realpath(p);	/* /usr/bin/code -> /usr/share/code/bin/code */
    bin = path_dirname(real ? real : p);
    inst = path_dirname(bin);
    if (is_install(inst)) add_dir(out, inst);
    else free(inst);
    free(bin);
    free(real);
    free(p);
  }
#ifdef _WIN32
  {
    static const char *const sub[] = {"Programs\\Microsoft VS Code", "Programs\\Microsoft VS Code Insiders",
                                      "Programs\\VSCodium"};
    char *la = env("LOCALAPPDATA"), *pf = env("ProgramFiles");
    for (i = 0; i < sizeof(sub) / sizeof(sub[0]); i++) {
      if (la) add_dir(out, path_join(la, sub[i]));
      if (pf) add_dir(out, path_join(pf, sub[i] + 9));	/* "Microsoft VS Code" */
    }
    free(la);
    free(pf);
  }
#elif defined(__APPLE__)
  add_dir(out, xstrdup("/Applications/Visual Studio Code.app/Contents/Resources/app"));
  add_dir(out, xstrdup("/Applications/Visual Studio Code - Insiders.app/Contents/Resources/app"));
  add_dir(out, xstrdup("/Applications/VSCodium.app/Contents/Resources/app"));
#else
  {
    static const char *const dirs[] = {"/usr/share/code", "/usr/share/code-insiders", "/usr/share/codium",
                                       "/opt/visual-studio-code", "/opt/VSCodium", "/usr/lib/code"};
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) add_dir(out, xstrdup(dirs[i]));
  }
#endif
  return (int)out->n;
}


/* a portable install's data folder (<install>/data), NULL: not portable */
static char *portable_data (const char *inst) {
  char *d = path_join(inst, "data");
  if (is_dir(d)) return d;
  free(d);
  return NULL;
}


/* VS Code's User folders (settings.json, keybindings.json, snippets): the portable ones first */
int vscode_user_dirs (Vec *out) {
  static const char *const apps[] = {"Code", "Code - Insiders", "VSCodium"};
  Vec inst;
  size_t i;
  char *base = NULL;
  vec_init(out);
  vscode_installs(&inst);
  for (i = 0; i < inst.n; i++) {
    char *d = portable_data(inst.v[i]);
    if (d) {
      char *u = path_join(d, "user-data" MMC_SEPS "User");
      add_dir(out, u);
      free(d);
    }
  }
  vec_free(&inst);
#ifdef _WIN32
  base = env("APPDATA");
#else
  {
    char *home = env("HOME");
    if (home) {
#ifdef __APPLE__
      base = path_join(home, "Library/Application Support");
#else
      char *xdg = env("XDG_CONFIG_HOME");
      base = xdg ? xstrdup(xdg) : path_join(home, ".config");
      free(xdg);
#endif
      free(home);
    }
  }
#endif
  for (i = 0; base && i < sizeof(apps) / sizeof(apps[0]); i++) {
    char *app = path_join(base, apps[i]);
    add_dir(out, path_join(app, "User"));
    free(app);
  }
  free(base);
  return (int)out->n;
}


/* the folders VS Code installs extensions in: portable data/extensions, ~/.vscode/extensions ... */
int vscode_ext_dirs (Vec *out) {
  static const char *const homes[] = {"USERPROFILE", "HOME"};
  static const char *const sub[] = {".vscode", ".vscode-insiders", ".vscode-oss"};
  Vec inst;
  size_t i, k;
  vec_init(out);
  vscode_installs(&inst);
  for (i = 0; i < inst.n; i++) {
    char *d = portable_data(inst.v[i]);
    if (d) {
      add_dir(out, path_join(d, "extensions"));
      free(d);
    }
  }
  vec_free(&inst);
  for (i = 0; i < sizeof(homes) / sizeof(homes[0]); i++) {
    char *h = env(homes[i]);
    if (h == NULL) continue;
    for (k = 0; k < sizeof(sub) / sizeof(sub[0]); k++) {
      char *v = path_join(h, sub[k]);
      add_dir(out, path_join(v, "extensions"));
      free(v);
    }
    free(h);
  }
  return (int)out->n;
}


/* the extensions an install comes with: <install>/resources/app/extensions, or <install>/<commit>/... */
int vscode_builtin_dirs (Vec *out) {
  Vec inst;
  size_t i, k;
  vec_init(out);
  vscode_installs(&inst);
  for (i = 0; i < inst.n; i++) {
    Vec sub;
    char *d = path_join(inst.v[i], "resources" MMC_SEPS "app" MMC_SEPS "extensions");
    add_dir(out, d);
    d = path_join(inst.v[i], "extensions");	/* macOS: .../Contents/Resources/app/extensions */
    add_dir(out, d);
    vec_init(&sub);
    if (os_listdir(inst.v[i], &sub) == 0)
      for (k = 0; k < sub.n; k++) {	/* the commit's folder */
        char *c = path_join(inst.v[i], sub.v[k]);
        if (strlen(sub.v[k]) >= 8 && strspn(sub.v[k], "0123456789abcdef") == strlen(sub.v[k]))
          add_dir(out, path_join(c, "resources" MMC_SEPS "app" MMC_SEPS "extensions"));
        free(c);
      }
    vec_free(&sub);
  }
  vec_free(&inst);
  return (int)out->n;
}


/*
** An extension's folder by its id's start ("ms-python.debugpy"): in mme's
** extensions, then VS Code's, then its built-in ones; the newest version.
** NULL: none.
*/
char *vscode_find_ext (const char *id) {
  Vec roots, t;
  size_t i, k, n = strlen(id);
  char *best = NULL, *best_name = NULL;
  vec_init(&roots);
  vec_push(&roots, data_path("extensions"));
  vscode_ext_dirs(&t);
  for (i = 0; i < t.n; i++) vec_push(&roots, xstrdup(t.v[i]));
  vec_free(&t);
  vscode_builtin_dirs(&t);
  for (i = 0; i < t.n; i++) vec_push(&roots, xstrdup(t.v[i]));
  vec_free(&t);
  for (i = 0; i < roots.n && best == NULL; i++) {
    Vec v;
    vec_init(&v);
    if (os_listdir(roots.v[i], &v) == 0)
      for (k = 0; k < v.n; k++)
        if (m_strnicmp(v.v[k], id, n) == 0 && (v.v[k][n] == '-' || v.v[k][n] == '\0') &&
            (best_name == NULL || strcmp(v.v[k], best_name) > 0)) {
          free(best);
          free(best_name);
          best = path_join(roots.v[i], v.v[k]);
          best_name = xstrdup(v.v[k]);
        }
    vec_free(&v);
  }
  free(best_name);
  vec_free(&roots);
  return best;
}
