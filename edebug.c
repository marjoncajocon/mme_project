/*
** edebug.c - Run and Debug: a client of the Debug Adapter Protocol
**
** A debug adapter (dlv dap, python -m debugpy.adapter, lldb-dap, gdb -i
** dap ...) runs as a program; messages are "Content-Length: n" and n bytes
** of JSON, like a language server's, over its stdin and stdout, or over a
** TCP connection when it listens on a port (dlv says which on its stdout).
** .vscode/launch.json says what to debug, in VS Code's words. The Run and
** Debug view shows the variables, the watches, the call stack and the
** breakpoints; the panel's DEBUG CONSOLE the program's output and a REPL.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Sock;
#define NO_SOCK		INVALID_SOCKET
#define sock_close	closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int Sock;
#define NO_SOCK		(-1)
#define sock_close	close
#endif


/*
** {==================================================================
** The state
** ===================================================================
*/

enum { RQ_INIT, RQ_LAUNCH, RQ_SETBP, RQ_CONFDONE, RQ_THREADS, RQ_STACK, RQ_SCOPES,
       RQ_VARS, RQ_WATCH, RQ_REPL, RQ_HOVER, RQ_STEP, RQ_DISCONNECT, RQ_SETVAR, RQ_GOTOT, RQ_GOTO,
       RQ_EXCINFO, RQ_OTHER };

typedef struct Req {
  int seq, kind;
  long arg;	/* RQ_VARS: the reference; RQ_WATCH: the watch; RQ_STACK: the thread */
  char *path;	/* RQ_SETBP: the file */
  int list;	/* RQ_VARS: VARIABLES (0) or WATCH (1) */
} Req;

typedef struct Bp {	/* a breakpoint: a line of a file */
  char *path;
  size_t line;	/* from 0 */
  int enabled, verified, id;
  char *cond, *hit, *log;	/* its condition, hit count, log message (a logpoint); NULL none */
  int temp;	/* Run to Cursor's: it goes when the program stops */
} Bp;

typedef struct ExcFilter {	/* the adapter's exception breakpoints: "Uncaught Exceptions" ... */
  char *filter, *label;
  int enabled;
} ExcFilter;

typedef struct Var {	/* a row of VARIABLES: a scope, or a value in it (or of WATCH: an expression) */
  char *name, *value, *path;	/* path: "Locals/p/x", to open it again after a step */
  long ref;	/* its children, 0 none */
  int depth, open;
} Var;

typedef struct VarList {
  Var *v;
  int n;
} VarList;

typedef struct Frame {
  long id;
  char *name, *path;
  size_t line;	/* from 1 */
} Frame;

typedef struct Thread {
  long id;
  char *name;
} Thread;

typedef struct CLine {	/* a line of the DEBUG CONSOLE */
  char *s;
  int cat;	/* CC_* */
} CLine;

enum { CC_OUT, CC_ERR, CC_INFO, CC_IN, CC_RESULT };

static struct {
  int on;	/* a session */
  int ready;	/* talking: initialize went */
  int stopped;
  int nodebug;	/* Run Without Debugging */
  int restart;	/* when this one ends, the next starts */
  long long ending;	/* disconnect went at: it ends when answered, or soon after */
  long long waited;	/* the port is waited for since */
  char type[32];	/* the configuration's "type" */
  char name[128];	/* and its "name" */
  char *args;	/* launch's arguments, JSON */
  char *request;	/* "launch" or "attach" */
  OsProc proc;
  long pid;
  int to, from;	/* stdio: its stdin, its stdout */
  int out;	/* TCP: its stdout (the port, its logs) */
  Sock sock;
  Buf in, outline;
  int seq;
  Req req[128];
  int nreq;
  long thread;	/* the thread that stopped */
  char reason[64];
  Thread *th;
  int nth;
  Frame *fr;
  int nfr, cur;	/* the frames of the stopped thread; the one looked at */
  int conf_done;
  int cap_cond, cap_hit, cap_log, cap_setvar, cap_goto, cap_excinfo, cap_exc;	/* what the adapter can */
  char *exc_title, *exc_desc;	/* stopped on an exception: what it says (the peek) */
} D;

static VarList g_vars;	/* VARIABLES: the scopes and their values */
static VarList g_wvars;	/* WATCH: a row for each expression (depth 0) and their values */
static Bp *g_bp;
static int g_nbp, g_capbp;
static char **g_watch;
static int g_nwatch;
static ExcFilter *g_exf;	/* the last adapter's exception filters, and which are on */
static int g_nexf;
static char *g_bp_root;	/* the folder the breakpoints and watches are of */
static int g_bp_dirty;	/* to be saved */
static Vec g_opened;	/* the paths of the variables open, kept from stop to stop */
static CLine *g_con;
static int g_ncon, g_capcon;
static int g_log = -2;	/* $MME_DAPLOG: every message */

/* the launch configurations of .vscode/launch.json */
static Json *g_launch;
static int g_cfg;	/* the one chosen */


static void trace (const char *dir, const char *s, size_t n) {
  if (g_log == -2) {
    char *f = os_getenv("MME_DAPLOG");
    g_log = f ? os_open(f, OS_APPEND) : -1;
    free(f);
  }
  if (g_log < 0) return;
  os_write(g_log, dir, strlen(dir));
  os_write(g_log, s, n);
  os_write(g_log, "\n", 1);
}


/* the same file? (an adapter may write E:/w/a.go for E:\w\a.go) */
static int same_path (const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
#ifdef _WIN32
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
#endif
    if (x != y) return 0;
  }
  return *a == *b;
}


static char *native_path (const char *p) {
  char *s = xstrdup(p), *q;
#ifdef _WIN32
  for (q = s; *q; q++)
    if (*q == '/') *q = '\\';
#else
  (void)q;
#endif
  return s;
}

/* }================================================================== */


/*
** {==================================================================
** Kept from session to session: mme-data/state/debug-<folder>.json
** ===================================================================
*/

static void bp_free (Bp *b) {
  free(b->path);
  free(b->cond);
  free(b->hit);
  free(b->log);
}


static char *state_file (const char *root) {
  char *d = data_path("state"), name[64], *f;
  unsigned long h = 2166136261UL;
  const char *c;
  for (c = root; *c; c++) {	/* FNV-1a of the folder, its case ignored where names are */
#ifdef _WIN32
    h = (h ^ (unsigned char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c == '/' ? '\\' : *c)) * 16777619UL;
#else
    h = (h ^ (unsigned char)*c) * 16777619UL;
#endif
    h &= 0xFFFFFFFFUL;
  }
  mkdir_p(d);
  snprintf(name, sizeof(name), "debug-%08lx.json", h);
  f = path_join(d, name);
  free(d);
  return f;
}


static void put_opt (Buf *b, const char *key, const char *v) {
  if (v == NULL) return;
  buf_printf(b, ",\"%s\":", key);
  json_put_str(b, v, strlen(v));
}


/* the breakpoints, the watches and the exception filters of g_bp_root, written */
static void bp_save (void) {
  Buf b;
  char *f;
  int i, first = 1, fd;
  g_bp_dirty = 0;
  if (g_bp_root == NULL) return;
  buf_init(&b);
  buf_puts(&b, "{\"breakpoints\":[");
  for (i = 0; i < g_nbp; i++) {
    if (g_bp[i].temp) continue;
    buf_puts(&b, first ? "\n  {\"path\":" : ",\n  {\"path\":");
    first = 0;
    json_put_str(&b, g_bp[i].path, strlen(g_bp[i].path));
    buf_printf(&b, ",\"line\":%lu,\"enabled\":%s", (unsigned long)(g_bp[i].line + 1), g_bp[i].enabled ? "true" : "false");
    put_opt(&b, "condition", g_bp[i].cond);
    put_opt(&b, "hitCondition", g_bp[i].hit);
    put_opt(&b, "logMessage", g_bp[i].log);
    buf_putc(&b, '}');
  }
  buf_puts(&b, "\n],\n\"watch\":[");
  for (i = 0; i < g_nwatch; i++) {
    if (i) buf_putc(&b, ',');
    json_put_str(&b, g_watch[i], strlen(g_watch[i]));
  }
  buf_puts(&b, "],\n\"exceptions\":[");
  for (i = 0; i < g_nexf; i++) {
    buf_puts(&b, i ? ",{\"filter\":" : "{\"filter\":");
    json_put_str(&b, g_exf[i].filter, strlen(g_exf[i].filter));
    buf_puts(&b, ",\"label\":");
    json_put_str(&b, g_exf[i].label, strlen(g_exf[i].label));
    buf_printf(&b, ",\"enabled\":%s}", g_exf[i].enabled ? "true" : "false");
  }
  buf_puts(&b, "]}\n");
  f = state_file(g_bp_root);
  if (g_nbp == 0 && g_nwatch == 0 && g_nexf == 0) os_unlink(f);	/* nothing kept: no file */
  else if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  free(f);
  buf_free(&b);
}


static char *opt_str (const Json *o, const char *key) {
  const char *v = json_str(json_get(o, key), NULL);
  return v && *v ? xstrdup(v) : NULL;
}


static void bp_load (void) {
  char *f = state_file(g_bp_root), *s;
  size_t len, i;
  Json *j;
  s = read_file(f, &len);
  free(f);
  if (s == NULL) return;
  j = json_parse(s, len);
  free(s);
  {
    const Json *l = json_get(j, "breakpoints");
    for (i = 0; l && l->type == J_ARR && i < l->n; i++) {
      const Json *o = l->kid[i];
      const char *p = json_str(json_get(o, "path"), NULL);
      Bp *b;
      if (p == NULL) continue;
      if (g_nbp == g_capbp) {
        g_capbp = g_capbp ? g_capbp * 2 : 16;
        g_bp = (Bp *)xrealloc(g_bp, (size_t)g_capbp * sizeof(Bp));
      }
      b = &g_bp[g_nbp++];
      memset(b, 0, sizeof(*b));
      b->path = xstrdup(p);
      b->line = (size_t)json_num(json_get(o, "line"), 1) - 1;
      b->enabled = json_bool(json_get(o, "enabled"), 1);
      b->id = -1;
      b->cond = opt_str(o, "condition");
      b->hit = opt_str(o, "hitCondition");
      b->log = opt_str(o, "logMessage");
    }
  }
  {
    const Json *l = json_get(j, "watch");
    for (i = 0; l && l->type == J_ARR && i < l->n; i++)
      if (l->kid[i]->type == J_STR) {
        g_watch = (char **)xrealloc(g_watch, (size_t)(g_nwatch + 1) * sizeof(char *));
        g_watch[g_nwatch++] = xstrdup(l->kid[i]->str);
      }
  }
  {
    const Json *l = json_get(j, "exceptions");
    for (i = 0; l && l->type == J_ARR && i < l->n; i++) {
      const char *fl = json_str(json_get(l->kid[i], "filter"), NULL), *lb = json_str(json_get(l->kid[i], "label"), fl);
      if (fl == NULL) continue;
      g_exf = (ExcFilter *)xrealloc(g_exf, (size_t)(g_nexf + 1) * sizeof(ExcFilter));
      g_exf[g_nexf].filter = xstrdup(fl);
      g_exf[g_nexf].label = xstrdup(lb);
      g_exf[g_nexf].enabled = json_bool(json_get(l->kid[i], "enabled"), 0);
      g_nexf++;
    }
  }
  json_free(j);
}


static void wrows_reset (void);

/* the folder open changed (or the first look): its breakpoints and watches */
static void bp_sync (void) {
  const char *root = side_root();
  int i;
  if (g_bp_root && strcmp(g_bp_root, root) == 0) return;
  if (g_bp_root && g_bp_dirty) bp_save();
  for (i = 0; i < g_nbp; i++) bp_free(&g_bp[i]);
  g_nbp = 0;
  for (i = 0; i < g_nwatch; i++) free(g_watch[i]);
  g_nwatch = 0;
  for (i = 0; i < g_nexf; i++) {
    free(g_exf[i].filter);
    free(g_exf[i].label);
  }
  g_nexf = 0;
  free(g_bp_root);
  g_bp_root = xstrdup(root);
  bp_load();
  wrows_reset();
}

/* }================================================================== */


/*
** {==================================================================
** The debug console
** ===================================================================
*/

