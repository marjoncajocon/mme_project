/*
** etask.c - Tasks: .vscode/tasks.json, like VS Code's
**
** A task is a command line run in a terminal of its own in the panel,
** named after it. tasks.json's tasks come first; without one, tasks are
** found as VS Code's extensions find them: a Makefile's targets, a Go
** module's build and test, package.json's scripts, Cargo's build and
** test. tasks.json is read as VS Code reads it: "shell" and "process"
** tasks, options (cwd, env, shell), the windows / linux / osx parts,
** dependsOn (in parallel, or in sequence), isBackground, presentation and
** the "inputs" its ${input:id} ask for. A task's problem matchers ($gcc,
** $go, $tsc, $msCompile, or one of its own: a regexp and its groups, one
** line or several) read its output: the problems go to the Problems panel;
** a background task's matcher says when it is ready (its endsPattern), and
** what waits for it (a task depending on it, a preLaunchTask) goes on.
** ${workspaceFolder}, ${file} ... are VS Code's variables (vs_subst), for
** launch.json too.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define OS_KEY	"windows"
#elif defined(__APPLE__)
#define OS_KEY	"osx"
#else
#define OS_KEY	"linux"
#endif


/*
** {==================================================================
** Variables
** ===================================================================
*/

static const Json *g_inputs;	/* the "inputs" of the file whose strings are put in */
static Vec g_ans;	/* the inputs answered in this run: id, value, id, value ... */
static int g_cancel;	/* an input was not answered: the run stops */


/* a run begins: the inputs its ${input:id} are of, none answered yet */
void vs_inputs (const Json *inputs) {
  size_t i;
  g_inputs = inputs;
  for (i = 0; i < g_ans.n; i++) free(g_ans.v[i]);
  g_ans.n = 0;
  g_cancel = 0;
}


/* an ${input:...} was not answered (Esc): what was put in must not be used */
int vs_cancelled (void) {
  return g_cancel;
}


static char *rel_to_root (const char *path) {
  const char *root = side_root();
  size_t n = strlen(root);
  if (m_fnncmp(path, root, n) == 0 && path_is_sep(path[n])) return xstrdup(path + n + 1);
  return xstrdup(path);
}


/* ${input:id}: its answer, asked once a run (promptString, pickString); NULL: not answered */
static char *input_value (const char *id) {
  const Json *in = NULL;
  const char *type, *desc, *dflt;
  char *v = NULL;
  size_t i;
  for (i = 0; i + 1 < g_ans.n; i += 2)
    if (strcmp(g_ans.v[i], id) == 0) return xstrdup(g_ans.v[i + 1]);
  if (g_cancel) return NULL;
  for (i = 0; g_inputs && g_inputs->type == J_ARR && i < g_inputs->n; i++)
    if (strcmp(json_str(json_get(g_inputs->kid[i], "id"), ""), id) == 0) in = g_inputs->kid[i];
  if (in == NULL) {
    toast(1, "Undefined input variable '%s' encountered. Remove or define '%s' to continue.", id, id);
    g_cancel = 1;
    return NULL;
  }
  type = json_str(json_get(in, "type"), "");
  desc = json_str(json_get(in, "description"), id);
  dflt = json_str(json_get(in, "default"), "");
  if (strcmp(type, "promptString") == 0) {	/* ask_text, its description said under it as VS Code says it */
    Pick p;
    char hint[600];
    pick_init(&p, desc);
    snprintf(hint, sizeof(hint), "%s (Press 'Enter' to confirm or 'Escape' to cancel)", desc);
    p.hint = hint;
    snprintf(p.text, sizeof(p.text), "%s", dflt);
    p.fresh = *dflt != '\0';
    if (pick_run(&p) == PICK_TEXT) v = xstrdup(p.text);
    pick_free(&p);
  }
  else if (strcmp(type, "pickString") == 0) {
    const Json *o = json_get(in, "options");
    Pick p;
    int r;
    pick_init(&p, desc);
    for (i = 0; o && o->type == J_ARR && i < o->n; i++) {	/* "x", or {"label": ..., "value": "x"} */
      const Json *k = o->kid[i];
      const char *val = k->type == J_OBJ ? json_str(json_get(k, "value"), "") : json_str(k, "");
      const char *lab = k->type == J_OBJ ? json_str(json_get(k, "label"), val) : val;
      pick_add(&p, lab, strcmp(val, dflt) == 0 ? "(Default)" : (strcmp(lab, val) != 0 ? val : NULL), 0);
      if (strcmp(val, dflt) == 0) p.start = (int)i;
    }
    p.keep_order = 1;
    r = pick_run(&p);
    pick_free(&p);
    if (r >= 0) {
      const Json *k = o->kid[r];
      v = xstrdup(k->type == J_OBJ ? json_str(json_get(k, "value"), "") : json_str(k, ""));
    }
  }
  else toast(1, "The input '%s' is of a type mme does not ask (%s)", id, type);
  if (v == NULL) {
    g_cancel = 1;
    return NULL;
  }
  vec_push(&g_ans, xstrdup(id));
  vec_push(&g_ans, xstrdup(v));
  return v;
}


/* ${config:editor.tabSize}: the setting as settings.json has it, else its default, as text */
static void put_config (Buf *b, const char *key) {
  const Json *v = settings_value(key);
  Json *def = NULL;
  if (v == NULL) {
    const char *d = settings_default(key);
    if (d == NULL || (def = json_parse(d, strlen(d))) == NULL) return;
    v = def;
  }
  if (v->type == J_STR) buf_puts(b, v->str);
  else if (v->type == J_NUM) buf_printf(b, "%g", v->num);
  else if (v->type == J_BOOL) buf_puts(b, v->b ? "true" : "false");
  else json_write(b, v);
  json_free(def);
}


/* ${command:pickProcess}: a process of the system's picked, its id (Attach to Process); NULL: none picked */
static char *pick_process (void) {
  Buf out;
  Vec pid = {0};
  Pick p;
  char *line, *nl, *v = NULL;
  int r;
  size_t i;
#ifdef _WIN32
  char *root = os_getenv("SystemRoot"), exe[600];
  char *argv[] = {exe, "/fo", "csv", "/nh", NULL};
  snprintf(exe, sizeof(exe), "%s\\System32\\tasklist.exe", root ? root : "C:\\Windows");
  free(root);
#else
  char *argv[] = {"/bin/ps", "-axo", "pid=,comm=", NULL};
#endif
  buf_init(&out);
  if (run_capture(argv, &out) != 0 || out.len == 0) {
    toast(1, "The processes could not be listed");
    buf_free(&out);
    return NULL;
  }
  pick_init(&p, "Pick the process to attach to");
  for (line = out.s; line && *line; line = nl) {
    char name[256], id[32];
    if ((nl = strchr(line, '\n')) != NULL) *nl++ = '\0';
#ifdef _WIN32
    if (sscanf(line, "\"%255[^\"]\",\"%31[0-9]\"", name, id) != 2) continue;	/* "mme.exe","1234","Console",... */
#else
    if (sscanf(line, " %31[0-9] %255[^\r\n]", id, name) != 2) continue;
#endif
    {
      char d[64];
      snprintf(d, sizeof(d), "process id: %s", id);
      pick_add(&p, path_basename(name), d, 0xEB2C);
    }
    vec_push(&pid, xstrdup(id));
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0 && (size_t)r < pid.n) v = xstrdup(pid.v[r]);
  for (i = 0; i < pid.n; i++) free(pid.v[i]);
  free(pid.v);
  buf_free(&out);
  return v;
}


/*
** ${command:id}: the command run, the string it gives put in (once a run),
** as VS Code does: mme's own that give one (pickProcess), else an
** extension's through the host. A command that gives nothing stops the run
** (a pick not made); one that gives other than a string, or is not there,
** says so. NULL: the run stops.
*/
static char *command_value (const char *id) {
  char key[160], *v = NULL, *r;
  Json *j = NULL;
  size_t i;
  int c = CMD_NONE;
  snprintf(key, sizeof(key), "${command:%s}", id);	/* not an input's id: the same answers keep both */
  for (i = 0; i + 1 < g_ans.n; i += 2)
    if (strcmp(g_ans.v[i], key) == 0) return xstrdup(g_ans.v[i + 1]);
  if (g_cancel) return NULL;
  if (strcmp(id, "pickProcess") == 0) v = pick_process();
  else if ((c = cmd_by_id(id)) != CMD_NONE && c < CMD_N) mme_command(c);	/* mme's own: they give nothing back */
  else if ((r = lsp_ext_command_value(id)) == NULL)
    toast(1, "command '%s' not found", id);
  else {
    if (r[0] == '!') toast(1, "%s", r + 1);	/* it threw */
    else if ((j = json_parse(r, strlen(r))) != NULL && j->type == J_STR) v = xstrdup(j->str);
    else if (j == NULL || j->type != J_NULL)
      toast(1, "Cannot substitute command variable '%s' because command did not return a result of type string.", id);
    else if (c == CMD_NONE) toast(1, "command '%s' not found", id);	/* nothing, from no command the host knows */
    if (r[0] != '!') json_free(j);
    free(r);
  }
  if (v == NULL) {
    g_cancel = 1;
    return NULL;
  }
  vec_push(&g_ans, xstrdup(key));
  vec_push(&g_ans, xstrdup(v));
  return v;
}


