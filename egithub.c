/*
** egithub.c - GitHub pull requests and issues, as VS Code's GitHub Pull
** Requests extension has them: the repository's open pull requests and
** issues in a list, each one's description and comments, its changes,
** checking a pull request out, starting on an issue (its branch), a
** comment, a new pull request or issue.
**
** It talks to GitHub's REST API with curl. The token: GH_TOKEN or
** GITHUB_TOKEN, else the one "GitHub: Sign In" kept (mme-data/github-token),
** else Git's credential helper (asked quietly), else `gh auth token`. A
** public repository needs none to be read. github-enterprise.uri is a
** GitHub Enterprise server.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static char *g_token;	/* found once */
static int g_token_looked;
static char g_owner[128], g_repo[128];


/*
** {==================================================================
** Talking to GitHub
** ===================================================================
*/

static char *curl_path (void) {
#ifdef _WIN32
  char *root = os_getenv("SystemRoot");	/* Windows' own curl first (the Extensions view's, Chat's) */
  if (root) {
    char *p = path_join(root, "System32\\curl.exe");
    OsStat st;
    free(root);
    if (os_stat(p, &st) == 0 && st.exists) return p;
    free(p);
  }
#endif
  return find_program("curl");
}


/* the API: MME_GITHUB_API (tests), github-enterprise.uri's, else api.github.com */
static char *api_base (void) {
  char *env = os_getenv("MME_GITHUB_API");
  const char *ent = json_str(settings_get("github-enterprise\\.uri"), "");
  if (env && *env) return env;
  free(env);
  if (*ent) {
    char *b = (char *)xmalloc(strlen(ent) + 16);
    size_t n;
    strcpy(b, ent);
    n = strlen(b);
    while (n > 0 && b[n - 1] == '/') b[--n] = '\0';
    strcat(b, "/api/v3");
    return b;
  }
  return xstrdup("https://api.github.com");
}


static char *token_file (void) {
  return data_path("github-token");
}


static void trim (char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
}


/* Git's credential helper for github.com, asked so that it never opens a window or asks on a terminal */
static char *git_credential (void) {
  char *git = find_program("git"), *argv[4], *pw = NULL;
  int in[2], out[2], io[3], null;
  OsProc proc;
  long pid;
  Buf b;
  char chunk[4096];
  long n;
  static const char q[] = "protocol=https\nhost=github.com\n\n";
  if (git == NULL) return NULL;
  if (os_pipe(in) != 0) {
    free(git);
    return NULL;
  }
  if (os_pipe(out) != 0) {
    os_close(in[0]);
    os_close(in[1]);
    free(git);
    return NULL;
  }
#ifdef _WIN32
  null = os_open("NUL", OS_WRITE);
#else
  null = os_open("/dev/null", OS_WRITE);
#endif
  io[0] = in[0];
  io[1] = out[1];
  io[2] = null;
  argv[0] = git;
  argv[1] = (char *)"credential";
  argv[2] = (char *)"fill";
  argv[3] = NULL;
  os_setenv("GIT_TERMINAL_PROMPT", "0");
  os_setenv("GCM_INTERACTIVE", "never");
  if (os_spawn(git, argv, NULL, io, 3, &proc, &pid) != 0) {
    os_close(in[0]);
    os_close(in[1]);
    os_close(out[0]);
    os_close(out[1]);
    if (null >= 0) os_close(null);
    free(git);
    return NULL;
  }
  os_close(in[0]);
  os_close(out[1]);
  if (null >= 0) os_close(null);
  os_write(in[1], q, sizeof(q) - 1);
  os_close(in[1]);
  buf_init(&b);
  while ((n = os_read(out[0], chunk, sizeof(chunk))) > 0) buf_putn(&b, chunk, (size_t)n);
  os_close(out[0]);
  buf_putc(&b, '\0');
  if (os_wait(proc) == 0) {
    char *p = strstr(b.s, "password=");
    if (p) {
      char *e = strchr(p, '\n');
      pw = e ? xstrndup(p + 9, (size_t)(e - p - 9)) : xstrdup(p + 9);
      trim(pw);
    }
  }
  buf_free(&b);
  free(git);
  return pw;
}