static struct {
  char in[512];	/* what is typed */
  int top;	/* lines scrolled up from the end */
  char *hist[32];
  int nhist, at;
  int h;
} CN;


/* text into the console, line by line; an unfinished last line is added to */
static void con_add (const char *s, size_t n, int cat) {
  static int open_line = 0, open_cat = -1;
  while (n > 0) {
    const char *e = memchr(s, '\n', n);
    size_t len = e ? (size_t)(e - s) : n;
    if (len > 0 && s[len - 1] == '\r') len--;
    if (open_line && open_cat == cat && g_ncon > 0) {	/* the rest of the last line */
      CLine *c = &g_con[g_ncon - 1];
      size_t ol = strlen(c->s);
      c->s = (char *)xrealloc(c->s, ol + len + 1);
      memcpy(c->s + ol, s, len);
      c->s[ol + len] = '\0';
    }
    else {
      if (g_ncon == 5000) {	/* the oldest go */
        int i;
        for (i = 0; i < 1000; i++) free(g_con[i].s);
        memmove(g_con, g_con + 1000, (size_t)(g_ncon - 1000) * sizeof(CLine));
        g_ncon -= 1000;
      }
      if (g_ncon == g_capcon) {
        g_capcon = g_capcon ? g_capcon * 2 : 256;
        g_con = (CLine *)xrealloc(g_con, (size_t)g_capcon * sizeof(CLine));
      }
      g_con[g_ncon].s = xstrndup(s, len);
      g_con[g_ncon].cat = cat;
      g_ncon++;
    }
    open_line = e == NULL;
    open_cat = cat;
    if (e == NULL) break;
    n -= (size_t)(e - s) + 1;
    s = e + 1;
  }
}


static void con_print (int cat, const char *fmt, const char *arg) {
  char b[1024];
  snprintf(b, sizeof(b), fmt, arg);
  con_add(b, strlen(b), cat);
  con_add("\n", 1, cat);
}

/* }================================================================== */


/*
** {==================================================================
** Talking to the adapter
** ===================================================================
*/

static Sock tcp_connect (int port) {
  struct sockaddr_in a;
  Sock s;
#ifdef _WIN32
  static int wsa = 0;
  if (!wsa) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return NO_SOCK;
    wsa = 1;
  }
#endif
  s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == NO_SOCK) return s;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(0x7F000001);	/* 127.0.0.1 */
  if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
    sock_close(s);
    return NO_SOCK;
  }
  return s;
}


static int sock_ready (Sock s) {
  fd_set r;
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  FD_ZERO(&r);
  FD_SET(s, &r);
  return select((int)s + 1, &r, NULL, NULL, &tv) > 0;
}


static void send_msg (const Buf *body) {
  char hdr[64];
  int n = snprintf(hdr, sizeof(hdr), "Content-Length: %lu\r\n\r\n", (unsigned long)body->len);
  trace(">> ", body->s, body->len);
  if (D.sock != NO_SOCK) {
    const char *parts[2];
    size_t lens[2], i;
    parts[0] = hdr;
    lens[0] = (size_t)n;
    parts[1] = body->s;
    lens[1] = body->len;
    for (i = 0; i < 2; i++) {
      size_t done = 0;
      while (done < lens[i]) {
        int k = (int)send(D.sock, parts[i] + done, (int)(lens[i] - done), 0);
        if (k <= 0) return;
        done += (size_t)k;
      }
    }
  }
  else if (D.to >= 0) {
    os_write(D.to, hdr, (size_t)n);
    os_write(D.to, body->s, body->len);
  }
}


/* a request: {"seq":n,"type":"request","command":c,"arguments":args}; its seq */
static int request (const char *command, const char *args, int kind, long arg, const char *path) {
  Buf b;
  int seq = ++D.seq;
  buf_init(&b);
  buf_printf(&b, "{\"seq\":%d,\"type\":\"request\",\"command\":\"%s\"", seq, command);
  if (args) buf_printf(&b, ",\"arguments\":%s", args);
  buf_putc(&b, '}');
  send_msg(&b);
  buf_free(&b);
  if (D.nreq == 128) {	/* the oldest is forgotten */
    free(D.req[0].path);
    memmove(D.req, D.req + 1, 127 * sizeof(Req));
    D.nreq--;
  }
  D.req[D.nreq].seq = seq;
  D.req[D.nreq].kind = kind;
  D.req[D.nreq].arg = arg;
  D.req[D.nreq].path = path ? xstrdup(path) : NULL;
  D.req[D.nreq].list = 0;
  D.nreq++;
  return seq;
}


/* the answer to a request of the adapter's (runInTerminal ...): not done */
static void refuse (const Json *msg) {
  Buf b;
  buf_init(&b);
  buf_printf(&b, "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,\"success\":false,\"command\":\"%s\","
                 "\"message\":\"not supported by mme\"}",
             ++D.seq, (int)json_num(json_get(msg, "seq"), 0), json_str(json_get(msg, "command"), ""));
  send_msg(&b);
  buf_free(&b);
}

/* }================================================================== */


/*
** {==================================================================
** Breakpoints
** ===================================================================
*/

int dbg_mark (const char *path, size_t line) {
  int i, m = 0;
  if (path == NULL) return 0;
  bp_sync();
  for (i = 0; i < g_nbp; i++)
    if (g_bp[i].line == line && same_path(g_bp[i].path, path) && !g_bp[i].temp) {
      m |= DM_BP;
      if (g_bp[i].log) m |= DM_LOG;
      else if (g_bp[i].cond || g_bp[i].hit) m |= DM_COND;
      if (!g_bp[i].enabled) m |= DM_DISABLED;
      else if (D.on && D.ready && !D.nodebug && !g_bp[i].verified) m |= DM_UNVERIFIED;
    }
  if (D.stopped && D.nfr > 0) {
    for (i = 0; i < D.nfr; i++)
      if (D.fr[i].path && D.fr[i].line == line + 1 && same_path(D.fr[i].path, path)) {
        if (i == 0) m |= DM_TOP;
        else if (i == D.cur) m |= DM_FRAME;
      }
  }
  return m;
}


/* the adapter is told the breakpoints of a file (all of them, the enabled ones) */
static void send_bps (const char *path) {
  Buf b;
  int i, first = 1;
  if (!D.on || !D.ready || D.nodebug) return;
  buf_init(&b);
  buf_puts(&b, "{\"source\":{\"path\":");
  json_put_str(&b, path, strlen(path));
  buf_puts(&b, ",\"name\":");
  json_put_str(&b, path_basename(path), strlen(path_basename(path)));
  buf_puts(&b, "},\"breakpoints\":[");
  for (i = 0; i < g_nbp; i++)
    if (g_bp[i].enabled && same_path(g_bp[i].path, path)) {
      buf_printf(&b, "%s{\"line\":%lu", first ? "" : ",", (unsigned long)(g_bp[i].line + 1));
      put_opt(&b, "condition", g_bp[i].cond);
      put_opt(&b, "hitCondition", g_bp[i].hit);
      put_opt(&b, "logMessage", g_bp[i].log);
      buf_putc(&b, '}');
      first = 0;
    }
  buf_puts(&b, "],\"sourceModified\":false}");
  request("setBreakpoints", b.s, RQ_SETBP, 0, path);
  buf_free(&b);
}


static void send_all_bps (void) {
  int i, j;
  for (i = 0; i < g_nbp; i++) {
    for (j = 0; j < i; j++)
      if (same_path(g_bp[j].path, g_bp[i].path)) break;
    if (j == i) send_bps(g_bp[i].path);
  }
}


/* the breakpoint on a line, -1 none */
static int bp_at (const char *path, size_t line) {
  int i;
  for (i = 0; i < g_nbp; i++)
    if (g_bp[i].line == line && !g_bp[i].temp && same_path(g_bp[i].path, path)) return i;
  return -1;
}


static int bp_add (const char *path, size_t line) {
  Bp *b;
  if (g_nbp == g_capbp) {
    g_capbp = g_capbp ? g_capbp * 2 : 16;
    g_bp = (Bp *)xrealloc(g_bp, (size_t)g_capbp * sizeof(Bp));
  }
  b = &g_bp[g_nbp];
  memset(b, 0, sizeof(*b));
  b->path = xstrdup(path);
  b->line = line;
  b->enabled = 1;
  b->id = -1;
  g_bp_dirty = 1;
  return g_nbp++;
}


static void bp_remove (int i) {
  char *p;
  if (i < 0 || i >= g_nbp) return;
  p = xstrdup(g_bp[i].path);
  bp_free(&g_bp[i]);
  memmove(g_bp + i, g_bp + i + 1, (size_t)(g_nbp - i - 1) * sizeof(Bp));
  g_nbp--;
  g_bp_dirty = 1;
  send_bps(p);
  free(p);
}


/* F9: a breakpoint on the line, or not any more */
void dbg_toggle (const char *path, size_t line) {
  int i;
  if (path == NULL) {
    toast(0, "Save the file to set breakpoints in it");
    return;
  }
  bp_sync();
  if ((i = bp_at(path, line)) >= 0) {
    bp_remove(i);
    return;
  }
  bp_add(path, line);
  send_bps(path);
}


static void bp_enable (int i) {
  if (i < 0 || i >= g_nbp) return;
  g_bp[i].enabled = !g_bp[i].enabled;
  g_bp_dirty = 1;
  send_bps(g_bp[i].path);
}


/* Disable / Enable Breakpoint, on a line */
void dbg_enable_bp (const char *path, size_t line) {
  int i;
  if (path == NULL) return;
  bp_sync();
  if ((i = bp_at(path, line)) >= 0) bp_enable(i);
}


static void set_field (char **f, const char *v) {
  free(*f);
  *f = v && *v ? xstrdup(v) : NULL;
}


/*
** VS Code's breakpoint widget, as an input: mode 0 Expression (break when
** it is true), 1 Hit Count, 2 Log Message (a logpoint: {x} is x's value);
** -1: which one is asked first. A breakpoint is made when there is none.
*/
void dbg_edit_bp (const char *path, size_t line, int mode) {
  static const char *const what[] = {"Expression", "Hit Count", "Log Message"};
  static const char *const hint[] = {"Break when expression evaluates to true",
                                     "Break when hit count condition is met (5, >=5, %2)",
                                     "Message to log when breakpoint is hit. Expressions within {} are interpolated."};
  int i;
  char *v, title[256];
  const char *old;
  if (path == NULL) {
    toast(0, "Save the file to set breakpoints in it");
    return;
  }
  bp_sync();
  i = bp_at(path, line);
  if (mode < 0) {
    Pick p;
    int k, r;
    pick_init(&p, "Edit Breakpoint");
    for (k = 0; k < 3; k++) {
      const char *cur = i < 0 ? NULL : k == 0 ? g_bp[i].cond : k == 1 ? g_bp[i].hit : g_bp[i].log;
      pick_add(&p, what[k], cur, k == 2 ? 0xEAAB : 0xEAA7);
    }
    p.keep_order = 1;
    p.start = i >= 0 && g_bp[i].log ? 2 : i >= 0 && g_bp[i].hit && !g_bp[i].cond ? 1 : 0;
    r = pick_run(&p);
    pick_free(&p);
    if (r < 0) return;
    mode = r;
  }
  if (mode > 2) mode = 2;
  old = i < 0 ? NULL : mode == 0 ? g_bp[i].cond : mode == 1 ? g_bp[i].hit : g_bp[i].log;
  snprintf(title, sizeof(title), "%s  (line %lu: %s)", hint[mode], (unsigned long)(line + 1), what[mode]);
  v = ask_text(title, old ? old : "");
  if (v == NULL) return;
  if (i < 0) {
    if (*v == '\0') {	/* nothing said: no breakpoint */
      free(v);
      return;
    }
    i = bp_add(path, line);
  }
  set_field(mode == 0 ? &g_bp[i].cond : mode == 1 ? &g_bp[i].hit : &g_bp[i].log, v);
  free(v);
  g_bp_dirty = 1;
  if (mode == 0 && g_bp[i].cond && D.on && D.ready && !D.cap_cond) toast(1, "The debug adapter does not support conditional breakpoints");
  if (mode == 2 && g_bp[i].log && D.on && D.ready && !D.cap_log) toast(1, "The debug adapter does not support logpoints");
  send_bps(g_bp[i].path);
}