/* "${workspaceFolder}/${fileBasenameNoExtension}": VS Code's variables put in */
char *vs_subst (const char *s) {
  const char *file = editor_file(), *root = side_root();
  Buf b;
  buf_init(&b);
  while (*s) {
    const char *e;
    char name[128];
    size_t n;
    if (s[0] != '$' || s[1] != '{' || (e = strchr(s, '}')) == NULL || (n = (size_t)(e - s - 2)) >= sizeof(name)) {
      buf_putc(&b, *s++);
      continue;
    }
    memcpy(name, s + 2, n);
    name[n] = '\0';
    s = e + 1;
    if (strcmp(name, "workspaceFolder") == 0 || strcmp(name, "workspaceRoot") == 0 || strcmp(name, "cwd") == 0 ||
        strcmp(name, "fileWorkspaceFolder") == 0)
      buf_puts(&b, root);
    else if (strcmp(name, "workspaceFolderBasename") == 0) buf_puts(&b, path_basename(root));
    else if (strcmp(name, "pathSeparator") == 0 || strcmp(name, "/") == 0) buf_putc(&b, MMC_SEP);
    else if (strncmp(name, "env:", 4) == 0) {
      char *v = os_getenv(name + 4);
      if (v) buf_puts(&b, v);
      free(v);
    }
    else if (strncmp(name, "config:", 7) == 0) put_config(&b, name + 7);
    else if (strncmp(name, "input:", 6) == 0) {
      char *v = input_value(name + 6);
      if (v) buf_puts(&b, v);
      free(v);
    }
    else if (strncmp(name, "command:", 8) == 0) {
      char *v = command_value(name + 8);
      if (v) buf_puts(&b, v);
      free(v);
    }
    else if (strcmp(name, "userHome") == 0) {
#ifdef _WIN32
      char *v = os_getenv("USERPROFILE");
#else
      char *v = os_getenv("HOME");
#endif
      if (v) buf_puts(&b, v);
      free(v);
    }
    else if (file == NULL) continue;	/* no file in front: the file's variables are empty */
    else if (strcmp(name, "file") == 0) buf_puts(&b, file);
    else if (strcmp(name, "lineNumber") == 0) buf_printf(&b, "%lu", (unsigned long)(editor_line() + 1));
    else if (strcmp(name, "fileBasename") == 0) buf_puts(&b, path_basename(file));
    else if (strcmp(name, "fileBasenameNoExtension") == 0) {
      const char *base = path_basename(file), *dot = strrchr(base, '.');
      buf_putn(&b, base, dot && dot != base ? (size_t)(dot - base) : strlen(base));
    }
    else if (strcmp(name, "fileExtname") == 0) {
      const char *base = path_basename(file), *dot = strrchr(base, '.');
      if (dot && dot != base) buf_puts(&b, dot);
    }
    else if (strcmp(name, "fileDirname") == 0 || strcmp(name, "relativeFileDirname") == 0 ||
             strcmp(name, "fileDirnameBasename") == 0) {
      char *dir = path_dirname(file);
      if (name[0] == 'r') {
        char *r = rel_to_root(dir);
        buf_puts(&b, strcmp(r, dir) == 0 ? "." : r);
        free(r);
      }
      else if (strcmp(name, "fileDirnameBasename") == 0) buf_puts(&b, path_basename(dir));
      else buf_puts(&b, dir);
      free(dir);
    }
    else if (strcmp(name, "relativeFile") == 0) {
      char *r = rel_to_root(file);
      buf_puts(&b, r);
      free(r);
    }
  }
  buf_putc(&b, '\0');
  return buf_take(&b);
}

/* }================================================================== */


/*
** {==================================================================
** The tasks there are
** ===================================================================
*/

enum { PM_NONE, PM_GCC, PM_GO, PM_TSC, PM_MS };

typedef struct Task {
  char *label, *cmd, *cwd, *source;	/* source: "" for tasks.json's, else "make", "go" ...; cmd: a found one's */
  char *detail;	/* tasks.json's "detail": said in Run Task */
  Json *def;	/* tasks.json's: the task as it is on this system (its OS's part, the file's options in it) */
  int build, dflt, test, hide;	/* its group; hide: not in Run Task (it is run as another's dependsOn) */
  int matcher;
  char *custom;	/* an extension's CustomExecution: its id, the host runs it */
} Task;

typedef struct ExtTask {	/* an extension's task (registerTaskProvider), as the host said it */
  char *id, *label, *source, *cmd, *cwd;
  int build, dflt, test, matcher, custom;
} ExtTask;

static ExtTask *g_ext;
static int g_next, g_ext_got;

static Task *g_task;
static int g_ntask;
static Json *g_tj;	/* tasks.json: its "inputs" */


static Task *task_add (const char *label, const char *cmd, const char *cwd, const char *source,
                       int build, int dflt, int test, int matcher) {
  Task *t;
  g_task = (Task *)xrealloc(g_task, (size_t)(g_ntask + 1) * sizeof(Task));
  t = &g_task[g_ntask++];
  memset(t, 0, sizeof(*t));
  t->label = xstrdup(label);
  t->cmd = xstrdup(cmd);
  t->cwd = xstrdup(cwd ? cwd : side_root());
  t->source = xstrdup(source);
  t->build = build;
  t->dflt = dflt;
  t->test = test;
  t->matcher = matcher;
  return t;
}


static void tasks_free (void) {
  int i;
  for (i = 0; i < g_ntask; i++) {
    free(g_task[i].label);
    free(g_task[i].cmd);
    free(g_task[i].cwd);
    free(g_task[i].source);
    free(g_task[i].detail);
    free(g_task[i].custom);
    json_free(g_task[i].def);
  }
  free(g_task);
  g_task = NULL;
  g_ntask = 0;
  json_free(g_tj);
  g_tj = NULL;
}


static int matcher_name (const char *s) {
  if (strcmp(s, "$gcc") == 0) return PM_GCC;
  if (strcmp(s, "$go") == 0) return PM_GO;
  if (strncmp(s, "$tsc", 4) == 0) return PM_TSC;
  if (strcmp(s, "$msCompile") == 0) return PM_MS;
  return PM_NONE;
}


/* a member of an object by its whole name (json_get reads "a.b" as a path) */
static const Json *member (const Json *o, const char *key) {
  size_t i;
  for (i = 0; o && o->type == J_OBJ && i < o->n; i++)
    if (o->kid[i]->key && strcmp(o->kid[i]->key, key) == 0) return o->kid[i];
  return NULL;
}


/* object a with b's members over it (an object in both is merged the same way), as JSON text */
static void merge_put (Buf *out, const Json *a, const Json *b) {
  size_t i;
  int first = 1;
  buf_putc(out, '{');
  for (i = 0; a && a->type == J_OBJ && i < a->n; i++) {
    const Json *o = member(b, a->kid[i]->key);
    if (!first) buf_putc(out, ',');
    first = 0;
    json_put_str(out, a->kid[i]->key, strlen(a->kid[i]->key));
    buf_putc(out, ':');
    if (o && o->type == J_OBJ && a->kid[i]->type == J_OBJ) merge_put(out, a->kid[i], o);
    else json_write(out, o ? o : a->kid[i]);
  }
  for (i = 0; b && b->type == J_OBJ && i < b->n; i++) {
    if (member(a, b->kid[i]->key)) continue;
    if (!first) buf_putc(out, ',');
    first = 0;
    json_put_str(out, b->kid[i]->key, strlen(b->kid[i]->key));
    buf_putc(out, ':');
    json_write(out, b->kid[i]);
  }
  buf_putc(out, '}');
}


static Json *merged (const Json *a, const Json *b) {
  Buf out;
  Json *j;
  buf_init(&out);
  merge_put(&out, a, b);
  j = json_parse(out.s, out.len);
  buf_free(&out);
  return j;
}


/* {"options": o.options, "presentation": o.presentation} */
static Json *op_of (const Json *o) {
  Buf b;
  Json *j;
  buf_init(&b);
  buf_puts(&b, "{\"options\":");
  if (member(o, "options")) json_write(&b, member(o, "options"));
  else buf_puts(&b, "{}");
  buf_puts(&b, ",\"presentation\":");
  if (member(o, "presentation")) json_write(&b, member(o, "presentation"));
  else buf_puts(&b, "{}");
  buf_putc(&b, '}');
  j = json_parse(b.s, b.len);
  buf_free(&b);
  return j;
}