static const char *token (void) {
  char *s;
  size_t len;
  if (g_token_looked) return g_token;
  g_token_looked = 1;
  if ((s = os_getenv("GH_TOKEN")) != NULL || (s = os_getenv("GITHUB_TOKEN")) != NULL) {
    trim(s);
    if (*s) return g_token = s;
    free(s);
  }
  {
    char *f = token_file();
    s = read_file(f, &len);
    free(f);
    if (s) {
      trim(s);
      if (*s) return g_token = s;
      free(s);
    }
  }
  {
    char *base = api_base();
    int public_host = strstr(base, "api.github.com") != NULL;
    free(base);
    if (public_host && (s = git_credential()) != NULL && *s) return g_token = s;
  }
  {
    char *gh = find_program("gh");
    if (gh) {
      char *argv[4];
      Buf b;
      argv[0] = gh;
      argv[1] = (char *)"auth";
      argv[2] = (char *)"token";
      argv[3] = NULL;
      buf_init(&b);
      if (run_capture(argv, &b) == 0 && b.s) {
        trim(b.s);
        if (*b.s) g_token = xstrdup(b.s);
      }
      buf_free(&b);
      free(gh);
    }
  }
  return g_token;
}


/* a notice drawn now, before a request that makes the user wait */
static void busy (const char *what) {
  toast(0, "GitHub: %s...", what);
  if (ui_background) ui_background();
  toast_draw();
  scr_flush();
}


/* a value of curl's config (-K): quoted, \ " and the line ends escaped */
static void cfg_put (Buf *b, const char *name, const char *s, size_t n) {
  size_t i;
  buf_printf(b, "%s = \"", name);
  for (i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\n') buf_puts(b, "\\n");
    else if (c == '\r') buf_puts(b, "\\r");
    else if (c == '\t') buf_puts(b, "\\t");
    else {
      if (c == '\\' || c == '"') buf_putc(b, '\\');
      buf_putc(b, c);
    }
  }
  buf_puts(b, "\"\n");
}


struct GhReq {
  OsProc proc;
  long pid;
  int out;	/* what curl prints: the answer, then its status */
  Buf got;
};


/*
** A request started: method, the path after the API's base, a JSON body
** (or NULL), what is accepted (NULL: JSON). curl's arguments name nothing
** but "-K -": the URL, the token and the body go in on its stdin (no length
** limit, nothing another program sees). NULL: curl is not there.
*/
static GhReq *start (const char *method, const char *path, const char *body, const char *accept) {
  char *curl = curl_path(), *base, *argv[4];
  const char *tok = token();
  int in[2], out[2], io[3];
  size_t done = 0;
  Buf c;
  GhReq *r;
  if (curl == NULL) {
    toast(1, "GitHub: curl was not found.");
    return NULL;
  }
  base = api_base();
  buf_init(&c);
  buf_printf(&c, "%s%s", base, path);
  {
    char *url = xstrdup(c.s);
    c.len = 0;
    c.s[0] = '\0';
    cfg_put(&c, "url", url, strlen(url));
    free(url);
  }
  buf_puts(&c, "silent\nlocation\nmax-time = 30\n");
  cfg_put(&c, "request", method, strlen(method));
  {
    char h[400];
    snprintf(h, sizeof(h), "Accept: %s", accept ? accept : "application/vnd.github+json");
    cfg_put(&c, "header", h, strlen(h));
    cfg_put(&c, "header", "User-Agent: mme", 15);
    cfg_put(&c, "header", "X-GitHub-Api-Version: 2022-11-28", 32);
    if (tok) {
      snprintf(h, sizeof(h), "Authorization: Bearer %s", tok);
      cfg_put(&c, "header", h, strlen(h));
      memset(h, 0, sizeof(h));
    }
  }
  if (body) {
    cfg_put(&c, "header", "Content-Type: application/json", 30);
    cfg_put(&c, "data-binary", body, strlen(body));
  }
  cfg_put(&c, "write-out", "\n%{http_code}", 13);
  argv[0] = curl;
  argv[1] = (char *)"-K";
  argv[2] = (char *)"-";
  argv[3] = NULL;
  r = (GhReq *)xmalloc(sizeof(*r));
  memset(r, 0, sizeof(*r));
  if (os_pipe(in) != 0) {
    free(r);
    r = NULL;
  }
  else if (os_pipe(out) != 0) {
    os_close(in[0]);
    os_close(in[1]);
    free(r);
    r = NULL;
  }
  else {
    io[0] = in[0];
    io[1] = out[1];
    io[2] = -1;
    if (os_spawn(curl, argv, NULL, io, 3, &r->proc, &r->pid) != 0) {
      os_close(in[0]);
      os_close(in[1]);
      os_close(out[0]);
      os_close(out[1]);
      free(r);
      r = NULL;
    }
    else {
      os_close(in[0]);
      os_close(out[1]);
      while (done < c.len) {	/* curl reads its config before anything else */
        long w = os_write(in[1], c.s + done, c.len - done);
        if (w <= 0) break;
        done += (size_t)w;
      }
      os_close(in[1]);
      r->out = out[0];
      buf_init(&r->got);
    }
  }
  if (c.s) memset(c.s, 0, c.cap);	/* it held the token */
  buf_free(&c);
  free(base);
  free(curl);
  if (r == NULL) toast(1, "GitHub: curl could not be started.");
  return r;
}