/*
** delta lines went in (delta > 0) at line first of path, or -delta lines
** from line first on went out: the breakpoints stay with their text (those
** on lines that went, on the line before)
*/
void dbg_lines (const char *path, size_t first, long delta) {
  int i;
  if (path == NULL || delta == 0) return;
  for (i = 0; i < g_nbp; i++) {
    Bp *b = &g_bp[i];
    if (b->line < first || !same_path(b->path, path)) continue;
    if (delta < 0 && b->line < first + (size_t)(-delta)) b->line = first > 0 ? first - 1 : 0;
    else b->line = (size_t)((long)b->line + delta);
    g_bp_dirty = 1;
  }
}


/* Run to Cursor's breakpoints go when the program stops (tell: the adapter is told) */
static void drop_temp (int tell) {
  int i;
  for (i = g_nbp - 1; i >= 0; i--)
    if (g_bp[i].temp) {
      if (tell) bp_remove(i);
      else {
        bp_free(&g_bp[i]);
        memmove(g_bp + i, g_bp + i + 1, (size_t)(g_nbp - i - 1) * sizeof(Bp));
        g_nbp--;
      }
    }
}


/* the exception filters that are on, to the adapter */
static void send_exc (void) {
  Buf b;
  int i, first = 1;
  if (!D.on || !D.ready || D.nodebug) return;
  buf_init(&b);
  buf_puts(&b, "{\"filters\":[");
  for (i = 0; D.cap_exc && i < g_nexf; i++)	/* an adapter without filters gets none */
    if (g_exf[i].enabled) {
      if (!first) buf_putc(&b, ',');
      json_put_str(&b, g_exf[i].filter, strlen(g_exf[i].filter));
      first = 0;
    }
  buf_puts(&b, "]}");
  request("setExceptionBreakpoints", b.s, RQ_OTHER, 0, NULL);
  buf_free(&b);
}


/* initialize's answer: what the adapter can, and its exception filters (on as they were) */
static void got_caps (const Json *c) {
  const Json *l = json_get(c, "exceptionBreakpointFilters");
  ExcFilter *nf = NULL;
  int n = 0, i, k;
  D.cap_cond = json_bool(json_get(c, "supportsConditionalBreakpoints"), 0);
  D.cap_hit = json_bool(json_get(c, "supportsHitConditionalBreakpoints"), 0);
  D.cap_log = json_bool(json_get(c, "supportsLogPoints"), 0);
  D.cap_setvar = json_bool(json_get(c, "supportsSetVariable"), 0);
  D.cap_goto = json_bool(json_get(c, "supportsGotoTargetsRequest"), 0);
  D.cap_excinfo = json_bool(json_get(c, "supportsExceptionInfoRequest"), 0);
  if (l == NULL || l->type != J_ARR) return;
  D.cap_exc = 1;
  nf = (ExcFilter *)xmalloc((l->n + 1) * sizeof(ExcFilter));
  for (i = 0; i < (int)l->n; i++) {
    const char *f = json_str(json_get(l->kid[i], "filter"), NULL);
    if (f == NULL) continue;
    nf[n].filter = xstrdup(f);
    nf[n].label = xstrdup(json_str(json_get(l->kid[i], "label"), f));
    nf[n].enabled = json_bool(json_get(l->kid[i], "default"), 0);
    for (k = 0; k < g_nexf; k++)	/* as the user left it */
      if (strcmp(g_exf[k].filter, f) == 0) nf[n].enabled = g_exf[k].enabled;
    n++;
  }
  for (k = 0; k < g_nexf; k++) {
    free(g_exf[k].filter);
    free(g_exf[k].label);
  }
  free(g_exf);
  g_exf = nf;
  g_nexf = n;
  g_bp_dirty = 1;
}

/* }================================================================== */


/*
** {==================================================================
** Launch configurations
** ===================================================================
*/

static char *launch_path (void) {
  char *dir = path_join(side_root(), ".vscode"), *f = path_join(dir, "launch.json");
  free(dir);
  return f;
}


static void load_launch (void) {
  char *f = launch_path(), *s;
  size_t len;
  json_free(g_launch);
  g_launch = NULL;
  s = read_file(f, &len);
  free(f);
  if (s == NULL) return;
  g_launch = json_parse(s, len);
  free(s);
}


static const Json *configs (int *n) {
  const Json *c = json_get(g_launch, "configurations");
  *n = (c && c->type == J_ARR) ? (int)c->n : 0;
  return c;
}


static const char *config_name (int i) {
  int n;
  const Json *c = configs(&n);
  if (i < 0 || i >= n) return NULL;
  return json_str(json_get(c->kid[i], "name"), "Launch");
}