static void load_tasks_json (void) {
  char *dir = path_join(side_root(), ".vscode"), *f = path_join(dir, "tasks.json"), *s;
  size_t len, i;
  Json *top = NULL, *tmp;
  const Json *list;
  free(dir);
  s = read_file(f, &len);
  free(f);
  if (s == NULL) return;
  g_tj = json_parse(s, len);
  free(s);
  if (g_tj == NULL) {
    toast(1, "tasks.json is not valid JSON");
    return;
  }
  {	/* the file's own options and presentation (its OS's part over them): every task's, under its own */
    Json *os = op_of(member(g_tj, OS_KEY));
    tmp = op_of(g_tj);
    top = merged(tmp, os);
    json_free(tmp);
    json_free(os);
  }
  list = json_get(g_tj, "tasks");
  for (i = 0; list && list->type == J_ARR && i < list->n; i++) {
    const Json *g, *def;
    const char *type, *label, *kind;
    Json *d1, *d;
    Task *t;
    char lb[256];
    if (list->kid[i]->type != J_OBJ) continue;
    d1 = merged(top, list->kid[i]);
    d = merged(d1, member(d1, OS_KEY));	/* "windows": {...} over the rest */
    json_free(d1);
    if (d == NULL) continue;
    type = json_str(member(d, "type"), "shell");
    label = json_str(member(d, "label"), json_str(member(d, "taskName"), NULL));
    if (label == NULL && strcmp(type, "npm") == 0 && member(d, "script")) {
      snprintf(lb, sizeof(lb), "npm: %s", json_str(member(d, "script"), ""));
      label = lb;
    }
    if (label == NULL) label = json_str(member(d, "command"), NULL);
    if (label == NULL) {
      json_free(d);
      continue;
    }
    g = member(d, "group");
    kind = g && g->type == J_OBJ ? json_str(json_get(g, "kind"), "") : json_str(g, "");
    t = task_add(label, "", NULL, "", strcmp(kind, "build") == 0, 0, strcmp(kind, "test") == 0, PM_NONE);
    if (g && g->type == J_OBJ) {
      def = json_get(g, "isDefault");
      t->dflt = def && (def->type == J_STR || json_bool(def, 0));
    }
    t->hide = json_bool(member(d, "hide"), 0);
    if (member(d, "detail")) t->detail = xstrdup(json_str(member(d, "detail"), ""));
    t->def = d;
  }
  json_free(top);
}


static int exists (const char *name) {
  char *f = path_join(side_root(), name);
  OsStat st;
  int r = os_stat(f, &st) == 0 && st.exists;
  free(f);
  return r;
}