GhReq *gh_start (const char *method, const char *path, const char *body) {
  return start(method, path, body, NULL);
}


/*
** What the request has come to, while the editor waits for keys: -1 it is
** not done; else the HTTP status (0: GitHub could not be reached), the
** answer in *out (buf_free it), and r is freed.
*/
int gh_poll (GhReq *r, Buf *out) {
  char chunk[16384];
  int status = 0;
  size_t k;
  while (os_wait_readable(r->out, 0) == 1) {
    long n = os_read(r->out, chunk, sizeof(chunk));
    if (n <= 0) {
      os_close(r->out);
      r->out = -1;
      break;
    }
    buf_putn(&r->got, chunk, (size_t)n);
  }
  if (r->out >= 0) return -1;
  os_wait(r->proc);
  *out = r->got;
  if (out->len > 0) {	/* the status: the last line */
    char code[8];
    k = out->len;
    while (k > 0 && out->s[k - 1] != '\n') k--;
    snprintf(code, sizeof(code), "%.*s", (int)(out->len - k < 7 ? out->len - k : 7), out->s + k);
    status = atoi(code);
    out->len = k > 0 ? k - 1 : 0;
    out->s[out->len] = '\0';
  }
  free(r);
  return status;
}


/* a request, waited for (the notice of busy() on the screen) */
static int http (const char *method, const char *path, const char *body, const char *accept, Buf *out) {
  GhReq *r = start(method, path, body, accept);
  int st;
  buf_init(out);
  if (r == NULL) return 0;
  while ((st = gh_poll(r, out)) < 0) os_wait_readable(r->out, 200);
  return st;
}


int gh_request (const char *method, const char *path, const char *body, Buf *out) {
  return http(method, path, body, NULL, out);
}


void gh_busy (const char *what) {
  busy(what);
}


int gh_signed_in (void) {
  return token() != NULL;
}


void gh_fail (int status, const Buf *out) {	/* what went wrong, in words */
  Json *j = out->s ? json_parse(out->s, out->len) : NULL;
  const char *msg = json_str(json_get(j, "message"), "");
  if (status == 0) toast(1, "GitHub: it could not be reached (see your connection, or curl).");
  else if (status == 401) toast(1, "GitHub: the token was refused. GitHub: Sign In with a new one.");
  else if (status == 404) toast(1, "GitHub: not found (a private repository needs GitHub: Sign In). %s", msg);
  else if (status == 403) toast(1, "GitHub: %s", *msg ? msg : "forbidden (the rate limit? GitHub: Sign In)");
  else toast(1, "GitHub: %d %s", status, msg);
  json_free(j);
}