/* "Add Configuration": .vscode/launch.json, made with a configuration for the file's language */
static void create_launch (void) {
  char *dir = path_join(side_root(), ".vscode"), *f = path_join(dir, "launch.json");
  const char *file = editor_file(), *ext = file ? strrchr(file, '.') : NULL;
  OsStat st;
  if (os_stat(f, &st) != 0 || !st.exists) {
    Buf b;
    int fd;
    buf_init(&b);
    buf_puts(&b, "{\n"
                 "  // Use IntelliSense to learn about possible attributes.\n"
                 "  // Hover to view descriptions of existing attributes.\n"
                 "  // For more information, visit: https://go.microsoft.com/fwlink/?linkid=830387\n"
                 "  \"version\": \"0.2.0\",\n"
                 "  \"configurations\": [\n");
    if (ext && strcmp(ext, ".py") == 0)
      buf_puts(&b, "    {\n"
                   "      \"name\": \"Python Debugger: Current File\",\n"
                   "      \"type\": \"debugpy\",\n"
                   "      \"request\": \"launch\",\n"
                   "      \"program\": \"${file}\",\n"
                   "      \"console\": \"internalConsole\"\n"
                   "    }\n");
    else if (ext && (strcmp(ext, ".c") == 0 || strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 ||
                     strcmp(ext, ".h") == 0 || strcmp(ext, ".rs") == 0 || strcmp(ext, ".zig") == 0))
      buf_puts(&b, "    {\n"
                   "      \"name\": \"Debug\",\n"
                   "      \"type\": \"lldb-dap\",\n"
                   "      \"request\": \"launch\",\n"
                   "      \"program\": \"${workspaceFolder}/${fileBasenameNoExtension}\",\n"
                   "      \"args\": [],\n"
                   "      \"cwd\": \"${workspaceFolder}\"\n"
                   "    },\n"
                   "    {\n"
                   "      \"name\": \"Debug (gdb)\",\n"
                   "      \"type\": \"gdb\",\n"
                   "      \"request\": \"launch\",\n"
                   "      \"program\": \"${workspaceFolder}/${fileBasenameNoExtension}\",\n"
                   "      \"args\": [],\n"
                   "      \"cwd\": \"${workspaceFolder}\"\n"
                   "    }\n");
    else
      buf_puts(&b, "    {\n"
                   "      \"name\": \"Launch Package\",\n"
                   "      \"type\": \"go\",\n"
                   "      \"request\": \"launch\",\n"
                   "      \"mode\": \"auto\",\n"
                   "      \"program\": \"${fileDirname}\"\n"
                   "    }\n");
    buf_puts(&b, "  ]\n}\n");
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


/* no launch.json: a configuration for the file in front, as VS Code's "Select debugger" makes */
static Json *default_config (void) {
  const char *file = editor_file(), *ext = file ? strrchr(file, '.') : NULL;
  const char *text;
  if (ext && strcmp(ext, ".go") == 0)
    text = "{\"name\":\"Launch Package\",\"type\":\"go\",\"request\":\"launch\",\"mode\":\"auto\","
           "\"program\":\"${fileDirname}\"}";
  else if (ext && strcmp(ext, ".py") == 0)
    text = "{\"name\":\"Python Debugger: Current File\",\"type\":\"debugpy\",\"request\":\"launch\","
           "\"program\":\"${file}\",\"console\":\"internalConsole\"}";
  else return NULL;
  return json_parse(text, strlen(text));
}


/* ${file} ... in every string of the configuration */
static void subst_tree (Json *j) {
  size_t i;
  if (j == NULL) return;
  if (j->type == J_STR && strstr(j->str, "${")) {
    char *s = vs_subst(j->str);
    free(j->str);
    j->str = s;
    j->len = strlen(s);
  }
  for (i = 0; (j->type == J_ARR || j->type == J_OBJ) && i < j->n; i++) subst_tree(j->kid[i]);
}


static Json *member (Json *j, const char *key) {
  size_t i;
  for (i = 0; j && j->type == J_OBJ && i < j->n; i++)
    if (j->kid[i]->key && strcmp(j->kid[i]->key, key) == 0) return j->kid[i];
  return NULL;
}


/* the adapter's command line for a type: settings (mme.debugAdapters), else what VS Code's extensions run */
static char *adapter_cmd (const char *type) {
  const Json *set = settings_get("mme\\.debugAdapters");
  const char *s = json_str(json_get(set, type), NULL);
  if (s && *s) return xstrdup(s);
  if (strcmp(type, "go") == 0) {
    char *p = find_program("dlv"), *home, *c;
    if (p == NULL) {	/* where "go install" puts it */
      char *gp = os_getenv("GOPATH");
      home = gp ? gp : os_getenv("USERPROFILE");
      if (home == NULL) home = os_getenv("HOME");
      if (home) {
        char *bin = gp ? path_join(home, "bin") : path_join(home, "go" MMC_SEPS "bin");
#ifdef _WIN32
        char *exe = path_join(bin, "dlv.exe");
#else
        char *exe = path_join(bin, "dlv");
#endif
        if (os_is_exec(exe)) p = exe;
        else free(exe);
        free(bin);
      }
      free(home);
    }
    if (p == NULL) return NULL;
    c = xstrcat3("\"", p, "\" dap --listen=127.0.0.1:0");
    free(p);
    return c;
  }
  if (strcmp(type, "debugpy") == 0 || strcmp(type, "python") == 0) {
#ifdef _WIN32
    const char *py = "python";
#else
    const char *py = "python3";
#endif
    char *ext = vscode_find_ext("ms-python.debugpy");	/* VS Code's Python Debugger has the adapter */
    if (ext) {
      char *ad = path_join(ext, "bundled" MMC_SEPS "libs" MMC_SEPS "debugpy" MMC_SEPS "adapter");
      OsStat st;
      free(ext);
      if (os_stat(ad, &st) == 0 && st.is_dir) {
        char *prog = find_program(py), *c;
        Buf b;
        buf_init(&b);
        buf_printf(&b, "\"%s\" \"%s\"", prog ? prog : py, ad);
        buf_putc(&b, '\0');
        c = buf_take(&b);
        free(prog);
        free(ad);
        return c;
      }
      free(ad);
    }
    return xstrcat3(py, " -m debugpy.adapter", "");
  }
  if (strcmp(type, "lldb-dap") == 0 || strcmp(type, "lldb") == 0 || strcmp(type, "lldb-vscode") == 0)
    return xstrdup("lldb-dap");
  if (strcmp(type, "gdb") == 0 || strcmp(type, "cppdbg") == 0) return xstrdup("gdb -i dap");
  return NULL;
}


/* "a b" "c": the words, quotes taken away; the program found in PATH */
static char **split_cmd (const char *cmd) {
  char **argv = (char **)xmalloc(33 * sizeof(char *));
  int n = 0;
  while (*cmd && n < 32) {
    Buf w;
    while (*cmd == ' ') cmd++;
    if (!*cmd) break;
    buf_init(&w);
    while (*cmd && *cmd != ' ') {
      if (*cmd == '"') {
        for (cmd++; *cmd && *cmd != '"'; cmd++) buf_putc(&w, *cmd);
        if (*cmd) cmd++;
      }
      else buf_putc(&w, *cmd++);
    }
    buf_putc(&w, '\0');
    argv[n++] = buf_take(&w);
  }
  argv[n] = NULL;
  if (n > 0 && !path_is_sep(argv[0][0]) && !(argv[0][0] && argv[0][1] == ':')) {
    char *full = find_program(argv[0]);
    if (full) {
      free(argv[0]);
      argv[0] = full;
    }
  }
  return argv;
}

/* }================================================================== */


/*
** {==================================================================
** The session
** ===================================================================
*/

static void clear_stop (void) {
  int i;
  for (i = 0; i < D.nfr; i++) {
    free(D.fr[i].name);
    free(D.fr[i].path);
  }
  free(D.fr);
  D.fr = NULL;
  D.nfr = D.cur = 0;
  for (i = 0; i < g_vars.n; i++) {
    free(g_vars.v[i].name);
    free(g_vars.v[i].value);
    free(g_vars.v[i].path);
  }
  free(g_vars.v);
  g_vars.v = NULL;
  g_vars.n = 0;
  D.stopped = 0;
  free(D.exc_title);
  free(D.exc_desc);
  D.exc_title = D.exc_desc = NULL;
  wrows_reset();
}


/* WATCH's rows: one for each expression, its value not known */
static void wrows_reset (void) {
  int i;
  for (i = 0; i < g_wvars.n; i++) {
    free(g_wvars.v[i].name);
    free(g_wvars.v[i].value);
    free(g_wvars.v[i].path);
  }
  g_wvars.v = (Var *)xrealloc(g_wvars.v, (size_t)(g_nwatch + 1) * sizeof(Var));
  g_wvars.n = g_nwatch;
  for (i = 0; i < g_nwatch; i++) {
    Var *v = &g_wvars.v[i];
    v->name = xstrdup(g_watch[i]);
    v->value = NULL;
    v->path = xstrcat3("#w/", g_watch[i], "");
    v->ref = 0;
    v->depth = 0;
    v->open = 0;
  }
}


/* the row of watch i in WATCH (its children follow it) */
static int wrow_of (int w) {
  int i, k = 0;
  for (i = 0; i < g_wvars.n; i++)
    if (g_wvars.v[i].depth == 0 && k++ == w) return i;
  return -1;
}


static void clear_threads (void) {
  int i;
  for (i = 0; i < D.nth; i++) free(D.th[i].name);
  free(D.th);
  D.th = NULL;
  D.nth = 0;
}


static int dbg_start (int nodebug);

static void end_session (void);

static void step (const char *command);

/* the adapter went away by itself */
static void adapter_gone (void) {
  if (D.ending == 0 && !D.restart) {
    con_print(CC_ERR, "%s", "The debug adapter ended unexpectedly.");
    toast(1, "The debug adapter ended unexpectedly");
  }
  end_session();
}


static void end_session (void) {
  int i, again = D.restart, nd = D.nodebug;
  if (!D.on) return;
  if (D.sock != NO_SOCK) sock_close(D.sock);
  if (D.to >= 0) os_close(D.to);
  if (D.from >= 0) os_close(D.from);
  if (D.out >= 0) os_close(D.out);
  if (D.pid > 0) os_kill(D.pid, 9);
  clear_stop();
  clear_threads();
  for (i = 0; i < D.nreq; i++) free(D.req[i].path);
  buf_free(&D.in);
  buf_free(&D.outline);
  free(D.args);
  free(D.request);
  drop_temp(0);
  memset(&D, 0, sizeof(D));
  D.sock = NO_SOCK;
  D.to = D.from = D.out = -1;
  for (i = 0; i < g_nbp; i++) g_bp[i].verified = 0;
  on_debug(DE_END, NULL, 0);
  if (again) dbg_start(nd);
}


/* the configuration to run: launch.json's chosen one, else one for the file */
static char *g_override;	/* dbg_start_json's configuration, for the next start */

static Json *pick_config (void) {
  int n;
  const Json *c;
  Buf b;
  Json *j;
  if (g_override) {	/* Test: Debug Test at Cursor's */
    j = json_parse(g_override, strlen(g_override));
    free(g_override);
    g_override = NULL;
    return j;
  }
  load_launch();
  c = configs(&n);
  if (n == 0) return default_config();
  if (g_cfg >= n) g_cfg = 0;
  buf_init(&b);	/* a copy of it, to change */
  json_write(&b, c->kid[g_cfg]);
  j = json_parse(b.s, b.len);
  buf_free(&b);
  return j;
}


static int dbg_start (int nodebug) {
  Json *cfg, *m;
  char *cmd, **argv, *root;
  int to[2], from[2], io[3], null, i, tcp, r;
  bp_sync();
  cfg = pick_config();
  if (cfg == NULL) {
    int n;
    configs(&n);
    toast(0, "No launch configuration for this file: Run > Add Configuration makes one");
    return -1;
  }
  subst_tree(cfg);
  snprintf(D.type, sizeof(D.type), "%s", json_str(json_get(cfg, "type"), ""));
  snprintf(D.name, sizeof(D.name), "%s", json_str(json_get(cfg, "name"), D.type));
  D.request = xstrdup(json_str(json_get(cfg, "request"), "launch"));
  if ((m = member(cfg, "mode")) != NULL && m->type == J_STR && strcmp(m->str, "auto") == 0) {	/* Go's "auto" */
    const char *prog = json_str(json_get(cfg, "program"), "");
    size_t pl = strlen(prog);
    free(m->str);
    m->str = xstrdup(pl > 8 && strcmp(prog + pl - 8, "_test.go") == 0 ? "test" : "debug");
    m->len = strlen(m->str);
  }
  if ((strcmp(D.type, "debugpy") == 0 || strcmp(D.type, "python") == 0) && (m = member(cfg, "console")) != NULL &&
      m->type == J_STR) {	/* no terminal to run it in: its output comes to the console */
    free(m->str);
    m->str = xstrdup("internalConsole");
    m->len = strlen(m->str);
  }
  cmd = adapter_cmd(D.type);
  if (cmd == NULL) {
    toast(1, "No debug adapter for type '%s': set mme.debugAdapters in settings.json", D.type);
    json_free(cfg);
    free(D.request);
    D.request = NULL;
    return -1;
  }
  {	/* launch's arguments: the configuration, and noDebug for Run Without Debugging */
    Buf b;
    buf_init(&b);
    json_write(&b, cfg);
    if (nodebug && b.len > 1 && b.s[b.len - 1] == '}') {
      b.len--;
      buf_puts(&b, b.len > 1 ? ",\"noDebug\":true}" : "\"noDebug\":true}");
    }
    buf_putc(&b, '\0');
    D.args = buf_take(&b);
  }
  json_free(cfg);
  argv = split_cmd(cmd);
  tcp = argv[0] && strstr(path_basename(argv[0]), "dlv") != NULL;	/* dlv dap listens on a port */
  free(cmd);
  if (argv[0] == NULL || os_pipe(to) != 0 || os_pipe(from) != 0) {
    toast(1, "The debug adapter could not start");
    for (i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
    return -1;
  }
#ifdef _WIN32
  null = os_open("NUL", OS_WRITE);
#else
  null = os_open("/dev/null", OS_WRITE);
#endif
  io[0] = to[0];
  io[1] = from[1];
  io[2] = tcp ? from[1] : null;
  root = xstrdup(side_root());
  {
    char *cwd = os_getcwd();
    os_chdir(root);
    r = os_spawn(argv[0], argv, NULL, io, 3, &D.proc, &D.pid);
    if (cwd) os_chdir(cwd);
    free(cwd);
  }
  free(root);
  os_close(to[0]);
  os_close(from[1]);
  if (null >= 0) os_close(null);
  if (r != 0) {
    toast(1, "The debug adapter '%s' could not start", argv[0]);
    os_close(to[1]);
    os_close(from[0]);
    for (i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
    free(D.args);
    D.args = NULL;
    return -1;
  }
  for (i = 0; argv[i]; i++) free(argv[i]);
  free(argv);
  D.on = 1;
  D.nodebug = nodebug;
  D.sock = NO_SOCK;
  buf_init(&D.in);
  buf_init(&D.outline);
  if (tcp) {	/* its stdout says the port */
    D.to = -1;
    os_close(to[1]);
    D.from = -1;
    D.out = from[0];
    D.waited = os_now_us();
  }
  else {
    D.to = to[1];
    D.from = from[0];
    D.out = -1;
  }
  con_print(CC_INFO, "%s", nodebug ? "Running..." : "Starting the debugger...");
  on_debug(DE_START, NULL, 0);
  if (!tcp) {
    D.ready = 1;
    request("initialize", "{\"clientID\":\"mme\",\"clientName\":\"mme\",\"adapterID\":\"mme\",\"pathFormat\":\"path\","
                          "\"linesStartAt1\":true,\"columnsStartAt1\":true,\"supportsVariableType\":true,"
                          "\"supportsRunInTerminalRequest\":false,\"locale\":\"en\"}", RQ_INIT, 0, NULL);
  }
  return 0;
}


/* a session with the configuration given (the Testing view's Debug Test) */
void dbg_start_json (const char *config) {
  if (dbg_active()) {
    toast(0, "A debug session is running already.");
    return;
  }
  free(g_override);
  g_override = xstrdup(config);
  if (dbg_start(0) != 0) {
    free(g_override);
    g_override = NULL;
  }
}


static void stop_session (void) {
  if (!D.on) return;
  if (!D.ready) {
    end_session();
    return;
  }
  if (D.ending == 0) {
    D.ending = os_now_us();
    request("disconnect", "{\"restart\":false,\"terminateDebuggee\":true}", RQ_DISCONNECT, 0, NULL);
  }
}


static void ask_stack (void) {
  char a[96];
  snprintf(a, sizeof(a), "{\"threadId\":%ld,\"startFrame\":0,\"levels\":50}", D.thread);
  request("stackTrace", a, RQ_STACK, D.thread, NULL);
}


static void ask_scopes (void) {
  char a[64];
  int i;
  for (i = 0; i < g_vars.n; i++) {
    free(g_vars.v[i].name);
    free(g_vars.v[i].value);
    free(g_vars.v[i].path);
  }
  g_vars.n = 0;
  if (D.cur >= D.nfr) return;
  snprintf(a, sizeof(a), "{\"frameId\":%ld}", D.fr[D.cur].id);
  request("scopes", a, RQ_SCOPES, D.fr[D.cur].id, NULL);
}


static void eval_watches (void) {
  int i;
  wrows_reset();
  for (i = 0; i < g_nwatch; i++) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"expression\":");
    json_put_str(&b, g_watch[i], strlen(g_watch[i]));
    if (D.cur < D.nfr) buf_printf(&b, ",\"frameId\":%ld", D.fr[D.cur].id);
    buf_puts(&b, ",\"context\":\"watch\"}");
    request("evaluate", b.s, RQ_WATCH, i, NULL);
    buf_free(&b);
  }
}


/* the frame looked at: its file, its variables, the watches in it */
static void select_frame (int i) {
  if (i < 0 || i >= D.nfr) return;
  D.cur = i;
  if (D.fr[i].path) on_debug(DE_STOP, D.fr[i].path, D.fr[i].line);
  ask_scopes();
  eval_watches();
}


static void var_insert (VarList *l, int at, const Var *v, int n) {
  l->v = (Var *)xrealloc(l->v, (size_t)(l->n + n + 1) * sizeof(Var));
  memmove(l->v + at + n, l->v + at, (size_t)(l->n - at) * sizeof(Var));
  memcpy(l->v + at, v, (size_t)n * sizeof(Var));
  l->n += n;
}


static void var_expand (VarList *l, int i) {
  char a[64];
  if (i < 0 || i >= l->n || l->v[i].ref <= 0 || l->v[i].open) return;
  l->v[i].open = 1;
  snprintf(a, sizeof(a), "{\"variablesReference\":%ld}", l->v[i].ref);
  request("variables", a, RQ_VARS, l->v[i].ref, NULL);
  D.req[D.nreq - 1].list = l == &g_wvars;
  {
    size_t k;
    for (k = 0; k < g_opened.n; k++)
      if (strcmp(g_opened.v[k], l->v[i].path) == 0) return;
    vec_push(&g_opened, xstrdup(l->v[i].path));
  }
}


static void var_collapse (VarList *l, int i) {
  int j = i + 1;
  size_t k;
  if (i < 0 || i >= l->n || !l->v[i].open) return;
  while (j < l->n && l->v[j].depth > l->v[i].depth) {
    free(l->v[j].name);
    free(l->v[j].value);
    free(l->v[j].path);
    j++;
  }
  memmove(l->v + i + 1, l->v + j, (size_t)(l->n - j) * sizeof(Var));
  l->n -= j - i - 1;
  l->v[i].open = 0;
  for (k = 0; k < g_opened.n; k++)
    if (strcmp(g_opened.v[k], l->v[i].path) == 0) {
      free(g_opened.v[k]);
      g_opened.v[k] = g_opened.v[--g_opened.n];
      break;
    }
}


static int was_open (const char *path) {
  size_t k;
  for (k = 0; k < g_opened.n; k++)
    if (strcmp(g_opened.v[k], path) == 0) return 1;
  return 0;
}


/* the children of reference ref came: under the row that asked */
static void got_vars (VarList *l, long ref, const Json *list, int scopes) {
  int at = -1, depth = 0, i, n = 0;
  Var *v;
  const char *parent = "";
  if (!scopes) {
    for (i = 0; i < l->n; i++)
      if (l->v[i].ref == ref && l->v[i].open && (i + 1 >= l->n || l->v[i + 1].depth <= l->v[i].depth)) {
        at = i;
        break;
      }
    if (at < 0) return;
    depth = l->v[at].depth + 1;
    parent = l->v[at].path;
  }
  if (list == NULL || list->type != J_ARR) return;
  v = (Var *)xmalloc((list->n + 1) * sizeof(Var));
  for (i = 0; i < (int)list->n; i++) {
    const Json *x = list->kid[i];
    const char *name = json_str(json_get(x, "name"), "?");
    Var *y = &v[n++];
    y->name = xstrdup(name);
    y->value = scopes ? NULL : xstrdup(json_str(json_get(x, "value"), ""));
    y->ref = (long)json_num(json_get(x, "variablesReference"), 0);
    y->depth = depth;
    y->open = 0;
    y->path = xstrcat3(parent, "/", name);
  }
  var_insert(l, scopes ? l->n : at + 1, v, n);
  free(v);
  {	/* what was open before is opened again; the first scope always */
    int first = scopes ? l->n - n : at + 1;
    for (i = first + n - 1; i >= first; i--)
      if (l->v[i].ref > 0 && (was_open(l->v[i].path) || (scopes && i == first &&
                                                         !json_bool(json_get(list->kid[0], "expensive"), 0))))
        var_expand(l, i);
  }
}


static void got_stack (const Json *body) {
  const Json *list = json_get(body, "stackFrames");
  int i;
  char *et = D.exc_title, *ed = D.exc_desc;	/* this stop's exception stays */
  D.exc_title = D.exc_desc = NULL;
  clear_stop();
  D.exc_title = et;
  D.exc_desc = ed;
  D.stopped = 1;
  if (list == NULL || list->type != J_ARR) return;
  D.fr = (Frame *)xmalloc((list->n + 1) * sizeof(Frame));
  for (i = 0; i < (int)list->n; i++) {
    const Json *f = list->kid[i];
    const char *p = json_str(json_get(f, "source.path"), NULL);
    Frame *y = &D.fr[D.nfr++];
    y->id = (long)json_num(json_get(f, "id"), 0);
    y->name = xstrdup(json_str(json_get(f, "name"), "?"));
    y->path = p ? native_path(p) : NULL;
    y->line = (size_t)json_num(json_get(f, "line"), 0);
  }
  D.cur = 0;	/* the first frame with a file of its own (the program's, not the runtime's) */
  for (i = 0; i < D.nfr; i++)
    if (D.fr[i].path) {
      OsStat st;
      if (os_stat(D.fr[i].path, &st) == 0 && st.exists) {
        D.cur = i;
        break;
      }
    }
  select_frame(D.cur);
}


static void got_event (const Json *msg) {
  const char *ev = json_str(json_get(msg, "event"), "");
  const Json *body = json_get(msg, "body");
  if (strcmp(ev, "initialized") == 0) {
    send_all_bps();
    send_exc();
    request("configurationDone", "{}", RQ_CONFDONE, 0, NULL);
  }
  else if (strcmp(ev, "stopped") == 0) {
    D.thread = (long)json_num(json_get(body, "threadId"), D.thread);
    snprintf(D.reason, sizeof(D.reason), "%s", json_str(json_get(body, "reason"), "pause"));
    drop_temp(1);
    request("threads", NULL, RQ_THREADS, 0, NULL);
    ask_stack();
    if (strcmp(D.reason, "exception") == 0) {
      const char *t = json_str(json_get(body, "text"), json_str(json_get(body, "description"), "Exception"));
      char a[64];
      con_print(CC_ERR, "%s", t);
      free(D.exc_title);
      free(D.exc_desc);
      D.exc_title = xstrdup(json_str(json_get(body, "text"), "Exception"));	/* the peek, until exceptionInfo says more */
      D.exc_desc = xstrdup(json_str(json_get(body, "description"), ""));
      if (D.cap_excinfo) {
        snprintf(a, sizeof(a), "{\"threadId\":%ld}", D.thread);
        request("exceptionInfo", a, RQ_EXCINFO, 0, NULL);
      }
    }
  }
  else if (strcmp(ev, "continued") == 0) {
    clear_stop();
    on_debug(DE_CONT, NULL, 0);
  }
  else if (strcmp(ev, "output") == 0) {
    const char *cat = json_str(json_get(body, "category"), "console"), *s = json_str(json_get(body, "output"), "");
    if (strcmp(cat, "telemetry") != 0)
      con_add(s, strlen(s), strcmp(cat, "stderr") == 0 ? CC_ERR : strcmp(cat, "console") == 0 ? CC_INFO : CC_OUT);
  }
  else if (strcmp(ev, "breakpoint") == 0) {
    const Json *b = json_get(body, "breakpoint");
    int id = (int)json_num(json_get(b, "id"), -2), i;
    for (i = 0; i < g_nbp; i++)
      if (g_bp[i].id == id) g_bp[i].verified = json_bool(json_get(b, "verified"), 0);
  }
  else if (strcmp(ev, "exited") == 0) {
    char code[32];
    snprintf(code, sizeof(code), "%d", (int)json_num(json_get(body, "exitCode"), 0));
    con_print(CC_INFO, "Process exited with code %s.", code);
  }
  else if (strcmp(ev, "terminated") == 0) {
    if (D.ending == 0) {
      D.ending = os_now_us();
      request("disconnect", "{\"restart\":false,\"terminateDebuggee\":true}", RQ_DISCONNECT, 0, NULL);
    }
  }
}


static void got_response (const Json *msg) {
  int seq = (int)json_num(json_get(msg, "request_seq"), -1), i, ok = json_bool(json_get(msg, "success"), 0);
  const Json *body = json_get(msg, "body");
  Req r;
  for (i = 0; i < D.nreq; i++)
    if (D.req[i].seq == seq) break;
  if (i == D.nreq) return;
  r = D.req[i];
  memmove(D.req + i, D.req + i + 1, (size_t)(D.nreq - i - 1) * sizeof(Req));
  D.nreq--;
  switch (r.kind) {
    case RQ_INIT:
      if (!ok) {
        toast(1, "The debug adapter failed: %s", json_str(json_get(msg, "message"), "initialize"));
        stop_session();
        break;
      }
      got_caps(body);
      request(D.request, D.args, RQ_LAUNCH, 0, NULL);
      break;
    case RQ_LAUNCH:
      if (!ok) {
        const char *m = json_str(json_get(msg, "body.error.format"), json_str(json_get(msg, "message"), "failed"));
        con_print(CC_ERR, "%s", m);
        toast(1, "%s", m);
        end_session();
      }
      break;
    case RQ_SETBP: {
      const Json *list = json_get(body, "breakpoints");
      int k = 0, j;
      for (j = 0; ok && list && j < g_nbp; j++)
        if (g_bp[j].enabled && r.path && same_path(g_bp[j].path, r.path) && k < (int)list->n) {
          const Json *b = list->kid[k++];
          g_bp[j].verified = json_bool(json_get(b, "verified"), 0);
          g_bp[j].id = (int)json_num(json_get(b, "id"), -1);
          if (g_bp[j].verified && json_get(b, "line")) g_bp[j].line = (size_t)json_num(json_get(b, "line"), 1) - 1;
        }
      break;
    }
    case RQ_THREADS: {
      const Json *list = json_get(body, "threads");
      int j;
      clear_threads();
      if (list == NULL || list->type != J_ARR) break;
      D.th = (Thread *)xmalloc((list->n + 1) * sizeof(Thread));
      for (j = 0; j < (int)list->n; j++) {
        D.th[D.nth].id = (long)json_num(json_get(list->kid[j], "id"), 0);
        D.th[D.nth].name = xstrdup(json_str(json_get(list->kid[j], "name"), "thread"));
        D.nth++;
      }
      break;
    }
    case RQ_STACK:
      if (ok) got_stack(body);
      break;
    case RQ_SCOPES:
      if (ok && D.cur < D.nfr && D.fr[D.cur].id == r.arg) got_vars(&g_vars, 0, json_get(body, "scopes"), 1);
      break;
    case RQ_VARS:
      if (ok) got_vars(r.list ? &g_wvars : &g_vars, r.arg, json_get(body, "variables"), 0);
      break;
    case RQ_WATCH: {
      int w = wrow_of((int)r.arg);
      if (w < 0) break;
      free(g_wvars.v[w].value);
      g_wvars.v[w].value = xstrdup(ok ? json_str(json_get(body, "result"), "")
                                      : json_str(json_get(msg, "message"), "not available"));
      g_wvars.v[w].ref = ok ? (long)json_num(json_get(body, "variablesReference"), 0) : 0;
      if (g_wvars.v[w].ref > 0 && was_open(g_wvars.v[w].path)) var_expand(&g_wvars, w);
      break;
    }
    case RQ_SETVAR:
      if (!ok) toast(1, "%s", json_str(json_get(msg, "body.error.format"), json_str(json_get(msg, "message"), "Set Value failed")));
      else if (D.stopped) select_frame(D.cur);	/* the values again */
      break;
    case RQ_GOTOT: {
      const Json *t = json_get(body, "targets");
      char a[96];
      if (!ok || t == NULL || t->type != J_ARR || t->n == 0) {
        toast(0, "There is no place to jump to at that line");
        break;
      }
      snprintf(a, sizeof(a), "{\"threadId\":%ld,\"targetId\":%ld}", D.thread ? D.thread : 1,
               (long)json_num(json_get(t->kid[0], "id"), 0));
      request("goto", a, RQ_GOTO, 0, NULL);
      break;
    }
    case RQ_GOTO:
      if (!ok) toast(1, "%s", json_str(json_get(msg, "message"), "Jump to Cursor failed"));
      break;
    case RQ_EXCINFO:
      if (ok && D.stopped) {
        const char *id = json_str(json_get(body, "exceptionId"), NULL), *d = json_str(json_get(body, "description"), NULL);
        if (id) {
          free(D.exc_title);
          D.exc_title = xstrdup(id);
        }
        if (d) {
          free(D.exc_desc);
          D.exc_desc = xstrdup(d);
        }
      }
      break;
    case RQ_REPL:
      if (ok) con_print(CC_RESULT, "%s", json_str(json_get(body, "result"), ""));
      else con_print(CC_ERR, "%s", json_str(json_get(msg, "body.error.format"), json_str(json_get(msg, "message"), "error")));
      break;
    case RQ_HOVER:
      if (ok && D.stopped) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "```\n");
        buf_puts(&b, json_str(json_get(body, "result"), ""));
        buf_puts(&b, "\n```");
        buf_putc(&b, '\0');
        on_hover(b.s);
        buf_free(&b);
      }
      break;
    case RQ_STEP:
      if (!ok) toast(1, "%s", json_str(json_get(msg, "message"), "failed"));
      break;
    case RQ_DISCONNECT:
      free(r.path);
      end_session();
      return;
  }
  free(r.path);
}