/* a Makefile's targets: "name:" at the start of a line, not .PHONY, %.o or VAR := */
static void detect_make (void) {
  static const char *const names[] = {"Makefile", "makefile", "GNUmakefile"};
  size_t i, len;
  char *f = NULL, *s = NULL, *p, *line;
  int n = 0;
  for (i = 0; i < 3 && s == NULL; i++) {
    f = path_join(side_root(), names[i]);
    s = read_file(f, &len);
    free(f);
  }
  if (s == NULL) return;
  task_add("make", "make", NULL, "make", 1, 0, 0, PM_GCC);
  for (p = s; *p && n < 40; p = line) {
    char *e = p, *colon;
    line = strchr(p, '\n');
    if (line) *line++ = '\0';
    else line = p + strlen(p);
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) continue;
    while (*e && (*e == '_' || *e == '-' || *e == '.' || *e == '/' || (*e >= 'a' && *e <= 'z') ||
                  (*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9'))) e++;
    colon = e;
    while (*colon == ' ') colon++;
    if (*colon != ':' || colon[1] == '=') continue;
    *e = '\0';
    {
      char label[160], cmd[160];
      snprintf(label, sizeof(label), "make: %s", p);
      snprintf(cmd, sizeof(cmd), "make %s", p);
      task_add(label, cmd, NULL, "make", 0, 0, strcmp(p, "test") == 0, PM_GCC);
      n++;
    }
  }
  free(s);
}


static void detect_npm (void) {
  char *f = path_join(side_root(), "package.json"), *s;
  size_t len, i;
  Json *j;
  const Json *sc;
  s = read_file(f, &len);
  free(f);
  if (s == NULL) return;
  j = json_parse(s, len);
  free(s);
  sc = json_get(j, "scripts");
  for (i = 0; sc && sc->type == J_OBJ && i < sc->n; i++) {
    char label[160], cmd[160];
    snprintf(label, sizeof(label), "npm: %s", sc->kid[i]->key);
    snprintf(cmd, sizeof(cmd), "npm run %s", sc->kid[i]->key);
    task_add(label, cmd, NULL, "npm", strcmp(sc->kid[i]->key, "build") == 0, 0,
             strcmp(sc->kid[i]->key, "test") == 0, PM_TSC);
  }
  json_free(j);
}


static void load_tasks (void) {
  tasks_free();
  load_tasks_json();
  detect_make();
  if (exists("go.mod")) {
    task_add("go: build package", "go build ./...", NULL, "go", 1, 0, 0, PM_GO);
    task_add("go: test package", "go test ./...", NULL, "go", 0, 0, 1, PM_GO);
    task_add("go: run", "go run .", NULL, "go", 0, 0, 0, PM_GO);
  }
  detect_npm();
  if (exists("Cargo.toml")) {
    task_add("rust: cargo build", "cargo build", NULL, "cargo", 1, 0, 0, PM_GCC);
    task_add("rust: cargo test", "cargo test", NULL, "cargo", 0, 0, 1, PM_GCC);
    task_add("rust: cargo run", "cargo run", NULL, "cargo", 0, 0, 0, PM_GCC);
  }
  {	/* the extensions' (registerTaskProvider), but not one found above by its name */
    int i, k, dup;
    for (i = 0; i < g_next; i++) {
      const ExtTask *e = &g_ext[i];
      char *cmd, *cwd;
      for (dup = 0, k = 0; k < g_ntask && !dup; k++) dup = strcmp(g_task[k].label, e->label) == 0;
      if (dup) continue;
      cmd = vs_subst(e->cmd);
      cwd = e->cwd[0] ? vs_subst(e->cwd) : NULL;
      task_add(e->label, cmd, cwd, e->source[0] ? e->source : "extension", e->build, e->dflt, e->test, e->matcher);
      if (e->custom) g_task[g_ntask - 1].custom = xstrdup(e->id);
      free(cmd);
      free(cwd);
    }
  }
}


static void ext_free (void) {
  int i;
  for (i = 0; i < g_next; i++) {
    free(g_ext[i].id);
    free(g_ext[i].label);
    free(g_ext[i].source);
    free(g_ext[i].cmd);
    free(g_ext[i].cwd);
  }
  free(g_ext);
  g_ext = NULL;
  g_next = 0;
}


static void ext_one (ExtTask *e, const Json *t) {
  const char *g = json_str(json_get(t, "group"), "");
  e->id = xstrdup(json_str(json_get(t, "id"), ""));
  e->label = xstrdup(json_str(json_get(t, "label"), ""));
  e->source = xstrdup(json_str(json_get(t, "source"), ""));
  e->cmd = xstrdup(json_str(json_get(t, "cmd"), ""));
  e->cwd = xstrdup(json_str(json_get(t, "cwd"), ""));
  e->build = strcmp(g, "build") == 0;
  e->test = strcmp(g, "test") == 0;
  e->dflt = json_bool(json_get(t, "dflt"), 0);
  e->matcher = matcher_name(json_str(json_get(t, "matcher"), ""));
  e->custom = json_bool(json_get(t, "custom"), 0);
}


/* mme/tasks: the extensions' tasks, for Run Task */
void task_ext_list (const Json *tasks) {
  size_t i;
  ext_free();
  if (tasks && tasks->type == J_ARR && tasks->n) {
    g_ext = (ExtTask *)xmalloc(tasks->n * sizeof(ExtTask));
    for (i = 0; i < tasks->n; i++) ext_one(&g_ext[g_next++], tasks->kid[i]);
  }
  g_ext_got = 1;
}


static int ext_got (void) {
  return g_ext_got;
}


/* the extensions' tasks asked for again: the host answers at once, it is waited for a moment */
static void ask_ext_tasks (void) {
  if (!lsp_running(EXT_LANG)) return;
  g_ext_got = 0;
  lsp_ext_notify("mme/provideTasks", "{}");
  lsp_ext_wait(2500, ext_got);
}

/* }================================================================== */


/*
** {==================================================================
** Problem matchers
** ===================================================================
*/

typedef struct Pat {	/* a line of a problem matcher's pattern: its regexp, which group is what (0 none) */
  Regex *re;
  int file, loc, line, col, eline, ecol, sev, code, msg;
  int loop;	/* the last of several: the lines after it that match are problems too */
} Pat;

typedef struct Matcher {
  int builtin;	/* PM_*: mme's own reading of $gcc ... ; PM_NONE: pat */
  Pat *pat;
  int npat;
  int sev;	/* "severity": when the pattern says none (1 error, 2 warning, 3 info) */
  int loc;	/* "fileLocation": 0 relative, 1 absolute, 2 autoDetect */
  char *base;	/* relative to this folder */
  Regex *begins, *ends;	/* "background": a background task's cycle begins, ends (it is ready) */
  int at;	/* the pattern the next line is for (several lines) */
  char *file, *msg;	/* what the lines so far said */
  size_t line, col, eline, ecol;
  int lsev;
} Matcher;

typedef struct Prob {
  char *path;
  Diag d;
  int serial;	/* the job whose output it was (Job.serial) */
  char *label;	/* its task */
} Prob;

static Prob *g_prob;
static size_t g_nprob;


static void matcher_free (Matcher *m) {
  int i;
  for (i = 0; i < m->npat; i++) re_free(m->pat[i].re);
  free(m->pat);
  free(m->base);
  if (m->begins) re_free(m->begins);
  if (m->ends) re_free(m->ends);
  free(m->file);
  free(m->msg);
}


/* "regexp" (or a string that is one) compiled; NULL when there is none or it is not one mme reads */
static Regex *rx_of (const Json *j) {
  const char *s = json_str(j->type == J_OBJ ? json_get(j, "regexp") : j, NULL), *err;
  Regex *re;
  if (s == NULL || *s == '\0') return NULL;
  re = re_compile(s, 0, &err);
  if (re == NULL) toast(1, "A problem matcher's regexp is not one mme reads: %s", err ? err : s);
  return re;
}


/* does re match s somewhere? its groups in cap[20] */
static int rx_match (const Regex *re, const char *s, size_t *cap) {
  size_t n = strlen(s), i, end, c[20];
  for (i = 0; i <= n; i++) {
    if (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) continue;
    if (re_at(re, s, n, i, &end, cap ? cap : c)) return 1;
  }
  return 0;
}


static int pat_group (const Json *p, const char *key, int def) {
  const Json *g = json_get(p, key);
  return g ? (int)json_num(g, def) : def;
}


static void pat_add (Matcher *m, const Json *p, int single) {
  Pat *x;
  Regex *re = rx_of(p);
  if (re == NULL) return;
  m->pat = (Pat *)xrealloc(m->pat, (size_t)(m->npat + 1) * sizeof(Pat));
  x = &m->pat[m->npat++];
  memset(x, 0, sizeof(*x));
  x->re = re;
  x->file = pat_group(p, "file", single ? 1 : 0);	/* one line: VS Code's defaults, file 1, line 2, column 3 */
  x->loc = pat_group(p, "location", 0);
  x->line = pat_group(p, "line", single && !x->loc ? 2 : 0);
  x->col = pat_group(p, "column", single && !x->loc ? 3 : 0);
  x->eline = pat_group(p, "endLine", 0);
  x->ecol = pat_group(p, "endColumn", 0);
  x->sev = pat_group(p, "severity", 0);
  x->code = pat_group(p, "code", 0);
  x->msg = pat_group(p, "message", single ? 0 : -1);	/* -1: this line says no message */
  x->loop = json_bool(json_get(p, "loop"), 0);
}


static int sev_word (const char *s, int def) {
  if (s == NULL || *s == '\0') return def;
  if ((s[0] == 'e' || s[0] == 'E') && (s[1] == 'r' || s[1] == 'R')) return 1;
  if (s[0] == 'w' || s[0] == 'W') return 2;
  if (s[0] == 'i' || s[0] == 'I' || s[0] == 'n' || s[0] == 'N' || s[0] == 'h' || s[0] == 'H') return 3;
  return def;
}


/*
** A problemMatcher of tasks.json into the list: "$gcc", or an object of its
** own (owner, pattern, fileLocation, severity, background, base), or an
** array of them.
*/
static void matchers_of (Matcher **v, int *n, const Json *pm) {
  Matcher m;
  size_t i;
  if (pm == NULL) return;
  if (pm->type == J_ARR) {
    for (i = 0; i < pm->n; i++) matchers_of(v, n, pm->kid[i]);
    return;
  }
  memset(&m, 0, sizeof(m));
  m.sev = 1;
  if (pm->type == J_STR) {
    m.builtin = matcher_name(pm->str);
    if (strcmp(pm->str, "$tsc-watch") == 0) {	/* tsc --watch: ready when it says it watches */
      const char *err;
      m.begins = re_compile("(Starting compilation in watch mode|File change detected\\. Starting incremental compilation)", 0, &err);
      m.ends = re_compile("(Found \\d+ errors?\\. Watching for file changes|Watching for file changes)", 0, &err);
    }
    if (m.builtin == PM_NONE) return;
  }
  else if (pm->type == J_OBJ) {
    const Json *p = json_get(pm, "pattern"), *fl = json_get(pm, "fileLocation"), *bg = json_get(pm, "background");
    const char *base = json_str(json_get(pm, "base"), NULL), *where;
    if (base) m.builtin = matcher_name(base);
    if (p && p->type == J_OBJ) pat_add(&m, p, 1);
    else if (p && p->type == J_ARR)
      for (i = 0; i < p->n; i++) pat_add(&m, p->kid[i], p->n == 1);
    if (m.npat) m.builtin = PM_NONE;	/* its own pattern: base's reading is not used */
    m.sev = sev_word(json_str(json_get(pm, "severity"), NULL), 1);
    where = fl && fl->type == J_ARR && fl->n ? json_str(fl->kid[0], "relative") : json_str(fl, "relative");
    m.loc = strcmp(where, "absolute") == 0 ? 1 : strcmp(where, "autoDetect") == 0 ? 2 : 0;
    if (fl && fl->type == J_ARR && fl->n > 1 && fl->kid[1]->type == J_STR) m.base = vs_subst(fl->kid[1]->str);
    if (bg && bg->type == J_OBJ) {
      if (json_get(bg, "beginsPattern")) m.begins = rx_of(json_get(bg, "beginsPattern"));
      if (json_get(bg, "endsPattern")) m.ends = rx_of(json_get(bg, "endsPattern"));
    }
    if (m.builtin == PM_NONE && m.npat == 0 && m.begins == NULL && m.ends == NULL) {
      matcher_free(&m);
      return;
    }
  }
  else return;
  *v = (Matcher *)xrealloc(*v, (size_t)(*n + 1) * sizeof(Matcher));
  (*v)[(*n)++] = m;
}

/* }================================================================== */


/*
** {==================================================================
** Running them: jobs
** ===================================================================
*/

/*
** A job is a task being run: the one asked for, and those it dependsOn,
** each a job of its own (a job already running is waited for, not run
** again). NEW: made; WAIT: its dependencies run; RUN: its command runs;
** READY: a background task that said it is ready (it runs on); OK, FAIL:
** it ended. task_poll moves them on.
*/
enum { JS_NEW, JS_WAIT, JS_RUN, JS_READY, JS_OK, JS_FAIL };

typedef struct Job {
  int used;
  int serial;	/* its number (its problems say it) */
  char *label;
  int state, code;
  int top;	/* the job asked for, that this one is part of */
  int *dep, ndep, next, seq;	/* dependsOn; next: in sequence, the one running */
  char **argv;	/* its program and arguments, NULL: nothing to run (dependsOn only) */
  char *echo, *cwd, *custom;	/* echo: what " *  Executing task:" says; custom: an extension runs it */
  Vec env;	/* options.env: name, value, name, value ... */
  int bg;	/* isBackground */
  int flags;	/* TT_* and PR_*: presentation */
  int silent;	/* presentation.reveal "silent": the terminal comes to the front when it fails */
  int quiet;	/* runInTerminal's: no word when it ends */
  int term;	/* its terminal's id (task_output ...), 0 none */
  Matcher *m;
  int nm;
  Buf line;
  int exited, exit_code, ready;	/* what its terminal said, for task_poll */
  int told;	/* the one asked for: cb was told */
  TaskCb cb;
  void *ud;
} Job;

static Job *g_job;
static int g_njob;
static int g_next_id = 1;
static int g_made[256], g_nmade;	/* the jobs job_make made in this run (they go if it cannot start) */
static int g_serial;	/* each job's number, never used again */


static int is_made (int j) {
  int m;
  for (m = 0; m < g_nmade; m++)
    if (g_made[m] == j) return 1;
  return 0;
}


static void job_free (int j) {
  Job *x = &g_job[j];
  int i;
  free(x->label);
  free(x->dep);
  for (i = 0; x->argv && x->argv[i]; i++) free(x->argv[i]);
  free(x->argv);
  free(x->echo);
  free(x->cwd);
  free(x->custom);
  vec_free(&x->env);
  for (i = 0; i < x->nm; i++) matcher_free(&x->m[i]);
  free(x->m);
  buf_free(&x->line);
  memset(x, 0, sizeof(*x));
}


static int job_new (const char *label) {
  int j;
  for (j = 0; j < g_njob && g_job[j].used; j++) ;
  if (j == g_njob) g_job = (Job *)xrealloc(g_job, (size_t)++g_njob * sizeof(Job));
  memset(&g_job[j], 0, sizeof(Job));
  g_job[j].used = 1;
  g_job[j].label = xstrdup(label);
  g_job[j].top = j;
  g_job[j].serial = ++g_serial;
  if (g_nmade < 256) g_made[g_nmade++] = j;
  vec_init(&g_job[j].env);
  buf_init(&g_job[j].line);
  return j;
}


static int active (const Job *x) {
  return x->used && (x->state == JS_WAIT || x->state == JS_RUN || x->state == JS_READY);
}


/* a job ended long ago goes, when no job waits for it */
static void jobs_tidy (void) {
  int j, k, i, held;
  for (j = 0; j < g_njob; j++) {
    const Job *x = &g_job[j];
    if (!x->used || x->state == JS_NEW || active(x) || (x->top == j && !x->told)) continue;
    for (held = 0, k = 0; k < g_njob && !held; k++)
      if (g_job[k].used && (g_job[k].state == JS_WAIT || g_job[k].state == JS_NEW || !g_job[g_job[k].top].told))
        for (i = 0; i < g_job[k].ndep; i++) held |= g_job[k].dep[i] == j;
    if (!held) job_free(j);
  }
}


/* a word of a command line, quoted when it must be */
static void put_arg (Buf *b, const char *a) {
  if (*a && strpbrk(a, " \t\"&|<>") == NULL) {
    buf_puts(b, a);
    return;
  }
  buf_putc(b, '"');
  for (; *a; a++) {
    if (*a == '"') buf_putc(b, '\\');
    buf_putc(b, *a);
  }
  buf_putc(b, '"');
}


/* a command's or argument's text: "x", or {"value": "x", "quoting": ...}; put in */
static char *word_of (const Json *w) {
  if (w && w->type == J_OBJ) w = json_get(w, "value");
  if (w && w->type == J_ARR) {	/* a value in parts: joined by spaces */
    Buf b;
    size_t i;
    char *r;
    buf_init(&b);
    for (i = 0; i < w->n; i++) {
      if (i) buf_putc(&b, ' ');
      buf_puts(&b, json_str(w->kid[i], ""));
    }
    buf_putc(&b, '\0');
    r = vs_subst(b.s);
    buf_free(&b);
    return r;
  }
  if (w && w->type == J_NUM) {
    char n[64];
    snprintf(n, sizeof(n), "%g", w->num);
    return xstrdup(n);
  }
  return vs_subst(json_str(w, ""));
}


static char **argv_new (Vec *v) {
  char **a = (char **)xmalloc((v->n + 1) * sizeof(char *));
  size_t i;
  for (i = 0; i < v->n; i++) a[i] = v->v[i];
  a[v->n] = NULL;
  free(v->v);
  vec_init(v);
  return a;
}


/* a program by its name ("python", "powershell.exe"): where PATH has it, else the name as it is */
static char *program_path (const char *name) {
  char *found, *bare;
  size_t n = strlen(name);
  if (strchr(name, '/') || strchr(name, '\\')) return xstrdup(name);
  bare = n > 4 && m_fncmp(name + n - 4, ".exe") == 0 ? xstrndup(name, n - 4) : xstrdup(name);
  found = find_program(bare);
  free(bare);
  return found ? found : xstrdup(name);
}


/* the shell's words before the command line: options.shell, else the system's shell */
static void shell_words (Vec *v, const Json *sh) {
  const char *exe = json_str(json_get(sh, "executable"), NULL);
  const Json *args = json_get(sh, "args");
  size_t i;
  if (exe == NULL || *exe == '\0') {
#ifdef _WIN32
    char *c = os_getenv("ComSpec");
    vec_push(v, c ? c : xstrdup("C:\\Windows\\System32\\cmd.exe"));
    vec_push(v, xstrdup("/d"));
    vec_push(v, xstrdup("/c"));
#else
    vec_push(v, xstrdup("/bin/sh"));
    vec_push(v, xstrdup("-c"));
#endif
    return;
  }
  {
    char *e = vs_subst(exe), *found = program_path(e);
    const char *base = path_basename(found);
    vec_push(v, found);
    if (args && args->type == J_ARR)
      for (i = 0; i < args->n; i++) vec_push(v, word_of(args->kid[i]));
    else if (m_fnncmp(base, "cmd", 3) == 0) {	/* each shell's own way to run a line */
      vec_push(v, xstrdup("/d"));
      vec_push(v, xstrdup("/c"));
    }
    else if (m_fnncmp(base, "powershell", 10) == 0 || m_fnncmp(base, "pwsh", 4) == 0) vec_push(v, xstrdup("-Command"));
    else vec_push(v, xstrdup("-c"));
    free(e);
  }
}


/*
** How job j runs task t, every variable put in (an ${input:...} is asked
** here): its program and arguments, folder, environment, problem matchers
** and presentation. -1: an input was not answered.
*/
static int job_resolve (int j, const Task *t) {
  Job *x = &g_job[j];
  const Json *d = t->def, *o, *pr;
  Vec av;
  vec_init(&av);
  if (t->custom) x->custom = xstrdup(t->custom);
  if (d == NULL) {	/* one found (make, npm ...), or an extension's: a command line for the shell */
    x->cwd = xstrdup(t->cwd);
    if (t->cmd[0]) {
      shell_words(&av, NULL);
      vec_push(&av, xstrdup(t->cmd));
      x->argv = argv_new(&av);
      x->echo = xstrdup(t->cmd);
    }
    if (t->matcher != PM_NONE) {
      Matcher m;
      memset(&m, 0, sizeof(m));
      m.builtin = t->matcher;
      m.sev = 1;
      x->m = (Matcher *)xmalloc(sizeof(Matcher));
      x->m[0] = m;
      x->nm = 1;
    }
    return 0;
  }
  {
    const char *type = json_str(member(d, "type"), "shell");
    const Json *cmd = member(d, "command"), *args = member(d, "args");
    size_t i;
    Buf line;
    buf_init(&line);
    if (strcmp(type, "npm") == 0 && member(d, "script")) {
      buf_puts(&line, "npm run ");
      buf_puts(&line, json_str(member(d, "script"), ""));
      type = "shell";
    }
    else if (cmd) {
      char *c = word_of(cmd);
      if (strcmp(type, "shell") == 0) buf_puts(&line, c);
      else {	/* "process" (and a C/C++ extension's cppbuild ...): the program itself, no shell */
        vec_push(&av, program_path(c));
        put_arg(&line, c);
      }
      free(c);
      for (i = 0; args && args->type == J_ARR && i < args->n; i++) {
        char *a = word_of(args->kid[i]);
        buf_putc(&line, ' ');
        if (strcmp(type, "shell") == 0) put_arg(&line, a);
        else {
          put_arg(&line, a);
          vec_push(&av, xstrdup(a));
        }
        free(a);
      }
    }
    buf_putc(&line, '\0');
    if (line.s[0]) {
      if (strcmp(type, "shell") == 0) {
        shell_words(&av, json_get(d, "options.shell"));
        vec_push(&av, xstrdup(line.s));
      }
      x->argv = argv_new(&av);
      x->echo = xstrdup(line.s);
    }
    else vec_free(&av);
    buf_free(&line);
  }
  o = member(d, "options");
  x->cwd = member(o, "cwd") ? word_of(member(o, "cwd")) : xstrdup(side_root());
  {
    const Json *env = member(o, "env");
    size_t i;
    for (i = 0; env && env->type == J_OBJ && i < env->n; i++) {
      vec_push(&x->env, xstrdup(env->kid[i]->key));
      vec_push(&x->env, word_of(env->kid[i]));
    }
  }
  x->bg = json_bool(member(d, "isBackground"), 0);
  pr = member(d, "presentation");
  {
    const char *reveal = json_str(member(pr, "reveal"), "always"), *panel = json_str(member(pr, "panel"), "shared");
    x->flags = strcmp(panel, "dedicated") == 0 ? PR_DEDICATED : strcmp(panel, "new") == 0 ? PR_NEW : PR_SHARED;
    if (strcmp(reveal, "always") != 0) x->flags |= TT_HIDE;
    x->silent = strcmp(reveal, "silent") == 0;
    if (json_bool(member(pr, "focus"), 0)) x->flags |= TT_FOCUS;
    if (json_bool(member(pr, "clear"), 0)) x->flags |= TT_CLEAR;	/* else a terminal used again keeps what it showed */
    if (json_bool(member(pr, "close"), 0)) x->flags |= TT_CLOSE;
    if (!json_bool(member(pr, "showReuseMessage"), 1)) x->flags |= TT_NOREUSEMSG;
    if (!json_bool(member(pr, "echo"), 1)) {
      free(x->echo);
      x->echo = NULL;
    }
  }
  matchers_of(&x->m, &x->nm, member(d, "problemMatcher"));
  return vs_cancelled() ? -1 : 0;
}


static int task_index (const char *label) {
  int i;
  for (i = 0; i < g_ntask; i++)
    if (strcmp(g_task[i].label, label) == 0) return i;
  return -1;
}


/* a dependsOn's name: "label", or {"type": "npm", "script": "build"}, or {"label": ...} */
static const char *dep_label (const Json *d, char *buf, size_t cap) {
  if (d->type == J_STR) return d->str;
  if (json_get(d, "label")) return json_str(json_get(d, "label"), "");
  if (json_get(d, "task")) return json_str(json_get(d, "task"), "");
  if (json_get(d, "script")) {
    snprintf(buf, cap, "%s: %s", json_str(json_get(d, "type"), "npm"), json_str(json_get(d, "script"), ""));
    return buf;
  }
  return "";
}


static int job_make (int t, int top, int depth);

/* the job of task label within run top: one already running is waited for; <0: it cannot be */
static int dep_job (const char *label, int top, int depth) {
  int j, t;
  for (j = 0; j < g_njob; j++)	/* this run's already, or one running */
    if (g_job[j].used && strcmp(g_job[j].label, label) == 0 && (g_job[j].top == top || active(&g_job[j]))) return j;
  if ((t = task_index(label)) < 0) {
    toast(1, "Couldn't resolve dependent task '%s' in workspace folder '%s'", label, path_basename(side_root()));
    return -1;
  }
  return job_make(t, top, depth);
}


/* the job of g_task[t] and of what it dependsOn, not started; <0: they cannot be (said) */
static int job_make (int t, int top, int depth) {
  int j = job_new(g_task[t].label);
  const Json *deps;
  if (top < 0) top = j;
  g_job[j].top = top;
  if (depth > 16) {
    toast(1, "The task '%s' depends on itself (dependsOn)", g_task[t].label);
    return -1;
  }
  if (job_resolve(j, &g_task[t]) != 0) return -2;
  deps = member(g_task[t].def, "dependsOn");
  g_job[j].seq = strcmp(json_str(member(g_task[t].def, "dependsOrder"), "parallel"), "sequence") == 0;
  if (deps && (deps->type == J_STR || deps->type == J_ARR)) {
    size_t n = deps->type == J_ARR ? deps->n : 1, i;
    g_job[j].dep = (int *)xmalloc((n + 1) * sizeof(int));
    for (i = 0; i < n; i++) {
      char buf[256];
      int d = dep_job(dep_label(deps->type == J_ARR ? deps->kid[i] : deps, buf, sizeof(buf)), top, depth + 1);
      if (d < 0) return d;
      if (d == j) {
        toast(1, "The task '%s' depends on itself (dependsOn)", g_task[t].label);
        return -1;
      }
      g_job[j].dep[g_job[j].ndep++] = d;
    }
  }
  return j;
}


/* the problems of the tasks this run runs again go (their output is old); the other tasks' stay */
static void probs_tidy (void) {
  size_t i, k = 0;
  for (i = 0; i < g_nprob; i++) {
    int m, again = 0, j;
    for (m = 0; m < g_nmade && !again; m++) again = strcmp(g_job[g_made[m]].label, g_prob[i].label) == 0;
    for (j = 0; j < g_njob && again; j++)	/* a background one that still runs keeps its own */
      if (active(&g_job[j]) && g_job[j].serial == g_prob[i].serial && !is_made(j)) again = 0;
    if (!again) g_prob[k++] = g_prob[i];
    else {
      free(g_prob[i].path);
      free(g_prob[i].label);
      free(g_prob[i].d.msg);
    }
  }
  g_nprob = k;
  lsp_task_clear();
  for (i = 0; i < g_nprob; i++) {	/* those left are said again */
    size_t m, n = 0;
    Diag *v = (Diag *)xmalloc((g_nprob + 1) * sizeof(Diag));
    for (m = 0; m < i && m_fncmp(g_prob[m].path, g_prob[i].path) != 0; m++) ;
    if (m < i) {
      free(v);
      continue;
    }
    for (m = i; m < g_nprob; m++)
      if (m_fncmp(g_prob[m].path, g_prob[i].path) == 0) v[n++] = g_prob[m].d;
    lsp_task_diags(g_prob[i].path, v, n);
    free(v);
  }
}


static void report_file (const char *path) {
  size_t i, n = 0;
  Diag *v = (Diag *)xmalloc((g_nprob + 1) * sizeof(Diag));
  for (i = 0; i < g_nprob; i++)
    if (m_fncmp(g_prob[i].path, path) == 0) v[n++] = g_prob[i].d;
  lsp_task_diags(path, v, n);
  free(v);
}


/* job j's problems go (a background task began again) */
static void probs_drop (int j) {
  size_t i, k = 0;
  Vec gone;
  vec_init(&gone);
  for (i = 0; i < g_nprob; i++) {
    if (g_prob[i].serial != g_job[j].serial) {
      g_prob[k++] = g_prob[i];
      continue;
    }
    vec_push(&gone, g_prob[i].path);
    free(g_prob[i].label);
    free(g_prob[i].d.msg);
  }
  g_nprob = k;
  for (i = 0; i < gone.n; i++) report_file(gone.v[i]);
  vec_free(&gone);
}


static int job_errors (void) {
  size_t i;
  int n = 0;
  for (i = 0; i < g_nprob; i++) n += g_prob[i].d.sev == 1;
  return n;
}


static void add_problem (int j, const Matcher *m, const char *path, size_t ln, size_t col, size_t eln, size_t ecol,
                         int sev, const char *msg) {
  OsStat st;
  char *full, *real;
  const char *base = m && m->base ? m->base : m && m->builtin == PM_NONE ? side_root() : g_job[j].cwd;
  int abs = path_is_sep(path[0]) || (path[0] && path[1] == ':');
  size_t i;
  if (m && m->loc == 1) full = xstrdup(path);	/* absolute */
  else if (abs) full = xstrdup(path);
  else full = path_join(base, path);
  real = os_realpath(full);
  free(full);
  if ((real == NULL || os_stat(real, &st) != 0 || !st.exists) && m && m->loc == 2) {	/* autoDetect: as it is */
    free(real);
    real = os_realpath(path);
  }
  if (real == NULL || os_stat(real, &st) != 0 || !st.exists || st.is_dir) {	/* only files that are there */
    free(real);
    return;
  }
  for (i = 0; i < g_nprob; i++)	/* the same one again (the terminal may paint a line twice) */
    if (g_prob[i].d.a.y + 1 == (ln ? ln : 1) && g_prob[i].d.a.x + 1 == (col ? col : 1) &&
        strcmp(g_prob[i].d.msg, msg) == 0 && m_fncmp(g_prob[i].path, real) == 0) {
      free(real);
      return;
    }
  g_prob = (Prob *)xrealloc(g_prob, (g_nprob + 1) * sizeof(Prob));
  g_prob[g_nprob].path = real;
  g_prob[g_nprob].serial = g_job[j].serial;
  g_prob[g_nprob].label = xstrdup(g_job[j].label);
  g_prob[g_nprob].d.a.y = ln > 0 ? ln - 1 : 0;
  g_prob[g_nprob].d.a.x = col > 0 ? col - 1 : 0;
  g_prob[g_nprob].d.b.y = eln > 0 ? eln - 1 : g_prob[g_nprob].d.a.y;
  g_prob[g_nprob].d.b.x = ecol > 0 ? ecol - 1 : g_prob[g_nprob].d.a.x + 1;
  if (g_prob[g_nprob].d.b.y == g_prob[g_nprob].d.a.y && g_prob[g_nprob].d.b.x <= g_prob[g_nprob].d.a.x)
    g_prob[g_nprob].d.b.x = g_prob[g_nprob].d.a.x + 1;
  g_prob[g_nprob].d.sev = sev;
  g_prob[g_nprob].d.msg = xstrdup(msg);
  g_nprob++;
  report_file(real);
}


/* is s "12" or "12,5" digits?; their values */
static const char *digits (const char *s, size_t *v) {
  if (*s < '0' || *s > '9') return NULL;
  *v = 0;
  while (*s >= '0' && *s <= '9') *v = *v * 10 + (size_t)(*s++ - '0');
  return s;
}


/*
** "file:12:5: error: msg" ($gcc, $go), "file(12,5): error TS1005: msg"
** ($tsc, $msCompile): the problem of a line of output; 0 when it is none
*/
static int parse_problem (int matcher, const char *s, char **path, size_t *ln, size_t *col, int *sev, const char **msg) {
  const char *p, *q, *start = s;
  while (*start == ' ' || *start == '\t') start++;
  if (matcher == PM_MS)	/* "  3>file(...)" */
    for (p = start; *p >= '0' && *p <= '9'; p++)
      if (p[1] == '>') start = p + 2;
  for (p = start; *p; p++) {
    size_t a = 0, b = 0;
    if (p == start || (p == start + 1 && *p == ':')) continue;	/* "C:\..." */
    if (*p == ':' && (q = digits(p + 1, &a)) != NULL) {	/* file:line[:col]: */
      if (*q == ':' && digits(q + 1, &b)) {
        q = digits(q + 1, &b);
        if (*q != ':') continue;
      }
      else if (*q != ':') continue;
      *path = xstrndup(start, (size_t)(p - start));
      *ln = a;
      *col = b;
      q++;
    }
    else if (*p == '(' && (q = digits(p + 1, &a)) != NULL) {	/* file(line[,col]): */
      if (*q == ',') q = digits(q + 1, &b);
      if (q == NULL || q[0] != ')' || q[1] != ':') continue;
      *path = xstrndup(start, (size_t)(p - start));
      *ln = a;
      *col = b;
      q += 2;
    }
    else continue;
    while (*q == ' ') q++;
    *sev = 1;
    if (strncmp(q, "fatal error", 11) == 0) q += 11;
    else if (strncmp(q, "error", 5) == 0) q += 5;
    else if (strncmp(q, "warning", 7) == 0) *sev = 2, q += 7;
    else if (strncmp(q, "note", 4) == 0 || strncmp(q, "info", 4) == 0) *sev = 3, q += 4;
    else if (matcher == PM_GCC || matcher == PM_TSC || matcher == PM_MS) {	/* those say what it is */
      free(*path);
      return 0;
    }
    while (*q == ' ') q++;
    if ((matcher == PM_TSC || matcher == PM_MS) && *q != ':') {	/* the code: TS1005 */
      const char *c = q;
      while (*c && *c != ':' && *c != ' ') c++;
      if (*c == ':') q = c;
    }
    while (*q == ':' || *q == ' ') q++;
    *msg = q;
    return 1;
  }
  return 0;
}


static char *group_of (const char *s, const size_t *cap, int g) {
  if (g <= 0 || g > 9 || cap[g * 2] == (size_t)-1 || cap[g * 2 + 1] == (size_t)-1 || cap[g * 2 + 1] < cap[g * 2])
    return NULL;
  return xstrndup(s + cap[g * 2], cap[g * 2 + 1] - cap[g * 2]);
}


static size_t group_num (const char *s, const size_t *cap, int g) {
  char *v = group_of(s, cap, g);
  size_t n = v ? (size_t)strtoul(v, NULL, 10) : 0;
  free(v);
  return n;
}


/* what line s says to pattern p, into m's reading so far */
static void pat_read (Matcher *m, const Pat *p, const char *s, const size_t *cap) {
  char *v = NULL;
  if (p->file && (v = group_of(s, cap, p->file)) != NULL) {
    free(m->file);
    m->file = v;
  }
  if (p->loc && (v = group_of(s, cap, p->loc)) != NULL) {	/* "12", "12,5", "12,5,12,9" */
    unsigned long a = 0, b = 0, c = 0, d = 0;
    int n = sscanf(v, "%lu,%lu,%lu,%lu", &a, &b, &c, &d);
    m->line = a;
    m->col = n >= 2 ? b : 0;
    m->eline = n >= 4 ? c : 0;
    m->ecol = n >= 4 ? d : 0;
    free(v);
  }
  if (p->line) m->line = group_num(s, cap, p->line);
  if (p->col) m->col = group_num(s, cap, p->col);
  if (p->eline) m->eline = group_num(s, cap, p->eline);
  if (p->ecol) m->ecol = group_num(s, cap, p->ecol);
  if (p->sev && (v = group_of(s, cap, p->sev)) != NULL) {
    m->lsev = sev_word(v, m->sev);
    free(v);
  }
  if (p->msg == 0 || (p->msg > 0 && (v = group_of(s, cap, p->msg)) != NULL)) {
    free(m->msg);
    m->msg = p->msg == 0 ? xstrndup(s + cap[0], cap[1] - cap[0]) : v;
  }
}


static void pat_reset (Matcher *m) {
  free(m->file);
  free(m->msg);
  m->file = m->msg = NULL;
  m->line = m->col = m->eline = m->ecol = 0;
  m->lsev = 0;
  m->at = 0;
}


static void pat_emit (int j, Matcher *m) {
  if (m->file && m->msg) add_problem(j, m, m->file, m->line, m->col, m->eline, m->ecol, m->lsev ? m->lsev : m->sev, m->msg);
}


/* a line for a matcher of its own: its pattern's lines, one after the other */
static void pat_line (int j, Matcher *m, const char *s) {
  size_t cap[20];
  int k;
  for (k = 0; k < 20; k++) cap[k] = (size_t)-1;
  if (m->at > 0 && rx_match(m->pat[m->at].re, s, cap)) {
    const Pat *p = &m->pat[m->at];
    pat_read(m, p, s, cap);
    if (m->at + 1 < m->npat) {
      m->at++;
      return;
    }
    pat_emit(j, m);
    if (p->loop) {	/* the next line may be one more */
      free(m->msg);
      m->msg = NULL;
    }
    else pat_reset(m);
    return;
  }
  pat_reset(m);
  for (k = 0; k < 20; k++) cap[k] = (size_t)-1;
  if (!rx_match(m->pat[0].re, s, cap)) return;
  pat_read(m, &m->pat[0], s, cap);
  if (m->npat == 1) {
    pat_emit(j, m);
    pat_reset(m);
  }
  else m->at = 1;
}


static void one_line (int j, const char *s) {
  int i;
  for (i = 0; i < g_job[j].nm; i++) {
    Matcher *m = &g_job[j].m[i];
    if (m->begins && rx_match(m->begins, s, NULL)) probs_drop(j);	/* a new cycle: its old problems go */
    if (m->ends && rx_match(m->ends, s, NULL)) g_job[j].ready = 1;
    if (m->npat) pat_line(j, m, s);
    else if (m->builtin != PM_NONE) {
      char *path;
      size_t ln, col;
      int sev;
      const char *msg;
      if (parse_problem(m->builtin, s, &path, &ln, &col, &sev, &msg)) {
        add_problem(j, m, path, ln, col, 0, 0, sev, msg);
        free(path);
      }
    }
  }
}


static int job_of_term (int id) {
  int j;
  for (j = 0; id > 0 && j < g_njob; j++)
    if (g_job[j].used && g_job[j].term == id) return j;
  return -1;
}


/* the output of a task's terminal: line by line, its escapes taken away */
void task_output (int id, const char *s, size_t n) {
  size_t i;
  int j = job_of_term(id);
  Job *x;
  if (j < 0) return;
  x = &g_job[j];
  if (x->nm == 0) return;
  for (i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\033') {	/* CSI ... letter, OSC ... BEL */
      if (i + 1 < n && s[i + 1] == '[') {
        for (i += 2; i < n && !((s[i] >= '@' && s[i] <= '~')); i++) ;
      }
      else if (i + 1 < n && s[i + 1] == ']') {
        for (i += 2; i < n && s[i] != '\a' && !(s[i] == '\033' && i + 1 < n && s[i + 1] == '\\'); i++) ;
      }
      else i++;
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n') {
      buf_putc(&x->line, '\0');
      one_line(j, x->line.s);
      x = &g_job[j];
      x->line.len = 0;
      continue;
    }
    buf_putc(&x->line, c);
  }
}


/* its terminal ended (or was killed: code -1): task_poll goes on from there */
void task_done (int id, int code) {
  int j = job_of_term(id);
  Job *x;
  if (j < 0) return;
  x = &g_job[j];
  if (x->line.len) {
    buf_putc(&x->line, '\0');
    one_line(j, x->line.s);
    g_job[j].line.len = 0;
  }
  g_job[j].exited = 1;
  g_job[j].exit_code = code;
}


/* its program in a terminal (the environment it says for it only); 0 it runs */
static int job_exec (int j) {
  Job *x = &g_job[j];
  char *old[64];
  size_t i, n = x->env.n / 2 < 64 ? x->env.n / 2 : 64;
  int r;
  if (x->custom) {	/* the extension runs it (its output: an Output channel of its own) */
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"id\":");
    json_put_str(&b, x->custom, strlen(x->custom));
    buf_puts(&b, ",\"label\":");
    json_put_str(&b, x->label, strlen(x->label));
    buf_puts(&b, "}");
    buf_putc(&b, '\0');
    lsp_ext_notify("mme/runTask", b.s);
    buf_free(&b);
    x->state = JS_OK;
    return 0;
  }
  if (x->argv == NULL || x->argv[0] == NULL) {	/* dependsOn only: done when they are */
    x->state = JS_OK;
    return 0;
  }
  for (i = 0; i < n; i++) {
    old[i] = os_getenv(x->env.v[i * 2]);
    os_setenv(x->env.v[i * 2], x->env.v[i * 2 + 1]);
  }
  x->term = g_next_id++;
  r = on_task_run(x->label, x->argv, x->echo, x->cwd, x->term, x->flags);
  x = &g_job[j];
  for (i = 0; i < n; i++) {
    os_setenv(x->env.v[i * 2], old[i]);
    free(old[i]);
  }
  if (r != 0) {
    x->term = 0;
    x->state = JS_FAIL;
    x->code = -1;
    return -1;
  }
  x->state = JS_RUN;
  if (x->bg) {	/* a background task without an endsPattern: ready at once */
    int k, ends = 0;
    for (k = 0; k < x->nm; k++) ends |= x->m[k].ends != NULL;
    if (!ends) x->state = JS_READY;
  }
  return 0;
}


