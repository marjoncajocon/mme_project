/*
** etask.c - Tasks: .vscode/tasks.json, like VS Code's
**
** A task is a command line run in a terminal of its own in the panel,
** named after it. tasks.json's tasks come first; without one, tasks are
** found as VS Code's extensions find them: a Makefile's targets, a Go
** module's build and test, package.json's scripts, Cargo's build and
** test. A task's problem matcher ($gcc, $go, $tsc, $msCompile) reads its
** output: file:line:col: error: message lines go to the Problems panel.
** ${workspaceFolder}, ${file} ... are VS Code's variables (vs_subst), for
** launch.json too.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Variables
** ===================================================================
*/

static char *rel_to_root (const char *path) {
  const char *root = side_root();
  size_t n = strlen(root);
  if (m_fnncmp(path, root, n) == 0 && path_is_sep(path[n])) return xstrdup(path + n + 1);
  return xstrdup(path);
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
    if (strcmp(name, "workspaceFolder") == 0 || strcmp(name, "workspaceRoot") == 0 || strcmp(name, "cwd") == 0)
      buf_puts(&b, root);
    else if (strcmp(name, "workspaceFolderBasename") == 0) buf_puts(&b, path_basename(root));
    else if (strcmp(name, "pathSeparator") == 0 || strcmp(name, "/") == 0) buf_putc(&b, MMC_SEP);
    else if (strncmp(name, "env:", 4) == 0) {
      char *v = os_getenv(name + 4);
      if (v) buf_puts(&b, v);
      free(v);
    }
    else if (file == NULL) continue;	/* no file in front: the file's variables are empty */
    else if (strcmp(name, "file") == 0) buf_puts(&b, file);
    else if (strcmp(name, "fileBasename") == 0) buf_puts(&b, path_basename(file));
    else if (strcmp(name, "fileBasenameNoExtension") == 0 || strcmp(name, "fileExtname") == 0) {
      const char *base = path_basename(file), *dot = strrchr(base, '.');
      if (name[12] == 'N') buf_putn(&b, base, dot && dot != base ? (size_t)(dot - base) : strlen(base));
      else if (dot && dot != base) buf_puts(&b, dot);
    }
    else if (strcmp(name, "fileDirname") == 0 || strcmp(name, "relativeFileDirname") == 0) {
      char *dir = path_dirname(file);
      if (name[0] == 'r') {
        char *r = rel_to_root(dir);
        buf_puts(&b, strcmp(r, dir) == 0 ? "." : r);
        free(r);
      }
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
  char *label, *cmd, *cwd, *source;	/* source: "" for tasks.json's, else "make", "go" ... */
  int build, dflt, test;	/* its group */
  int matcher;
} Task;

static Task *g_task;
static int g_ntask;


static void task_add (const char *label, const char *cmd, const char *cwd, const char *source,
                      int build, int dflt, int test, int matcher) {
  Task *t;
  g_task = (Task *)xrealloc(g_task, (size_t)(g_ntask + 1) * sizeof(Task));
  t = &g_task[g_ntask++];
  t->label = xstrdup(label);
  t->cmd = xstrdup(cmd);
  t->cwd = xstrdup(cwd ? cwd : side_root());
  t->source = xstrdup(source);
  t->build = build;
  t->dflt = dflt;
  t->test = test;
  t->matcher = matcher;
}


static void tasks_free (void) {
  int i;
  for (i = 0; i < g_ntask; i++) {
    free(g_task[i].label);
    free(g_task[i].cmd);
    free(g_task[i].cwd);
    free(g_task[i].source);
  }
  free(g_task);
  g_task = NULL;
  g_ntask = 0;
}


static int matcher_of (const Json *pm) {
  const char *s;
  size_t i;
  if (pm == NULL) return PM_NONE;
  if (pm->type == J_ARR) {
    for (i = 0; i < pm->n; i++)
      if (matcher_of(pm->kid[i]) != PM_NONE) return matcher_of(pm->kid[i]);
    return PM_NONE;
  }
  if (pm->type == J_OBJ) return matcher_of(json_get(pm, "base"));
  s = json_str(pm, "");
  if (strcmp(s, "$gcc") == 0) return PM_GCC;
  if (strcmp(s, "$go") == 0) return PM_GO;
  if (strncmp(s, "$tsc", 4) == 0) return PM_TSC;
  if (strcmp(s, "$msCompile") == 0) return PM_MS;
  return PM_NONE;
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


static void load_tasks_json (void) {
  char *dir = path_join(side_root(), ".vscode"), *f = path_join(dir, "tasks.json"), *s;
  size_t len, i, k;
  Json *j;
  const Json *list;
  free(dir);
  s = read_file(f, &len);
  free(f);
  if (s == NULL) return;
  j = json_parse(s, len);
  free(s);
  if (j == NULL) {
    toast(1, "tasks.json is not valid JSON");
    return;
  }
  list = json_get(j, "tasks");
  for (i = 0; list && list->type == J_ARR && i < list->n; i++) {
    const Json *t = list->kid[i], *g = json_get(t, "group"), *args = json_get(t, "args");
    const char *type = json_str(json_get(t, "type"), "shell"), *label = json_str(json_get(t, "label"), NULL);
    const char *cmd = json_str(json_get(t, "command"), NULL), *kind;
    char *c, *cwd = NULL;
    Buf b;
    int dflt = 0;
    if (strcmp(type, "npm") == 0 && json_get(t, "script")) {
      buf_init(&b);
      buf_puts(&b, "npm run ");
      buf_puts(&b, json_str(json_get(t, "script"), ""));
    }
    else {
      if (cmd == NULL) continue;
      buf_init(&b);
      buf_puts(&b, cmd);
      for (k = 0; args && args->type == J_ARR && k < args->n; k++) {
        buf_putc(&b, ' ');
        put_arg(&b, json_str(args->kid[k], ""));
      }
    }
    buf_putc(&b, '\0');
    c = vs_subst(b.s);
    buf_free(&b);
    if (json_get(t, "options.cwd")) cwd = vs_subst(json_str(json_get(t, "options.cwd"), ""));
    kind = g && g->type == J_OBJ ? json_str(json_get(g, "kind"), "") : json_str(g, "");
    if (g && g->type == J_OBJ) dflt = json_bool(json_get(g, "isDefault"), 0);
    task_add(label ? label : c, c, cwd, "", strcmp(kind, "build") == 0, dflt, strcmp(kind, "test") == 0,
             matcher_of(json_get(t, "problemMatcher")));
    free(c);
    free(cwd);
  }
  json_free(j);
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
}

/* }================================================================== */


/*
** {==================================================================
** Running one, reading its problems
** ===================================================================
*/

typedef struct Prob {
  char *path;
  Diag d;
} Prob;

static struct {
  int id;	/* the terminal's task id, 0 none */
  int matcher;
  char *cwd;
  Buf line;
  Prob *v;
  size_t n;
} R;

static int g_next_id = 1;


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
static int parse_problem (const char *s, char **path, size_t *ln, size_t *col, int *sev, const char **msg) {
  const char *p, *q, *start = s;
  while (*start == ' ' || *start == '\t') start++;
  if (R.matcher == PM_MS)	/* "  3>file(...)" */
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
    else if (R.matcher == PM_GCC || R.matcher == PM_TSC || R.matcher == PM_MS) {	/* those say what it is */
      free(*path);
      return 0;
    }
    while (*q == ' ') q++;
    if ((R.matcher == PM_TSC || R.matcher == PM_MS) && *q != ':') {	/* the code: TS1005 */
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


static void report_file (const char *path) {
  size_t i, n = 0;
  Diag *v = (Diag *)xmalloc((R.n + 1) * sizeof(Diag));
  for (i = 0; i < R.n; i++)
    if (m_fncmp(R.v[i].path, path) == 0) v[n++] = R.v[i].d;
  lsp_task_diags(path, v, n);
  free(v);
}


static void one_line (const char *s) {
  char *path;
  size_t ln, col;
  int sev;
  const char *msg;
  if (R.matcher == PM_NONE || !parse_problem(s, &path, &ln, &col, &sev, &msg)) return;
  {	/* relative to the task's folder; only files that are there */
    OsStat st;
    char *full = (path_is_sep(path[0]) || (path[0] && path[1] == ':')) ? xstrdup(path) : path_join(R.cwd, path);
    char *real = os_realpath(full);
    free(path);
    free(full);
    if (real == NULL || os_stat(real, &st) != 0 || !st.exists || st.is_dir) {
      free(real);
      return;
    }
    {	/* the same one again (the terminal may paint a line twice) */
      size_t i;
      for (i = 0; i < R.n; i++)
        if (R.v[i].d.a.y + 1 == (ln ? ln : 1) && R.v[i].d.a.x + 1 == (col ? col : 1) &&
            strcmp(R.v[i].d.msg, msg) == 0 && m_fncmp(R.v[i].path, real) == 0) {
          free(real);
          return;
        }
    }
    R.v = (Prob *)xrealloc(R.v, (R.n + 1) * sizeof(Prob));
    R.v[R.n].path = real;
    R.v[R.n].d.a.y = R.v[R.n].d.b.y = ln > 0 ? ln - 1 : 0;
    R.v[R.n].d.a.x = col > 0 ? col - 1 : 0;
    R.v[R.n].d.b.x = R.v[R.n].d.a.x + 1;
    R.v[R.n].d.sev = sev;
    R.v[R.n].d.msg = xstrdup(msg);
    R.n++;
    report_file(real);
  }
}


/* the output of a task's terminal: line by line, its escapes taken away */
void task_output (int id, const char *s, size_t n) {
  size_t i;
  if (id != R.id) return;
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
      buf_putc(&R.line, '\0');
      one_line(R.line.s);
      R.line.len = 0;
      continue;
    }
    buf_putc(&R.line, c);
  }
}


void task_done (int id, int code) {
  if (id != R.id) return;
  if (R.line.len) {
    buf_putc(&R.line, '\0');
    one_line(R.line.s);
    R.line.len = 0;
  }
  if (code != 0 && R.n == 0) toast(1, "The task exited with code %d", code);
  R.id = 0;
}


static void run (const Task *t) {
  size_t i;
  for (i = 0; i < R.n; i++) {
    free(R.v[i].path);
    free(R.v[i].d.msg);
  }
  free(R.v);
  R.v = NULL;
  R.n = 0;
  lsp_task_clear();
  free(R.cwd);
  R.cwd = xstrdup(t->cwd);
  R.matcher = t->matcher;
  if (R.line.s == NULL) buf_init(&R.line);
  R.line.len = 0;
  R.id = g_next_id++;
  if (on_task_terminal(t->label, t->cmd, t->cwd, R.id) != 0) R.id = 0;
}


/* Tasks: Run Task: the tasks there are, tasks.json's first */
static void run_task_pick (int build) {
  Pick p;
  int i, r, idx[512], n = 0;
  load_tasks();
  if (build) {	/* the default build task runs at once */
    for (i = 0; i < g_ntask; i++)
      if (g_task[i].build && g_task[i].dflt) {
        run(&g_task[i]);
        return;
      }
    for (i = 0; i < g_ntask; i++)
      if (g_task[i].build) idx[n++] = i;
    if (n == 1 && g_task[idx[0]].source[0] == '\0') {
      run(&g_task[idx[0]]);
      return;
    }
    if (n == 0) {
      toast(0, "No build task to run found. Configure Build Task...");
      build = 0;
    }
  }
  if (!build)
    for (i = 0; i < g_ntask && n < 512; i++) idx[n++] = i;
  if (n == 0) {
    toast(0, "No tasks: Terminal > Configure Tasks makes tasks.json");
    return;
  }
  pick_init(&p, build ? "Select the build task to run" : "Select the task to run");
  for (i = 0; i < n; i++) {
    const Task *t = &g_task[idx[i]];
    pick_add(&p, t->label, t->source[0] ? t->source : "tasks.json", 0xEB6D);	/* codicon tools */
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) run(&g_task[idx[r]]);
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
  if (cmd == CMD_TASK_RUN) run_task_pick(0);
  else if (cmd == CMD_TASK_BUILD) run_task_pick(1);
  else if (cmd == CMD_TASK_CONFIGURE) configure_tasks();
}

/* }================================================================== */