static void got_message (const Json *msg) {
  const char *type = json_str(json_get(msg, "type"), "");
  if (strcmp(type, "event") == 0) got_event(msg);
  else if (strcmp(type, "response") == 0) got_response(msg);
  else if (strcmp(type, "request") == 0) refuse(msg);
}


/* the whole messages that came in D.in */
static int messages (void) {
  int got = 0;
  for (;;) {
    char *hdr_end, *cl;
    size_t body, hlen;
    Json *j;
    if (!D.on || D.in.len == 0) break;
    D.in.s[D.in.len] = '\0';
    hdr_end = strstr(D.in.s, "\r\n\r\n");
    if (hdr_end == NULL) break;
    hlen = (size_t)(hdr_end - D.in.s) + 4;
    cl = strstr(D.in.s, "Content-Length:");
    if (cl == NULL || cl > hdr_end) {
      memmove(D.in.s, D.in.s + hlen, D.in.len - hlen);
      D.in.len -= hlen;
      continue;
    }
    body = (size_t)strtoul(cl + 15, NULL, 10);
    if (D.in.len < hlen + body) break;
    trace("<< ", D.in.s + hlen, body);
    j = json_parse(D.in.s + hlen, body);
    memmove(D.in.s, D.in.s + hlen + body, D.in.len - hlen - body);
    D.in.len -= hlen + body;
    if (j) {
      got_message(j);	/* it may end the session */
      json_free(j);
      got = 1;
    }
  }
  return got;
}