static int dep_ok (int d) {
  return g_job[d].state == JS_OK || g_job[d].state == JS_READY;
}


/* one step of job j; 1 when it changed */
static int job_step (int j) {
  Job *x = &g_job[j];
  int i;
  if (x->exited && (x->state == JS_RUN || x->state == JS_READY)) {
    x->exited = 0;
    x->code = x->exit_code;
    x->state = x->code == 0 ? JS_OK : JS_FAIL;
    x->term = 0;
    if (x->state == JS_FAIL && x->silent) on_task_run(x->label, NULL, NULL, NULL, 0, 0);	/* reveal "silent" */
    return 1;
  }
  if (x->ready && x->state == JS_RUN) {
    x->ready = 0;
    if (x->bg) {
      x->state = JS_READY;
      return 1;
    }
  }
  if (x->state != JS_WAIT) return 0;
  for (i = 0; i < x->ndep; i++)
    if (g_job[x->dep[i]].state == JS_FAIL) {	/* one it needs failed: it does not run */
      x->state = JS_FAIL;
      x->code = g_job[x->dep[i]].code;
      return 1;
    }
  if (x->seq) {
    while (x->next < x->ndep && dep_ok(x->dep[x->next])) x->next++;
    if (x->next < x->ndep) {
      if (g_job[x->dep[x->next]].state == JS_NEW) {
        g_job[x->dep[x->next]].state = JS_WAIT;
        return 1;
      }
      return 0;
    }
  }
  else {
    int wait = 0, changed = 0;
    for (i = 0; i < x->ndep; i++) {
      if (g_job[x->dep[i]].state == JS_NEW) {
        g_job[x->dep[i]].state = JS_WAIT;
        changed = 1;
      }
      if (!dep_ok(x->dep[i])) wait = 1;
    }
    if (wait) return changed;
  }
  job_exec(j);
  return 1;
}