/* GET path as JSON; NULL (and a toast): it failed */
static Json *get_json (const char *path) {
  Buf out;
  int st = http("GET", path, NULL, NULL, &out);
  Json *j = NULL;
  if (st >= 200 && st < 300) j = json_parse(out.s ? out.s : "", out.len);
  else gh_fail(st, &out);
  buf_free(&out);
  return j;
}

/* }================================================================== */


/*
** {==================================================================
** The repository
** ===================================================================
*/

/* owner and name from the origin remote's URL (https://github.com/o/r.git, git@github.com:o/r.git); 0: none */
static int repo (void) {
  const char *args[] = {"remote", "get-url", "origin", NULL};
  Buf b;
  char *s, *p, *slash, *end;
  g_owner[0] = g_repo[0] = '\0';
  if (git_root() == NULL) {
    toast(1, "GitHub: the folder is not a Git repository.");
    return 0;
  }
  buf_init(&b);
  if (git_exec(&b, 0, args) != 0 || b.s == NULL) {
    buf_free(&b);
    toast(1, "GitHub: the repository has no remote called origin.");
    return 0;
  }
  s = b.s;
  trim(s);
  {	/* the last two parts: .../owner/repo(.git), git@host:owner/repo (a user and secret before the host do not count) */
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') s[--n] = '\0';
    if (n > 4 && strcmp(s + n - 4, ".git") == 0) s[n -= 4] = '\0';
    end = s + n;
    p = end;
    while (p > s && p[-1] != '/' && p[-1] != ':') p--;	/* the repository */
    slash = p > s ? p - 1 : NULL;
    if (slash) {
      char *o = slash;
      while (o > s && o[-1] != '/' && o[-1] != ':') o--;	/* the owner */
      if (o < slash) {
        snprintf(g_owner, sizeof(g_owner), "%.*s", (int)(slash - o), o);
        snprintf(g_repo, sizeof(g_repo), "%s", p);
      }
    }
  }
  buf_free(&b);
  if (g_owner[0] == '\0' || g_repo[0] == '\0') {
    toast(1, "GitHub: origin is not a GitHub repository.");
    return 0;
  }
  return 1;
}


static void json_body (Buf *b, const char *k1, const char *v1, const char *k2, const char *v2, const char *k3,
                       const char *v3, const char *k4, const char *v4) {	/* {"k1": "v1", ...} */
  const char *k[4], *v[4];
  int i, first = 1;
  k[0] = k1; v[0] = v1; k[1] = k2; v[1] = v2; k[2] = k3; v[2] = v3; k[3] = k4; v[3] = v4;
  buf_putc(b, '{');
  for (i = 0; i < 4; i++) {
    if (k[i] == NULL || v[i] == NULL) continue;
    if (!first) buf_puts(b, ", ");
    first = 0;
    json_put_str(b, k[i], strlen(k[i]));
    buf_puts(b, ": ");
    json_put_str(b, v[i], strlen(v[i]));
  }
  buf_putc(b, '}');
}


static const char *login_of (const Json *j) {
  return json_str(json_get(j, "user.login"), "someone");
}

/* }================================================================== */


/*
** {==================================================================
** Descriptions, changes, comments
** ===================================================================
*/

static void add_comments (Buf *md, int num) {
  char path[300];
  Json *cs;
  size_t i;
  snprintf(path, sizeof(path), "/repos/%s/%s/issues/%d/comments?per_page=100", g_owner, g_repo, num);
  cs = get_json(path);
  if (cs == NULL || cs->type != J_ARR) {
    json_free(cs);
    return;
  }
  buf_printf(md, "\n---\n\n## Comments (%lu)\n", (unsigned long)cs->n);
  for (i = 0; i < cs->n; i++) {
    const Json *c = cs->kid[i];
    buf_printf(md, "\n**%s** \xC2\xB7 %.10s\n\n%s\n", login_of(c), json_str(json_get(c, "created_at"), ""),
               json_str(json_get(c, "body"), ""));
  }
  json_free(cs);
}