/* the adapter's own output (TCP): "DAP server listening at: 127.0.0.1:port", then its logs */
static void adapter_output (const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] != '\n') {
      if (s[i] != '\r') buf_putc(&D.outline, s[i]);
      continue;
    }
    buf_putc(&D.outline, '\0');
    D.outline.len--;
    if (D.sock == NO_SOCK && strstr(D.outline.s, "listening at")) {
      const char *c = strrchr(D.outline.s, ':');
      int port = c ? atoi(c + 1) : 0;
      if (port > 0 && (D.sock = tcp_connect(port)) != NO_SOCK) {
        D.ready = 1;
        request("initialize", "{\"clientID\":\"mme\",\"clientName\":\"mme\",\"adapterID\":\"go\",\"pathFormat\":\"path\","
                              "\"linesStartAt1\":true,\"columnsStartAt1\":true,\"supportsVariableType\":true,"
                              "\"supportsRunInTerminalRequest\":false,\"locale\":\"en\"}", RQ_INIT, 0, NULL);
      }
    }
    else if (D.outline.len > 0) {
      con_add(D.outline.s, D.outline.len, CC_OUT);
      con_add("\n", 1, CC_OUT);
    }
    D.outline.len = 0;
  }
}


int dbg_poll (void) {
  char chunk[65536];
  int got = 0, status;
  long n;
  if (g_bp_dirty) bp_save();	/* the breakpoints are kept for the next session */
  if (!D.on) return 0;
  if (D.out >= 0)
    while (D.on && os_wait_readable(D.out, 0) == 1) {
      n = os_read(D.out, chunk, sizeof(chunk));
      if (n <= 0) {
        os_close(D.out);
        D.out = -1;
        break;
      }
      adapter_output(chunk, (size_t)n);
      got = 1;
    }
  if (D.sock != NO_SOCK)
    while (D.on && sock_ready(D.sock)) {
      n = (long)recv(D.sock, chunk, (int)sizeof(chunk), 0);
      if (n <= 0) {
        adapter_gone();
        return 1;
      }
      buf_putn(&D.in, chunk, (size_t)n);
      buf_putc(&D.in, '\0');
      D.in.len--;
    }
  if (D.from >= 0)
    while (D.on && os_wait_readable(D.from, 0) == 1) {
      n = os_read(D.from, chunk, sizeof(chunk));
      if (n <= 0) {
        adapter_gone();
        return 1;
      }
      buf_putn(&D.in, chunk, (size_t)n);
      buf_putc(&D.in, '\0');
      D.in.len--;
    }
  got |= messages();
  if (!D.on) return 1;
  if (!D.ready && D.waited && os_now_us() - D.waited > 15000000) {
    toast(1, "The debug adapter did not say where to connect");
    end_session();
    return 1;
  }
  if (D.ending && os_now_us() - D.ending > 2000000) {	/* no answer to disconnect */
    end_session();
    return 1;
  }
  if (os_poll_proc(D.proc, &status) == 1 && D.in.len == 0) {	/* it ended by itself */
    if (D.out >= 0) {	/* its last words */
      while (os_wait_readable(D.out, 0) == 1 && (n = os_read(D.out, chunk, sizeof(chunk))) > 0)
        adapter_output(chunk, (size_t)n);
    }
    adapter_gone();
    return 1;
  }
  return got;
}


int dbg_active (void) {
  return D.on;
}


int dbg_stopped (void) {
  return D.on && D.stopped;
}


void dbg_shutdown (void) {
  if (D.on && D.ready) request("disconnect", "{\"terminateDebuggee\":true}", RQ_OTHER, 0, NULL);
  end_session();
  if (g_bp_dirty) bp_save();
}


/* Run to Cursor: a breakpoint there for once, and on */
void dbg_run_to (const char *path, size_t line) {
  int i;
  if (path == NULL) return;
  bp_sync();
  if (!D.on || !D.stopped) {
    toast(0, "Run to Cursor works while the program is paused");
    return;
  }
  i = bp_add(path, line);
  g_bp[i].temp = 1;
  send_bps(path);
  step("continue");
}


/* Jump to Cursor: the next line to run is this one (gotoTargets, goto) */
void dbg_jump_to (const char *path, size_t line) {
  Buf b;
  if (path == NULL || !D.on || !D.stopped) return;
  if (!D.cap_goto) {
    toast(0, "The debug adapter does not support Jump to Cursor");
    return;
  }
  buf_init(&b);
  buf_puts(&b, "{\"source\":{\"path\":");
  json_put_str(&b, path, strlen(path));
  buf_printf(&b, "},\"line\":%lu}", (unsigned long)(line + 1));
  request("gotoTargets", b.s, RQ_GOTOT, 0, NULL);
  buf_free(&b);
}


/* Debug: Add to Watch */
void dbg_add_watch (const char *expr) {
  if (expr == NULL || *expr == '\0') return;
  bp_sync();
  g_watch = (char **)xrealloc(g_watch, (size_t)(g_nwatch + 1) * sizeof(char *));
  g_watch[g_nwatch++] = xstrdup(expr);
  g_bp_dirty = 1;
  if (D.stopped) eval_watches();
  else wrows_reset();
}


static int is_idc (int c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}


/*
** debug.inlineValues: the values of the variables line (of path) names,
** "a = 1, b = 2", while paused in that file at or below it; 0: none
*/
int dbg_inline (const char *path, size_t line, const char *s, size_t n, char *out, size_t cap) {
  const Frame *f;
  size_t len = 0;
  int i, end;
  out[0] = '\0';
  if (!D.stopped || D.cur >= D.nfr || path == NULL) return 0;
  f = &D.fr[D.cur];
  if (f->path == NULL || f->line == 0 || line + 1 > f->line || line + 200 < f->line || !same_path(f->path, path)) return 0;
  for (end = 1; end < g_vars.n && g_vars.v[end].depth > 0; end++) ;	/* the first scope's values */
  for (i = 1; i < end; i++) {
    const Var *v = &g_vars.v[i];
    size_t nl = strlen(v->name), k;
    int found = 0;
    if (v->depth != 1 || v->value == NULL || nl == 0 || strchr(v->name, ' ')) continue;
    for (k = 0; k + nl <= n && !found; k++)	/* the name as a word of the line */
      if (memcmp(s + k, v->name, nl) == 0 && (k == 0 || !is_idc((unsigned char)s[k - 1])) &&
          (k + nl == n || !is_idc((unsigned char)s[k + nl])))
        found = 1;
    if (!found) continue;
    len += (size_t)snprintf(out + len, len < cap ? cap - len : 0, "%s%s = %.40s", len ? ", " : "", v->name, v->value);
    if (len >= cap) {
      out[cap - 1] = '\0';
      return (int)(cap - 1);
    }
  }
  return (int)len;
}


/* stopped on an exception at path:line (the frame in front): what the peek says */
int dbg_exception (const char *path, size_t line, const char **title, const char **desc) {
  if (!D.stopped || D.exc_title == NULL || D.cur >= D.nfr || D.cur != 0) return 0;
  if (D.fr[0].path == NULL || D.fr[0].line != line + 1 || path == NULL || !same_path(D.fr[0].path, path)) return 0;
  *title = D.exc_title;
  *desc = D.exc_desc ? D.exc_desc : "";
  return 1;
}


/* the value of expr under the mouse, while stopped; it comes to on_hover */
void dbg_hover (const char *expr) {
  Buf b;
  if (!dbg_stopped()) return;
  buf_init(&b);
  buf_puts(&b, "{\"expression\":");
  json_put_str(&b, expr, strlen(expr));
  if (D.cur < D.nfr) buf_printf(&b, ",\"frameId\":%ld", D.fr[D.cur].id);
  buf_puts(&b, ",\"context\":\"hover\"}");
  request("evaluate", b.s, RQ_HOVER, 0, NULL);
  buf_free(&b);
}


static void step (const char *command) {
  char a[64];
  if (!D.on || !D.ready) return;
  snprintf(a, sizeof(a), "{\"threadId\":%ld}", D.thread ? D.thread : 1);
  request(command, a, RQ_STEP, 0, NULL);
  if (strcmp(command, "pause") != 0) {	/* it runs: nothing to show until it stops */
    clear_stop();
    on_debug(DE_CONT, NULL, 0);
  }
}


/* Start Debugging... the configurations, and Add Configuration */
static void choose_config (void) {
  Pick p;
  int n, i, r;
  load_launch();
  configs(&n);
  pick_init(&p, "Select a launch configuration");
  for (i = 0; i < n; i++) pick_add(&p, config_name(i), i == g_cfg ? "current" : NULL, 0xEB91);
  pick_add(&p, "Add Configuration...", NULL, 0xEA60);
  p.keep_order = 1;
  p.start = g_cfg < n ? g_cfg : 0;
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) return;
  if (r == n) create_launch();
  else g_cfg = r;
}