/* the jobs moved on, and those asked for that are done told; from the main loop (1: something changed) */
int task_poll (void) {
  int j, changed, any = 0, guard = 0;
  if (g_njob == 0) return 0;
  do {
    changed = 0;
    for (j = 0; j < g_njob; j++)
      if (g_job[j].used) changed |= job_step(j);
    any |= changed;
  } while (changed && ++guard < 1000);
  for (j = 0; j < g_njob; j++) {
    Job *x = &g_job[j];
    int st = x->state;
    if (!x->used || x->top != j || x->told || !(st == JS_OK || st == JS_FAIL || st == JS_READY)) continue;
    x->told = 1;
    any = 1;
    if (!x->quiet) {
      if (st == JS_FAIL && x->code != 0 && x->code != -1 && job_errors() == 0 && x->cb == NULL)
        toast(1, "The task exited with code %d", x->code);
      acc_signal(st != JS_FAIL ? SIG_TASK_DONE : SIG_TASK_FAILED);
    }
    if (x->cb) x->cb(x->ud, st != JS_FAIL, x->code, job_errors());
  }
  jobs_tidy();
  return any;
}


/* a run of task g_task[t] (and what it dependsOn): 0 it started, -1 not (said, or an input not answered) */
static int run_task (int t, TaskCb cb, void *ud) {
  int j, k;
  for (j = 0; j < g_njob; j++)	/* already running: VS Code's question */
    if (g_job[j].used && g_job[j].top == j && active(&g_job[j]) && strcmp(g_job[j].label, g_task[t].label) == 0 &&
        !(g_job[j].bg && g_job[j].state == JS_READY && cb)) {
      static const char *const bt[] = {"Terminate Task", "Restart Task", "Cancel"};
      char msg[300];
      int r;
      snprintf(msg, sizeof(msg), "The task '%s' is already active.", g_task[t].label);
      r = dialog(msg, NULL, bt, 3);
      if (r == 0 || r == 1) {
        if (g_job[j].term) panel_kill_task(g_job[j].term);
        task_poll();
      }
      if (r != 1) return -1;
      break;
    }
  vs_inputs(json_get(g_tj, "inputs"));
  g_nmade = 0;
  j = job_make(t, -1, 0);
  if (j < 0) {	/* not made after all: what was made goes */
    for (k = 0; k < g_nmade; k++) job_free(g_made[k]);
    g_nmade = 0;
    return -1;
  }
  probs_tidy();
  g_nmade = 0;
  g_job[j].cb = cb;
  g_job[j].ud = ud;
  g_job[j].state = JS_WAIT;
  task_poll();
  return 0;
}