/* a pull request's (issue's) description and comments: a Markdown preview */
static void open_description (int num, int pr) {
  char path[300], name[80];
  Json *j;
  Buf md;
  busy(pr ? "loading the pull request" : "loading the issue");
  snprintf(path, sizeof(path), "/repos/%s/%s/%s/%d", g_owner, g_repo, pr ? "pulls" : "issues", num);
  if ((j = get_json(path)) == NULL) return;
  buf_init(&md);
  buf_printf(&md, "# %s (#%d)\n\n", json_str(json_get(j, "title"), ""), num);
  if (pr) {
    const char *state = json_bool(json_get(j, "merged"), 0) ? "Merged" : json_bool(json_get(j, "draft"), 0) ? "Draft" :
                        strcmp(json_str(json_get(j, "state"), ""), "open") == 0 ? "Open" : "Closed";
    buf_printf(&md, "**%s** \xC2\xB7 %s wants to merge `%s` into `%s` \xC2\xB7 %d commit%s \xC2\xB7 +%d \xE2\x88\x92%d \xC2\xB7 %d file%s\n",
               state, login_of(j), json_str(json_get(j, "head.ref"), ""), json_str(json_get(j, "base.ref"), ""),
               (int)json_num(json_get(j, "commits"), 0), json_num(json_get(j, "commits"), 0) == 1 ? "" : "s",
               (int)json_num(json_get(j, "additions"), 0), (int)json_num(json_get(j, "deletions"), 0),
               (int)json_num(json_get(j, "changed_files"), 0), json_num(json_get(j, "changed_files"), 0) == 1 ? "" : "s");
  }
  else {
    const Json *labels = json_get(j, "labels");
    size_t i;
    buf_printf(&md, "**%s** \xC2\xB7 opened by %s", strcmp(json_str(json_get(j, "state"), ""), "open") == 0 ? "Open" : "Closed",
               login_of(j));
    for (i = 0; labels && labels->type == J_ARR && i < labels->n; i++)
      buf_printf(&md, " \xC2\xB7 `%s`", json_str(json_get(labels->kid[i], "name"), ""));
    buf_putc(&md, '\n');
  }
  buf_printf(&md, "\n%s\n", json_str(json_get(j, "html_url"), ""));
  {
    const char *body = json_str(json_get(j, "body"), "");
    buf_printf(&md, "\n---\n\n%s\n", *body ? body : "*No description provided.*");
  }
  add_comments(&md, num);
  snprintf(name, sizeof(name), "%s #%d.md", pr ? "PR" : "Issue", num);
  buf_putc(&md, '\0');
  md_page("github", name, md.s);
  toast(0, "%s", "");
  buf_free(&md);
  json_free(j);
}


/* a pull request's changes: its diff, in an editor (Diff's colors) */
static void open_changes (int num) {
  char path[300], name[80], *dir, *f;
  Buf out;
  int st, fd;
  busy("loading the changes");
  snprintf(path, sizeof(path), "/repos/%s/%s/pulls/%d", g_owner, g_repo, num);
  st = http("GET", path, NULL, "application/vnd.github.v3.diff", &out);
  if (st < 200 || st >= 300) {
    gh_fail(st, &out);
    buf_free(&out);
    return;
  }
  dir = data_path("github");
  os_mkdir(dir);
  snprintf(name, sizeof(name), "PR #%d.diff", num);
  f = path_join(dir, name);
  free(dir);
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    if (out.len) os_write(fd, out.s, out.len);
    os_close(fd);
  }
  buf_free(&out);
  toast(0, "%s", "");
  open_path(f);
  free(f);
}


static void add_comment (int num) {
  char *text = ask_text("Comment", NULL), path[300];
  Buf body, out;
  int st;
  if (text == NULL || *text == '\0') {
    free(text);
    return;
  }
  buf_init(&body);
  json_body(&body, "body", text, NULL, NULL, NULL, NULL, NULL, NULL);
  buf_putc(&body, '\0');
  free(text);
  busy("adding the comment");
  snprintf(path, sizeof(path), "/repos/%s/%s/issues/%d/comments", g_owner, g_repo, num);
  st = http("POST", path, body.s, NULL, &out);
  if (st == 201) toast(0, "GitHub: the comment was added to #%d.", num);
  else gh_fail(st, &out);
  buf_free(&out);
  buf_free(&body);
}