void dbg_command (int cmd) {
  switch (cmd) {
    case CMD_DEBUG_START:
      if (D.on && D.stopped) step("continue");
      else if (!D.on) dbg_start(0);
      break;
    case CMD_DEBUG_RUN:
      if (!D.on) dbg_start(1);
      break;
    case CMD_DEBUG_STOP:
      D.restart = 0;
      stop_session();
      break;
    case CMD_DEBUG_RESTART:
      if (D.on) {
        D.restart = 1;
        stop_session();
      }
      else dbg_start(0);
      break;
    case CMD_DEBUG_STEP_OVER: if (D.stopped) step("next"); break;
    case CMD_DEBUG_STEP_INTO: if (D.stopped) step("stepIn"); break;
    case CMD_DEBUG_STEP_OUT: if (D.stopped) step("stepOut"); break;
    case CMD_DEBUG_PAUSE: if (D.on && !D.stopped) step("pause"); break;
    case CMD_DEBUG_CONFIG: create_launch(); break;
    case CMD_DEBUG_SELECT: choose_config(); break;
    case CMD_BP_REMOVE_ALL:
      bp_sync();
      while (g_nbp > 0) bp_remove(g_nbp - 1);
      break;
    case CMD_BP_ENABLE_ALL: case CMD_BP_DISABLE_ALL: {
      int i, j;
      bp_sync();
      for (i = 0; i < g_nbp; i++) g_bp[i].enabled = cmd == CMD_BP_ENABLE_ALL;
      g_bp_dirty = 1;
      for (i = 0; i < g_nbp; i++) {	/* each file once */
        for (j = 0; j < i; j++)
          if (same_path(g_bp[j].path, g_bp[i].path)) break;
        if (j == i) send_bps(g_bp[i].path);
      }
      break;
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** The Run and Debug view
** ===================================================================
*/

enum { SEC_VARS, SEC_WATCH, SEC_STACK, SEC_BPS, SEC_N };
enum { R_HEAD, R_VAR, R_WATCH, R_WADD, R_THREAD, R_FRAME, R_BP, R_NOTE, R_EXC };

typedef struct VRow {
  int kind, i;
} VRow;

static const char *const sec_name[SEC_N] = {"VARIABLES", "WATCH", "CALL STACK", "BREAKPOINTS"};
static int g_closed[SEC_N];
static VRow *g_row;
static int g_nrow, g_caprow, g_sel = -1, g_top, g_h;

#define VHEAD	3	/* the title, the start button, a line */


static void add_row (int kind, int i) {
  if (g_nrow == g_caprow) {
    g_caprow = g_caprow ? g_caprow * 2 : 64;
    g_row = (VRow *)xrealloc(g_row, (size_t)g_caprow * sizeof(VRow));
  }
  g_row[g_nrow].kind = kind;
  g_row[g_nrow].i = i;
  g_nrow++;
}


static void build_rows (void) {
  int s, i;
  g_nrow = 0;
  for (s = 0; s < SEC_N; s++) {
    add_row(R_HEAD, s);
    if (g_closed[s]) continue;
    switch (s) {
      case SEC_VARS:
        for (i = 0; i < g_vars.n; i++) add_row(R_VAR, i);
        break;
      case SEC_WATCH:
        for (i = 0; i < g_wvars.n; i++) add_row(R_WATCH, i);
        add_row(R_WADD, 0);
        break;
      case SEC_STACK:
        if (!D.on) break;
        if (D.nth == 0) add_row(R_NOTE, D.stopped ? 1 : 2);
        for (i = 0; i < D.nth; i++) {
          int j;
          add_row(R_THREAD, i);
          if (D.stopped && D.th[i].id == D.thread)
            for (j = 0; j < D.nfr; j++) add_row(R_FRAME, j);
        }
        break;
      case SEC_BPS:
        for (i = 0; i < g_nexf; i++) add_row(R_EXC, i);
        for (i = 0; i < g_nbp; i++)
          if (!g_bp[i].temp) add_row(R_BP, i);
        break;
    }
  }
  if (g_sel >= g_nrow) g_sel = g_nrow - 1;
}


void debug_draw (int x, int y, int w, int h, int focus) {
  int row, n;
  char t[256];
  scr_box(x, y, w, h, S_SIDE);
  if (h <= VHEAD) return;
  bp_sync();
  scr_puts(x + 2, y, "RUN AND DEBUG", S_SIDE_HEAD);
  load_launch();
  configs(&n);
  {	/* the start button and the configuration, like VS Code's */
    const char *nm = D.on ? D.name : (n ? config_name(g_cfg < n ? g_cfg : 0) : NULL);
    scr_put_rgb(x + 2, y + 1, D.on ? 0xEACF : 0xEB2C, 0x89D185, ui_color(C_SIDE_BG), 0);	/* play, green */
    if (nm) {
      snprintf(t, sizeof(t), "%s", nm);
      scr_putsw(x + 4, y + 1, w - 9, t, S_SIDE);
      scr_put(x + 5 + (int)(str_cols(t) < (size_t)(w - 9) ? str_cols(t) : (size_t)(w - 9)), y + 1, 0xEAB4, S_SIDE_DIM);
    }
    else scr_putsw(x + 4, y + 1, w - 9, "Run and Debug (F5)", S_SIDE);
    scr_put(x + w - 3, y + 1, 0xEAF8, S_SIDE_DIM);	/* gear: launch.json */
  }
  if (n == 0 && !D.on) scr_putsw(x + 2, y + 2, w - 3, "create a launch.json file", S_SIDE_DIM);
  build_rows();
  g_h = h - VHEAD;
  if (g_sel >= 0) {
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + g_h) g_top = g_sel - g_h + 1;
  }
  if (g_top > g_nrow - 1) g_top = g_nrow > 0 ? g_nrow - 1 : 0;
  for (row = 0; row < g_h; row++) {
    int k = g_top + row, sy = y + VHEAD + row, st, cx = x + 1;
    const VRow *r;
    if (k >= g_nrow) break;
    r = &g_row[k];
    st = (k == g_sel && focus) ? S_SIDE_SEL : (k == g_sel ? S_SIDE_CUR : S_SIDE);
    scr_fill(x, sy, w, st);
    switch (r->kind) {
      case R_HEAD:
        scr_put(cx, sy, g_closed[r->i] ? 0xEAB6 : 0xEAB4, st);
        scr_puts(cx + 2, sy, sec_name[r->i], st == S_SIDE ? S_SIDE_TITLE : st);
        if (r->i == SEC_WATCH) scr_put(x + w - 3, sy, 0xEA60, st == S_SIDE ? S_SIDE_DIM : st);	/* add */
        break;
      case R_VAR:
      case R_WATCH: {
        const Var *v = r->kind == R_VAR ? &g_vars.v[r->i] : &g_wvars.v[r->i];
        if (r->kind == R_WATCH && v->depth == 0 && v->value == NULL) {	/* not known: "not available" */
          cx += 3;
          cx += scr_putsw(cx, sy, x + w - cx, v->name, st == S_SIDE ? S_ICON_PURPLE : st);
          if (cx < x + w - 2) {
            cx += scr_puts(cx, sy, ": ", st);
            scr_putsw(cx, sy, x + w - cx, "not available", st == S_SIDE ? S_SIDE_DIM : st);
          }
          break;
        }
        cx += 1 + v->depth * 2;
        if (v->ref > 0) scr_put(cx, sy, v->open ? 0xEAB4 : 0xEAB6, st);
        cx += 2;
        if (v->value == NULL || v->value[0] == '\0') {	/* a scope, a group ("special variables") */
          scr_putsw(cx, sy, x + w - cx, v->name, st);
          break;
        }
        cx += scr_putsw(cx, sy, x + w - cx, v->name, st == S_SIDE ? S_ICON_PURPLE : st);
        if (cx < x + w - 2) {
          cx += scr_puts(cx, sy, ": ", st);
          scr_putsw(cx, sy, x + w - cx, v->value, st);
        }
        break;
      }
      case R_WADD:
        scr_put(cx + 3, sy, 0xEA60, st == S_SIDE ? S_SIDE_DIM : st);
        scr_putsw(cx + 5, sy, x + w - cx - 5, "Add Expression", st == S_SIDE ? S_SIDE_DIM : st);
        break;
      case R_THREAD: {
        const char *state = (D.stopped && D.th[r->i].id == D.thread) ? "PAUSED" : "RUNNING";
        char right[96];
        if (D.stopped && D.th[r->i].id == D.thread && D.reason[0] &&
            (int)(str_cols(D.th[r->i].name) + strlen(D.reason) + 17) <= w) {	/* "PAUSED ON BREAKPOINT", when there is room */
          size_t i;
          snprintf(right, sizeof(right), "PAUSED ON %s", D.reason);
          for (i = 10; right[i]; i++)
            if (right[i] >= 'a' && right[i] <= 'z') right[i] = (char)(right[i] - 32);
          state = right;
        }
        scr_put(cx + 1, sy, 0xEAB4, st);
        scr_putsw(cx + 3, sy, w - (int)str_cols(state) - 6, D.th[r->i].name, st);
        scr_puts(x + w - (int)str_cols(state) - 1, sy, state, st == S_SIDE ? S_SIDE_DIM : st);
        break;
      }
      case R_FRAME: {
        const Frame *f = &D.fr[r->i];
        char loc[160];
        int lw;
        if (r->i == D.cur) scr_put_rgb(cx + 2, sy, 0xEB8B, 0xFFCC00, ui_color(C_SIDE_BG), 0);	/* stackframe */
        if (f->path) snprintf(loc, sizeof(loc), "%s  %lu", path_basename(f->path), (unsigned long)f->line);
        else snprintf(loc, sizeof(loc), "Unknown Source");
        lw = (int)str_cols(loc);
        if (lw > w / 2) lw = w / 2;
        scr_putsw(cx + 4, sy, w - lw - 7, f->name, st == S_SIDE && !f->path ? S_SIDE_DIM : st);
        scr_putsw(x + w - lw - 1, sy, lw, loc, st == S_SIDE ? S_SIDE_DIM : st);
        break;
      }
      case R_BP: {
        const Bp *b = &g_bp[r->i];
        char ln[32], nm[300];
        int ist;
        uint32_t icon = file_icon(path_basename(b->path), &ist), dot = b->log ? 0xEAAB : (b->cond || b->hit) ? 0xEAA7 : 0xEA71;
        scr_put(cx + 1, sy, b->enabled ? 0xF046 : 0xF096, st);	/* check-square-o, square-o */
        if (!b->enabled) scr_put_rgb(cx + 3, sy, dot == 0xEA71 ? 0xEABC : dot, 0x848484, ui_color(C_SIDE_BG), 0);
        else scr_put_rgb(cx + 3, sy, (D.on && D.ready && !D.nodebug && !b->verified) ? (dot == 0xEA71 ? 0xEABC : dot - 1) : dot,
                         0xE51400, ui_color(C_SIDE_BG), 0);
        scr_put(cx + 5, sy, icon, st == S_SIDE ? ist : st);
        snprintf(ln, sizeof(ln), "%lu", (unsigned long)(b->line + 1));
        if (b->cond || b->hit || b->log)	/* "a.py  x > 3", like VS Code's list says it */
          snprintf(nm, sizeof(nm), "%s  %s", path_basename(b->path), b->log ? b->log : b->cond ? b->cond : b->hit);
        else snprintf(nm, sizeof(nm), "%s", path_basename(b->path));
        scr_putsw(cx + 7, sy, w - 12 - (int)strlen(ln), nm, st);
        scr_puts(x + w - (int)strlen(ln) - 1, sy, ln, st == S_SIDE ? S_SIDE_DIM : st);
        break;
      }
      case R_EXC: {
        const ExcFilter *e = &g_exf[r->i];
        scr_put(cx + 1, sy, e->enabled ? 0xF046 : 0xF096, st);
        scr_putsw(cx + 3, sy, w - 5, e->label, st);
        break;
      }
      case R_NOTE:
        scr_putsw(cx + 3, sy, w - 5, r->i == 1 ? "Paused" : "Running", st == S_SIDE ? S_SIDE_DIM : st);
        break;
    }
  }
}


static void add_watch (void) {
  char *e = ask_text("Expression to watch", "");
  if (e != NULL && *e) dbg_add_watch(e);
  free(e);
}


/* the watch a WATCH row is of (its depth-0 row before it) */
static int watch_of_row (int row) {
  int i, w = -1;
  for (i = 0; i <= row && i < g_wvars.n; i++)
    if (g_wvars.v[i].depth == 0) w++;
  return w;
}


static void del_watch (int w) {
  if (w < 0 || w >= g_nwatch) return;
  free(g_watch[w]);
  memmove(g_watch + w, g_watch + w + 1, (size_t)(g_nwatch - w - 1) * sizeof(char *));
  g_nwatch--;
  g_bp_dirty = 1;
  if (D.stopped) eval_watches();
  else wrows_reset();
}


/* F2 on a watch: its expression changed */
static void edit_watch (int w) {
  char *e;
  if (w < 0 || w >= g_nwatch) return;
  e = ask_text("Edit Expression", g_watch[w]);
  if (e == NULL) return;
  if (*e == '\0') {
    free(e);
    del_watch(w);
    return;
  }
  free(g_watch[w]);
  g_watch[w] = e;
  g_bp_dirty = 1;
  if (D.stopped) eval_watches();
  else wrows_reset();
}


/* F2 on a variable: Set Value (setVariable, in its parent's reference) */
static void set_value (VarList *l, int i) {
  int p;
  char *v;
  Buf b;
  if (i < 0 || i >= l->n || l->v[i].depth == 0 || l->v[i].value == NULL || !D.stopped) return;
  if (!D.cap_setvar) {
    toast(0, "The debug adapter does not support setting values");
    return;
  }
  for (p = i - 1; p >= 0 && l->v[p].depth >= l->v[i].depth; p--) ;
  if (p < 0 || l->v[p].ref <= 0) return;
  v = ask_text("Set Value", l->v[i].value);
  if (v == NULL) return;
  buf_init(&b);
  buf_printf(&b, "{\"variablesReference\":%ld,\"name\":", l->v[p].ref);
  json_put_str(&b, l->v[i].name, strlen(l->v[i].name));
  buf_puts(&b, ",\"value\":");
  json_put_str(&b, v, strlen(v));
  buf_putc(&b, '}');
  request("setVariable", b.s, RQ_SETVAR, 0, NULL);
  buf_free(&b);
  free(v);
}


/* an exception filter on or off */
static void exc_toggle (int i) {
  if (i < 0 || i >= g_nexf) return;
  g_exf[i].enabled = !g_exf[i].enabled;
  g_bp_dirty = 1;
  send_exc();
}


/* Enter or a click on row k */
static void row_act (int k, SideAct *act) {
  const VRow *r;
  if (k < 0 || k >= g_nrow) return;
  r = &g_row[k];
  switch (r->kind) {
    case R_HEAD: g_closed[r->i] = !g_closed[r->i]; break;
    case R_VAR:
      if (g_vars.v[r->i].open) var_collapse(&g_vars, r->i);
      else var_expand(&g_vars, r->i);
      break;
    case R_WATCH:
      if (g_wvars.v[r->i].open) var_collapse(&g_wvars, r->i);
      else var_expand(&g_wvars, r->i);
      break;
    case R_EXC: exc_toggle(r->i); break;
    case R_WADD: add_watch(); break;
    case R_FRAME:
      if (D.fr[r->i].path) {
        D.cur = r->i;
        ask_scopes();
        eval_watches();
        act->what = SA_OPEN;
        act->path = D.fr[r->i].path;
        act->line = D.fr[r->i].line;
      }
      break;
    case R_BP:
      act->what = SA_OPEN;
      act->path = g_bp[r->i].path;
      act->line = g_bp[r->i].line + 1;
      break;
  }
}


int debug_key (int k, SideAct *act) {
  int code = KEY_CODE(k);
  const VRow *r;
  build_rows();
  act->what = SA_NONE;
  r = (g_sel >= 0 && g_sel < g_nrow) ? &g_row[g_sel] : NULL;
  switch (code) {
    case K_UP: if (g_sel > 0) g_sel--; else g_sel = 0; return 1;
    case K_DOWN: if (g_sel + 1 < g_nrow) g_sel++; return 1;
    case K_HOME: g_sel = 0; return 1;
    case K_END: g_sel = g_nrow - 1; return 1;
    case K_PGUP: g_sel = g_sel > g_h ? g_sel - g_h : 0; return 1;
    case K_PGDN: g_sel = g_sel + g_h < g_nrow ? g_sel + g_h : g_nrow - 1; return 1;
    case K_ENTER: row_act(g_sel, act); return 1;
    case K_RIGHT:
      if (r && r->kind == R_VAR) var_expand(&g_vars, r->i);
      else if (r && r->kind == R_WATCH) var_expand(&g_wvars, r->i);
      else if (r && r->kind == R_HEAD) g_closed[r->i] = 0;
      return 1;
    case K_LEFT:
      if (r && (r->kind == R_VAR || r->kind == R_WATCH)) {
        VarList *l = r->kind == R_VAR ? &g_vars : &g_wvars;
        if (l->v[r->i].open) var_collapse(l, r->i);
        else {	/* to its parent */
          int j;
          for (j = r->i - 1; j >= 0 && l->v[j].depth >= l->v[r->i].depth; j--) ;
          if (j >= 0) {
            int q;
            for (q = 0; q < g_nrow; q++)
              if (g_row[q].kind == r->kind && g_row[q].i == j) g_sel = q;
          }
        }
      }
      else if (r && r->kind == R_HEAD) g_closed[r->i] = 1;
      return 1;
    case K_DEL: case K_BS:
      if (r && r->kind == R_WATCH) del_watch(watch_of_row(r->i));
      else if (r && r->kind == R_BP) bp_remove(r->i);
      return 1;
    case K_F2:	/* Edit Breakpoint, Edit Expression, Set Value */
      if (r && r->kind == R_BP) dbg_edit_bp(g_bp[r->i].path, g_bp[r->i].line, -1);
      else if (r && r->kind == R_WATCH && g_wvars.v[r->i].depth == 0) edit_watch(watch_of_row(r->i));
      else if (r && r->kind == R_WATCH) set_value(&g_wvars, r->i);
      else if (r && r->kind == R_VAR) set_value(&g_vars, r->i);
      return 1;
    case ' ':
      if (r && r->kind == R_BP) bp_enable(r->i);
      else row_act(g_sel, act);
      return 1;
  }
  if (k == CTRL('c') && r && (r->kind == R_VAR || r->kind == R_WATCH)) {	/* Copy Value */
    const Var *v = r->kind == R_VAR ? &g_vars.v[r->i] : &g_wvars.v[r->i];
    if (v->value) {
      act->what = SA_CLIP;
      act->path = v->value;
    }
    return 1;
  }
  return 0;
}


void debug_click (int row, int col, SideAct *act) {
  int w = side_width(), k;
  act->what = SA_NONE;
  if (row == 1) {	/* the start button, the configuration, the gear */
    if (col <= 3) dbg_command(D.on ? CMD_DEBUG_START : CMD_DEBUG_START);
    else if (col >= w - 4) create_launch();
    else choose_config();
    return;
  }
  if (row == 2) {
    int n;
    configs(&n);
    if (n == 0 && !D.on) create_launch();
    return;
  }
  if (row < VHEAD) return;
  build_rows();
  k = g_top + row - VHEAD;
  if (k >= g_nrow) return;
  g_sel = k;
  if (g_row[k].kind == R_HEAD && g_row[k].i == SEC_WATCH && col >= w - 4) {
    add_watch();
    return;
  }
  if (g_row[k].kind == R_BP && col <= 3) {
    bp_enable(g_row[k].i);
    return;
  }
  if (g_row[k].kind == R_EXC) {
    exc_toggle(g_row[k].i);
    return;
  }
  row_act(k, act);
}


void debug_wheel (int d) {
  g_top += d * 3;
  if (g_top < 0) g_top = 0;
  if (g_top > g_nrow - 1) g_top = g_nrow > 0 ? g_nrow - 1 : 0;
}

/* }================================================================== */


/*
** {==================================================================
** The DEBUG CONSOLE, in the panel
** ===================================================================
*/

void console_draw (int x, int y, int w, int h, int focus) {
  int rows = h - 1, i, first;
  scr_box(x, y, w, h, S_PANEL);
  CN.h = rows;
  if (CN.top > g_ncon - rows) CN.top = g_ncon - rows > 0 ? g_ncon - rows : 0;
  first = g_ncon - rows - CN.top;
  if (first < 0) first = 0;
  for (i = 0; i < rows && first + i < g_ncon; i++) {
    const CLine *c = &g_con[first + i];
    uint32_t fg = c->cat == CC_ERR ? 0xF48771 : c->cat == CC_INFO ? 0x3794FF : c->cat == CC_IN ? 0x9D9D9D : ui_color(C_TERM_FG);
    const char *p = c->s;
    int cx = x + 1;
    while (*p && cx < x + w - 1) {	/* each character in its color */
      size_t len;
      uint32_t cp = utf8_decode(p, strlen(p), &len);
      if (cp == '\t') cp = ' ';
      if (cp < 32) cp = '?';
      cx += scr_put_rgb(cx, y + i, cp, fg, ui_color(C_TERM_BG), 0);
      p += len;
    }
  }
  {	/* the REPL's input, at the bottom */
    int iy = y + h - 1, cx;
    scr_put(x + 1, iy, 0xEAB6, S_PANEL);
    if (CN.in[0]) cx = x + 3 + scr_putsw(x + 3, iy, w - 5, CN.in, S_PANEL);
    else {
      scr_putsw(x + 3, iy, w - 5, D.on ? "Evaluate expression" : "Please start a debug session to evaluate expressions",
                S_INPUT_HINT);
      cx = x + 3;
    }
    if (focus) scr_cursor(cx, iy);
  }
}


static void repl (void) {
  Buf b;
  if (CN.in[0] == '\0') return;
  con_print(CC_IN, "> %s", CN.in);
  if (CN.nhist == 32) {
    free(CN.hist[0]);
    memmove(CN.hist, CN.hist + 1, 31 * sizeof(char *));
    CN.nhist--;
  }
  CN.hist[CN.nhist++] = xstrdup(CN.in);
  CN.at = CN.nhist;
  if (!D.on || !D.ready) con_print(CC_ERR, "%s", "No debug session");
  else {
    buf_init(&b);
    buf_puts(&b, "{\"expression\":");
    json_put_str(&b, CN.in, strlen(CN.in));
    if (D.stopped && D.cur < D.nfr) buf_printf(&b, ",\"frameId\":%ld", D.fr[D.cur].id);
    buf_puts(&b, ",\"context\":\"repl\"}");
    request("evaluate", b.s, RQ_REPL, 0, NULL);
    buf_free(&b);
  }
  CN.in[0] = '\0';
  CN.top = 0;
}


int console_key (int k) {
  int code = KEY_CODE(k);
  size_t len = strlen(CN.in);
  if (code == K_ENTER) repl();
  else if (code == K_BS) {
    while (len > 0 && ((unsigned char)CN.in[len - 1] & 0xC0) == 0x80) len--;
    if (len > 0) CN.in[len - 1] = '\0';
  }
  else if (code == K_UP || code == K_DOWN) {	/* the history */
    CN.at += code == K_UP ? -1 : 1;
    if (CN.at < 0) CN.at = 0;
    if (CN.at >= CN.nhist) {
      CN.at = CN.nhist;
      CN.in[0] = '\0';
    }
    else snprintf(CN.in, sizeof(CN.in), "%s", CN.hist[CN.at]);
  }
  else if (code == K_PGUP) CN.top += CN.h > 1 ? CN.h - 1 : 1;
  else if (code == K_PGDN) CN.top = CN.top > CN.h ? CN.top - CN.h + 1 : 0;
  else if (k == CTRL('l')) {	/* Clear Console */
    int i;
    for (i = 0; i < g_ncon; i++) free(g_con[i].s);
    g_ncon = 0;
  }
  else if (IS_TEXT(k) && len + 4 < sizeof(CN.in)) {
    len += (size_t)utf8_encode((uint32_t)k, CN.in + len);
    CN.in[len] = '\0';
  }
  else return 0;
  return 1;
}


void console_paste (const char *s, size_t n) {
  size_t len = strlen(CN.in), i;
  for (i = 0; i < n && s[i] != '\n' && len + 1 < sizeof(CN.in); i++) CN.in[len++] = s[i];
  CN.in[len] = '\0';
}


void console_wheel (int d) {
  CN.top -= d * 3;
  if (CN.top < 0) CN.top = 0;
}

/* }================================================================== */


/*
** {==================================================================
** The debug toolbar, over the editor while debugging
** ===================================================================
*/

static struct {
  int x, y, n;
  int cmd[8];
} TB;


void dbg_toolbar_draw (int x, int w, int y) {
  static const uint32_t icon_run[] = {0xEAD1, 0xEAD6, 0xEAD4, 0xEAD5, 0xEAD2, 0xEAD7};	/* pause ... */
  static const int cmd_run[] = {CMD_DEBUG_PAUSE, CMD_DEBUG_STEP_OVER, CMD_DEBUG_STEP_INTO, CMD_DEBUG_STEP_OUT,
                                CMD_DEBUG_RESTART, CMD_DEBUG_STOP};
  static const uint32_t color[] = {0x75BEFF, 0x75BEFF, 0x75BEFF, 0x75BEFF, 0x89D185, 0xF48771};
  uint32_t bg = 0x333333;
  int i, bx, bw = 6 * 3 + 3;
  TB.n = 0;
  if (!D.on || w < bw + 2) return;
  bx = x + (w - bw) / 2;
  TB.x = bx;
  TB.y = y;
  scr_put_rgb(bx, y, 0xEB04, 0x808080, bg, 0);	/* the gripper */
  scr_put_rgb(bx + 1, y, ' ', 0x808080, bg, 0);
  for (i = 0; i < 6; i++) {
    uint32_t ic = icon_run[i];
    int cx = bx + 2 + i * 3, dim;
    if (i == 0 && D.stopped) ic = 0xEACF;	/* continue */
    dim = i >= 1 && i <= 3 && !D.stopped;
    scr_put_rgb(cx, y, ' ', 0, bg, 0);
    scr_put_rgb(cx + 1, y, ic, dim ? 0x5A5A5A : color[i], bg, 0);
    scr_put_rgb(cx + 2, y, ' ', 0, bg, 0);
    TB.cmd[i] = (i == 0 && D.stopped) ? CMD_DEBUG_START : cmd_run[i];
  }
  scr_put_rgb(bx + 2 + 18, y, ' ', 0, bg, 0);
  TB.n = 6;
}


/* a click on the toolbar: its command; 0 not on it */
int dbg_toolbar_hit (int x, int y) {
  int i;
  if (TB.n == 0 || y != TB.y) return 0;
  for (i = 0; i < TB.n; i++)
    if (x >= TB.x + 2 + i * 3 && x < TB.x + 5 + i * 3) return TB.cmd[i];
  return x >= TB.x && x < TB.x + 21 ? -1 : 0;
}

/* }================================================================== */