/* a task by its label (a preLaunchTask, a postDebugTask); cb is told when it ends, or is ready */
int task_run_label (const char *label, TaskCb cb, void *ud) {
  int t, j;
  if (!trust_require("Running tasks")) return -2;
  for (j = 0; j < g_njob; j++)	/* a background task that runs and is ready: nothing to wait for */
    if (cb && g_job[j].used && g_job[j].bg && g_job[j].state == JS_READY && strcmp(g_job[j].label, label) == 0) {
      cb(ud, 1, 0, job_errors());
      return 0;
    }
  ask_ext_tasks();
  load_tasks();
  if ((t = task_index(label)) < 0) return -1;
  return run_task(t, cb, ud) == 0 ? 0 : -2;
}


/*
** A debug adapter's runInTerminal: its program (argv) in a terminal named
** title, in cwd, with env's variables (a null one taken away); 0 it runs.
*/
int task_run_in_terminal (const char *title, char **argv, const char *cwd, const Json *env) {
  int j = job_new(title), i, r;
  Job *x = &g_job[j];
  size_t k;
  for (i = 0; argv[i]; i++) ;
  x->argv = (char **)xmalloc((size_t)(i + 1) * sizeof(char *));
  for (i = 0; argv[i]; i++) x->argv[i] = xstrdup(argv[i]);
  x->argv[i] = NULL;
  x->cwd = xstrdup(cwd && *cwd ? cwd : side_root());
  for (k = 0; env && env->type == J_OBJ && k < env->n; k++) {
    vec_push(&x->env, xstrdup(env->kid[k]->key));
    vec_push(&x->env, env->kid[k]->type == J_STR ? xstrdup(env->kid[k]->str) : NULL);
  }
  x->flags = PR_DEDICATED | TT_FOCUS;
  x->quiet = 1;
  x->state = JS_WAIT;
  r = job_exec(j);
  if (r != 0) job_free(j);
  return r;
}