static int git_do (const char *a, const char *b, const char *c, const char *d) {	/* git a b c d, its output in the Git channel */
  const char *args[5];
  Buf out;
  int r;
  args[0] = a;
  args[1] = b;
  args[2] = c;
  args[3] = d;
  args[4] = NULL;
  buf_init(&out);
  r = git_exec(&out, 1, args);
  if (r != 0 && out.s) {
    trim(out.s);
    toast(1, "Git: %s", out.s);
  }
  buf_free(&out);
  return r;
}


/* Checkout: the pull request's head into a branch of its own (its name when it is this repository's, else pr/N) */
static void checkout (const Json *pr) {
  int num = (int)json_num(json_get(pr, "number"), 0);
  const char *head_repo = json_str(json_get(pr, "head.repo.full_name"), ""), *base_repo = json_str(json_get(pr, "base.repo.full_name"), "");
  char branch[160], ref[200];
  const char *args[] = {"rev-parse", "--verify", "--quiet", NULL, NULL};
  Buf out;
  if (strcmp(head_repo, base_repo) == 0 && *head_repo) snprintf(branch, sizeof(branch), "%s", json_str(json_get(pr, "head.ref"), ""));
  else snprintf(branch, sizeof(branch), "pr/%d", num);
  if (branch[0] == '\0') snprintf(branch, sizeof(branch), "pr/%d", num);
  busy("checking the pull request out");
  args[3] = branch;
  buf_init(&out);
  if (git_exec(&out, 0, args) != 0) {	/* not a branch here yet: fetched into one */
    snprintf(ref, sizeof(ref), "pull/%d/head:%s", num, branch);
    if (git_do("fetch", "origin", ref, NULL) != 0) {
      buf_free(&out);
      return;
    }
  }
  buf_free(&out);
  if (git_do("checkout", branch, NULL, NULL) == 0) toast(0, "Switched to '%s' (#%d).", branch, num);
  git_refresh();
}


/* Start Working on Issue: a branch for it, issue-N-its-title */
static void start_issue (int num, const char *title) {
  char branch[96];
  size_t k = 0, i;
  int dash = 0;
  k = (size_t)snprintf(branch, sizeof(branch), "issue-%d-", num);
  for (i = 0; title[i] && k + 1 < sizeof(branch) - 1 && k < 60; i++) {
    char c = title[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      branch[k++] = c;
      dash = 0;
    }
    else if (c >= 'A' && c <= 'Z') {
      branch[k++] = (char)(c + 32);
      dash = 0;
    }
    else if (!dash) {
      branch[k++] = '-';
      dash = 1;
    }
  }
  while (k > 0 && branch[k - 1] == '-') k--;
  branch[k] = '\0';
  if (git_do("checkout", "-b", branch, NULL) == 0) toast(0, "Switched to a new branch '%s' for #%d.", branch, num);
  git_refresh();
}

/* }================================================================== */


/*
** {==================================================================
** The lists, the commands
** ===================================================================
*/

static void pr_actions (const Json *pr) {
  static const char *const acts[] = {"Checkout", "Open Description", "Open Changes", "Add Comment...", "Open on GitHub"};
  static const int icons[] = {0xEA68, 0xEB8A, 0xEAE1, 0xEA6B, 0xEB14};
  int num = (int)json_num(json_get(pr, "number"), 0), r;
  size_t i;
  char title[300];
  Pick p;
  snprintf(title, sizeof(title), "#%d %s", num, json_str(json_get(pr, "title"), ""));
  pick_init(&p, title);
  p.keep_order = 1;
  for (i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) pick_add(&p, acts[i], NULL, icons[i]);
  r = pick_run(&p);
  pick_free(&p);
  switch (r) {
    case 0: checkout(pr); break;
    case 1: open_description(num, 1); break;
    case 2: open_changes(num); break;
    case 3: add_comment(num); break;
    case 4: browse(json_str(json_get(pr, "html_url"), "")); break;
  }
}