/* mme/runTask: an extension's tasks.executeTask: run as one of Run Task's */
void task_ext_run (const Json *t) {
  ExtTask e;
  char *cmd, *cwd;
  if (!trust_require("Running tasks")) return;
  ext_one(&e, t);
  cmd = vs_subst(e.cmd);
  cwd = vs_subst(e.cwd[0] ? e.cwd : side_root());
  load_tasks();
  task_add(e.label, cmd, cwd, e.source, 0, 0, 0, e.matcher);
  if (e.cmd[0]) run_task(g_ntask - 1, NULL, NULL);
  free(cmd);
  free(cwd);
  free(e.id);
  free(e.label);
  free(e.source);
  free(e.cmd);
  free(e.cwd);
}


/* Tasks: Run Task: the tasks there are, tasks.json's first */
static void run_task_pick (int build) {
  Pick p;
  int i, r, idx[512], n = 0;
  ask_ext_tasks();
  load_tasks();
  if (build) {	/* the default build task runs at once */
    for (i = 0; i < g_ntask; i++)
      if (g_task[i].build && g_task[i].dflt) {
        run_task(i, NULL, NULL);
        return;
      }
    for (i = 0; i < g_ntask && n < 512; i++)
      if (g_task[i].build) idx[n++] = i;
    if (n == 1 && g_task[idx[0]].source[0] == '\0') {
      run_task(idx[0], NULL, NULL);
      return;
    }
    if (n == 0) {
      toast(0, "No build task to run found. Configure Build Task...");
      build = 0;
    }
  }
  if (!build)
    for (i = 0; i < g_ntask && n < 512; i++)
      if (!g_task[i].hide) idx[n++] = i;
  if (n == 0) {
    toast(0, "No tasks: Terminal > Configure Tasks makes tasks.json");
    return;
  }
  pick_init(&p, build ? "Select the build task to run" : "Select the task to run");
  for (i = 0; i < n; i++) {
    const Task *t = &g_task[idx[i]];
    pick_add(&p, t->label, t->detail ? t->detail : t->source[0] ? t->source : "tasks.json", 0xEB6D);	/* codicon tools */
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) run_task(idx[r], NULL, NULL);
}


/* Tasks: Terminate Task: the one running, or the one picked */
static void terminate_task (void) {
  int j, idx[64], n = 0, r;
  Pick p;
  for (j = 0; j < g_njob && n < 64; j++)
    if (g_job[j].used && g_job[j].term && (g_job[j].state == JS_RUN || g_job[j].state == JS_READY)) idx[n++] = j;
  if (n == 0) {
    toast(0, "There are no task running. Run a task first.");
    return;
  }
  if (n == 1) r = 0;
  else {
    pick_init(&p, "Select a task to terminate");
    for (j = 0; j < n; j++) pick_add(&p, g_job[idx[j]].label, g_job[idx[j]].bg ? "background" : NULL, 0xEB6D);
    r = pick_run(&p);
    pick_free(&p);
  }
  if (r >= 0) panel_kill_task(g_job[idx[r]].term);
  task_poll();
}


/* Configure Tasks: .vscode/tasks.json, made with a build task */
static void configure_tasks (void) {
  char *dir = path_join(side_root(), ".vscode"), *f = path_join(dir, "tasks.json");
  OsStat st;
  if (os_stat(f, &st) != 0 || !st.exists) {
    Buf b;
    int fd;
    const char *cmd = exists("go.mod") ? "go build ./..." : exists("Cargo.toml") ? "cargo build" : "make";
    const char *pm = exists("go.mod") ? "$go" : "$gcc";
    buf_init(&b);
    buf_printf(&b, "{\n"
                   "  // See https://go.microsoft.com/fwlink/?LinkId=733558\n"
                   "  // for the documentation about the tasks.json format\n"
                   "  \"version\": \"2.0.0\",\n"
                   "  \"tasks\": [\n"
                   "    {\n"
                   "      \"label\": \"build\",\n"
                   "      \"type\": \"shell\",\n"
                   "      \"command\": \"%s\",\n"
                   "      \"group\": {\"kind\": \"build\", \"isDefault\": true},\n"
                   "      \"problemMatcher\": [\"%s\"]\n"
                   "    }\n"
                   "  ]\n"
                   "}\n", cmd, pm);
    mkdir_p(dir);
    if ((fd = os_open(f, OS_WRITE)) >= 0) {
      os_write(fd, b.s, b.len);
      os_close(fd);
    }
    buf_free(&b);
  }
  on_debug(DE_OPEN, f, 0);
  free(f);
  free(dir);
}


void task_command (int cmd) {
  if ((cmd == CMD_TASK_RUN || cmd == CMD_TASK_BUILD) && !trust_require("Running tasks")) return;	/* Restricted Mode */
  if (cmd == CMD_TASK_RUN) run_task_pick(0);
  else if (cmd == CMD_TASK_BUILD) run_task_pick(1);
  else if (cmd == CMD_TASK_CONFIGURE) configure_tasks();
  else if (cmd == CMD_TASK_TERMINATE) terminate_task();
}

/* }================================================================== */