static void issue_actions (const Json *is) {
  static const char *const acts[] = {"Open Description", "Start Working on Issue", "Add Comment...", "Open on GitHub"};
  static const int icons[] = {0xEB8A, 0xEA68, 0xEA6B, 0xEB14};
  int num = (int)json_num(json_get(is, "number"), 0), r;
  size_t i;
  char title[300];
  Pick p;
  snprintf(title, sizeof(title), "#%d %s", num, json_str(json_get(is, "title"), ""));
  pick_init(&p, title);
  p.keep_order = 1;
  for (i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) pick_add(&p, acts[i], NULL, icons[i]);
  r = pick_run(&p);
  pick_free(&p);
  switch (r) {
    case 0: open_description(num, 0); break;
    case 1: start_issue(num, json_str(json_get(is, "title"), "")); break;
    case 2: add_comment(num); break;
    case 3: browse(json_str(json_get(is, "html_url"), "")); break;
  }
}


static void list (int pr) {
  char path[300], label[400], detail[400];
  Json *j;
  const Json **items;
  size_t i, n = 0;
  int r;
  Pick p;
  if (!repo()) return;
  busy(pr ? "loading the pull requests" : "loading the issues");
  snprintf(path, sizeof(path), "/repos/%s/%s/%s?state=open&per_page=100", g_owner, g_repo, pr ? "pulls" : "issues");
  if ((j = get_json(path)) == NULL) return;
  toast(0, "%s", "");
  if (j->type != J_ARR) {
    json_free(j);
    return;
  }
  items = (const Json **)xmalloc((j->n + 1) * sizeof(Json *));
  pick_init(&p, pr ? "Open pull requests" : "Open issues");
  for (i = 0; i < j->n; i++) {
    const Json *it = j->kid[i];
    if (!pr && json_get(it, "pull_request")) continue;	/* the issues' list has the pull requests too */
    snprintf(label, sizeof(label), "#%d %s", (int)json_num(json_get(it, "number"), 0), json_str(json_get(it, "title"), ""));
    if (pr)
      snprintf(detail, sizeof(detail), "%s%s \xC2\xB7 %s \xE2\x86\x92 %s", json_bool(json_get(it, "draft"), 0) ? "Draft \xC2\xB7 " : "",
               login_of(it), json_str(json_get(it, "head.ref"), ""), json_str(json_get(it, "base.ref"), ""));
    else {
      int nc = (int)json_num(json_get(it, "comments"), 0);
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d comment%s", login_of(it), nc, nc == 1 ? "" : "s");
    }
    pick_add(&p, label, detail, pr ? 0xEA64 : 0xEB0C);	/* git-pull-request, issues */
    items[n++] = it;
  }
  if (n == 0) {
    toast(0, "GitHub: %s/%s has no open %s.", g_owner, g_repo, pr ? "pull requests" : "issues");
    pick_free(&p);
    free(items);
    json_free(j);
    return;
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0 && (size_t)r < n) {
    if (pr) pr_actions(items[r]);
    else issue_actions(items[r]);
  }
  free(items);
  json_free(j);
}


/* Create Pull Request: this branch (pushed first) into the one asked (the repository's default) */
static void create_pr (void) {
  const char *branch = git_branch(), *log_args[] = {"log", "-1", "--pretty=%s", NULL};
  char path[300], *base, *title, *body;
  Json *info;
  Buf last, req, out;
  int st;
  if (!repo()) return;
  if (branch == NULL || *branch == '\0') {
    toast(1, "GitHub: no branch is checked out.");
    return;
  }
  if (token() == NULL) {
    toast(1, "GitHub: Sign In first (a token with the repo scope).");
    return;
  }
  busy("reading the repository");
  snprintf(path, sizeof(path), "/repos/%s/%s", g_owner, g_repo);
  if ((info = get_json(path)) == NULL) return;
  toast(0, "%s", "");
  base = ask_text("Merge into (the base branch)", json_str(json_get(info, "default_branch"), "main"));
  json_free(info);
  if (base == NULL || *base == '\0') {
    free(base);
    return;
  }
  buf_init(&last);
  git_exec(&last, 0, log_args);
  if (last.s) trim(last.s);
  title = ask_text("Title of the pull request", last.s ? last.s : branch);
  buf_free(&last);
  if (title == NULL || *title == '\0') {
    free(base);
    free(title);
    return;
  }
  body = ask_text("Description (optional)", NULL);
  busy("pushing the branch");
  if (git_do("push", "-u", "origin", branch) != 0) {
    free(base);
    free(title);
    free(body);
    return;
  }
  busy("creating the pull request");
  buf_init(&req);
  json_body(&req, "title", title, "head", branch, "base", base, "body", body ? body : "");
  buf_putc(&req, '\0');
  snprintf(path, sizeof(path), "/repos/%s/%s/pulls", g_owner, g_repo);
  st = http("POST", path, req.s, NULL, &out);
  if (st == 201) {
    Json *j = json_parse(out.s ? out.s : "", out.len);
    toast(0, "GitHub: pull request #%d was created: %s", (int)json_num(json_get(j, "number"), 0), json_str(json_get(j, "html_url"), ""));
    json_free(j);
  }
  else gh_fail(st, &out);
  buf_free(&out);
  buf_free(&req);
  free(base);
  free(title);
  free(body);
}


static void create_issue (void) {
  char path[300], *title, *body;
  Buf req, out;
  int st;
  if (!repo()) return;
  if (token() == NULL) {
    toast(1, "GitHub: Sign In first (a token with the repo scope).");
    return;
  }
  title = ask_text("Title of the issue", NULL);
  if (title == NULL || *title == '\0') {
    free(title);
    return;
  }
  body = ask_text("Description (optional)", NULL);
  busy("creating the issue");
  buf_init(&req);
  json_body(&req, "title", title, "body", body ? body : "", NULL, NULL, NULL, NULL);
  buf_putc(&req, '\0');
  snprintf(path, sizeof(path), "/repos/%s/%s/issues", g_owner, g_repo);
  st = http("POST", path, req.s, NULL, &out);
  if (st == 201) {
    Json *j = json_parse(out.s ? out.s : "", out.len);
    toast(0, "GitHub: issue #%d was created: %s", (int)json_num(json_get(j, "number"), 0), json_str(json_get(j, "html_url"), ""));
    json_free(j);
  }
  else gh_fail(st, &out);
  buf_free(&out);
  buf_free(&req);
  free(title);
  free(body);
}


/* Sign In: a personal access token, kept in mme-data/github-token (checked on the API first) */
static void sign_in (void) {
  char *tok = ask_text("GitHub: a personal access token (classic, the repo scope; or fine-grained)", NULL), *f;
  Json *me;
  int fd;
  if (tok == NULL || *tok == '\0') {
    free(tok);
    return;
  }
  trim(tok);
  free(g_token);
  g_token = tok;
  g_token_looked = 1;
  busy("checking the token");
  me = get_json("/user");
  if (me == NULL) {
    g_token = NULL;
    free(tok);
    return;
  }
  f = token_file();
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, tok, strlen(tok));
    os_close(fd);
  }
  free(f);
  toast(0, "GitHub: signed in as %s.", json_str(json_get(me, "login"), "?"));
  json_free(me);
}


static void sign_out (void) {
  char *f = token_file();
  os_unlink(f);
  free(f);
  free(g_token);
  g_token = NULL;
  g_token_looked = 0;
  toast(0, "GitHub: signed out (the token mme kept is gone).");
}


void gh_command (int cmd) {
  switch (cmd) {
    case CMD_GH_PRS: list(1); break;
    case CMD_GH_ISSUES: list(0); break;
    case CMD_GH_CREATE_PR: create_pr(); break;
    case CMD_GH_CREATE_ISSUE: create_issue(); break;
    case CMD_GH_SIGNIN: sign_in(); break;
    case CMD_GH_SIGNOUT: sign_out(); break;
  }
}

/* }================================================================== */
