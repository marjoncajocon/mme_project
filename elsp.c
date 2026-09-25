/*
** elsp.c - IntelliSense: a client of the Language Server Protocol
**
** A server per language (clangd, gopls, rust-analyzer ... as settings.json
** says) runs as a program; messages go to its stdin and come from its
** stdout, each a "Content-Length: n" header and n bytes of JSON-RPC. The
** open files are told to it whole at first (didOpen), then by the piece
** (didChange with the range that changed, when the server takes those);
** it answers completions and definitions, and sends diagnostics by itself.
** Positions are bytes when the server agrees to UTF-8, else UTF-16 units.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { RQ_INIT, RQ_COMPLETE, RQ_DEFINE, RQ_HOVER, RQ_SIGNATURE, RQ_RENAME, RQ_ACTIONS,
       RQ_RESOLVE, RQ_SYMBOLS, RQ_FORMAT, RQ_LOC_REFS, RQ_LOC_IMPL, RQ_LOC_TYPE, RQ_LOC_PEEK,
       RQ_WSYM, RQ_HIGHLIGHT, RQ_BULB, RQ_INLAY, RQ_SEMANTIC, RQ_LENS, RQ_LENS_RESOLVE, RQ_COMP_RESOLVE,
       RQ_ONTYPE, RQ_SOURCE, RQ_SOURCE_RESOLVE, RQ_SELRANGE, RQ_FOLDING, RQ_HPREP, RQ_HIER, RQ_INLINE, RQ_NEDIT,
       RQ_SIGNIN, RQ_SIGNOUT, RQ_CHECK, RQ_DEVICE, RQ_PULL, RQ_COLOR, RQ_COLOR_PRES, RQ_LINK,
       RQ_LINK_RESOLVE, RQ_WILL_RENAME,
       RQ_OTHER };

typedef struct Req {
  int id, kind;
  Doc *d;
  Pos at;	/* RQ_HIGHLIGHT: where it was asked */
} Req;

typedef struct Prog {	/* a $/progress that runs: "Loading packages..." */
  char token[64];
  char title[96];
  char msg[128];
  int pct;	/* -1: not said */
} Prog;

typedef struct Srv {
  char lang[16];
  OsProc proc;
  long pid;
  int gone;	/* restarted: another one does its work (it is kept, a question may still point at it) */
  Prog prog[8];
  int nprog;
  int to, from;	/* its stdin, its stdout */
  int err;	/* its stderr: to the OUTPUT view; -1 closed */
  char chan[64];	/* its output channel: "gopls" */
  Buf in;	/* what came, not yet a whole message */
  int id;
  int ready;	/* initialize was answered */
  int utf8;	/* positions in bytes, else in UTF-16 units */
  int dead;
  int told;	/* its end is in the output */
  Req req[64];
  int nreq;
  signed char semtok[64];	/* the server's token types (its legend) as T_*, -1: kept as it is */
  int nsem;
  int readonly_bit;	/* the modifier "readonly": a constant; -1 none */
  int can_resolve, can_range, can_sel, can_fold, can_calls, can_types;	/* what it said it does */
  int can_inline;	/* inlineCompletionProvider: editor.inlineSuggest asks it, nothing else does */
  int no_nedit;	/* it answered MethodNotFound to copilotInlineEdit: never asked again */
  char type_chars[16];	/* documentOnTypeFormattingProvider's characters */
  int sync_inc;	/* textDocumentSync 2: it takes the range that changed */
  int can_pull;	/* diagnosticProvider: its problems are asked for (textDocument/diagnostic) */
  char pull_id[64];	/* and the identifier it gave them */
  int can_color, can_link, link_resolve;	/* colorProvider, documentLinkProvider (and its resolve) */
  char *will_ren, *did_ren;	/* workspace.fileOperations: the filters of the renames it wants, as JSON */
} Srv;

typedef struct LDoc {
  Doc *d;
  Srv *s;
  char *uri;
  int version;
  unsigned long sent;	/* d->edits when the server last got the text */
  unsigned long seen;	/* d->edits when it last changed, and when that was */
  long long seen_at;
  int opened;	/* didOpen went */
  char *last;	/* the text the server has, to find what changed in it */
  size_t nlast;
  char *langid;	/* the file's language, not the server's: one server sees many */
  char *result_id;	/* the problems last pulled: the server says "unchanged" against it */
  int repull;	/* the server cancelled the pull: asked again a little later */
  long long repull_at;
} LDoc;

#define SYNC_WAIT	200000	/* us of quiet before the text goes: typing does not send a file a key */

typedef struct DFile {
  char *uri;
  Diag *v;	/* the server's, then the task's */
  size_t n;
  size_t nt;	/* the last nt are a task's (its problem matcher) */
} DFile;

#define MAX_SRV	64	/* restarts take new ones */
#define MAX_MESSAGE	((size_t)64 << 20)	/* a Content-Length no real server sends */

static Srv *g_srv[MAX_SRV];
static int g_nsrv;
static LDoc *g_doc;
static size_t g_ndoc, g_capdoc;
static DFile *g_diag;
static size_t g_ndiag, g_capdiag;
static char g_failed[1024];	/* " lang lang ": servers that could not start, told once */
static int g_log = -2;	/* $MME_LSPLOG: a file with every message, to see what goes wrong */
static int g_down;	/* quitting: a server that ends is not restarted */
static int g_will;	/* willRenameFiles a rename still waits for */


/* a server's number in range, else def: casting -1 or 1e300 to size_t or int is undefined in C */
static size_t unum (const Json *j, double def) {
  double v = json_num(j, def);
  if (v >= 0 && v < 1e15) return (size_t)v;
  return def >= 0 && def < 1e15 ? (size_t)def : 0;
}


static int inum (const Json *j, double def) {
  double v = json_num(j, def);
  if (v > -2e9 && v < 2e9) return (int)v;
  return def > -2e9 && def < 2e9 ? (int)def : 0;
}


/* a part of a message as JSON text (malloc'd), NULL when it is not there */
static char *json_text (const Json *j) {
  Buf b;
  if (j == NULL) return NULL;
  buf_init(&b);
  json_write(&b, j);
  buf_putc(&b, '\0');
  return buf_take(&b);
}


static void trace (const char *dir, const char *s, size_t n) {
  if (g_log == -2) {
    char *f = os_getenv("MME_LSPLOG");
    g_log = f ? os_open(f, OS_APPEND) : -1;
    free(f);
  }
  if (g_log < 0) return;
  os_write(g_log, dir, strlen(dir));
  os_write(g_log, s, n);
  os_write(g_log, "\n", 1);
}


/*
** {==================================================================
** URIs and positions
** ===================================================================
*/

/* file:///E:/w/a%20b.c for E:\w\a b.c */
static char *to_uri (const char *native) {
  static const char hex[] = "0123456789ABCDEF";
  char *real = os_realpath(native);
  const char *p = real ? real : native;
  Buf b;
  buf_init(&b);
  buf_puts(&b, "file://");
  if (p[0] != '/') buf_putc(&b, '/');
  for (; *p; p++) {
    unsigned char c = (unsigned char)*p;
    /* a backslash is the separator only where it is one: on the systems where
    ** it is an ordinary character of a name, turning it into / named another
    ** file. A quote or a control character has to go too, or the uri would
    ** break the JSON message it is put into. */
    if (c == '\\' && path_is_sep('\\')) buf_putc(&b, '/');
    else if (c < 32 || c > 127 || strchr(" \"\\%#?[]", c)) {
      buf_putc(&b, '%');
      buf_putc(&b, hex[c >> 4]);
      buf_putc(&b, hex[c & 15]);
    }
    else buf_putc(&b, (char)c);
  }
  free(real);
  return buf_take(&b);
}


char *lsp_path (const char *uri) {
  Buf b;
  const char *p = uri;
  if (strncmp(p, "file://", 7) == 0) p += 7;
#ifdef _WIN32
  if (p[0] == '/' && p[1] && (p[2] == ':' || (p[2] == '%' && p[3] == '3'))) p++;	/* /E:/ */
#endif
  buf_init(&b);
  for (; *p; p++) {
    if (*p == '%' && p[1] && p[2]) {
      char h[3];
      h[0] = p[1];
      h[1] = p[2];
      h[2] = '\0';
      buf_putc(&b, (char)strtol(h, NULL, 16));
      p += 2;
    }
#ifdef _WIN32
    else if (*p == '/') buf_putc(&b, '\\');
#endif
    else buf_putc(&b, *p);
  }
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/* the server's column of byte x in line y */
static size_t col_out (const Srv *s, const Doc *d, size_t y, size_t x) {
  const Row *r;
  size_t i = 0, u = 0, len;
  if (s->utf8 || y >= d->n) return x;
  r = &d->row[y];
  while (i < x && i < r->len) {
    uint32_t cp = utf8_decode(r->s + i, r->len - i, &len);
    u += cp >= 0x10000 ? 2 : 1;
    i += len;
  }
  return u;
}


/* the byte of the server's column c in line y */
static size_t col_in (const Srv *s, const Doc *d, size_t y, size_t c) {
  const Row *r;
  size_t i = 0, u = 0, len;
  if (y >= d->n) return c;
  r = &d->row[y];
  if (s->utf8) return c < r->len ? c : r->len;
  while (i < r->len && u < c) {
    uint32_t cp = utf8_decode(r->s + i, r->len - i, &len);
    u += cp >= 0x10000 ? 2 : 1;
    i += len;
  }
  return i;
}


static Pos pos_in (const Srv *s, const Doc *d, const Json *p) {
  Pos r;
  r.y = unum(json_get(p, "line"), 0);
  r.x = col_in(s, d, r.y, unum(json_get(p, "character"), 0));
  return r;
}

/* }================================================================== */


/*
** {==================================================================
** Talking
** ===================================================================
*/

static void send_msg (Srv *s, const Buf *body) {
  char hdr[64];
  int n = snprintf(hdr, sizeof(hdr), "Content-Length: %lu\r\n\r\n", (unsigned long)body->len);
  if (s->dead) return;
  trace(">> ", body->s, body->len);
  if (os_write(s->to, hdr, (size_t)n) < 0 || os_write(s->to, body->s, body->len) < 0) s->dead = 1;
}


/* a request: {"jsonrpc":"2.0","id":n,"method":m,"params":params}; its id */
static int request (Srv *s, const char *method, const char *params, int kind, Doc *d) {
  Buf b;
  int id = ++s->id;
  buf_init(&b);
  buf_printf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}", id, method, params);
  send_msg(s, &b);
  buf_free(&b);
  if (s->nreq == 64) {	/* the oldest is forgotten */
    memmove(s->req, s->req + 1, 63 * sizeof(Req));
    s->nreq--;
  }
  s->req[s->nreq].id = id;
  s->req[s->nreq].kind = kind;
  s->req[s->nreq].d = d;
  s->req[s->nreq].at.y = s->req[s->nreq].at.x = 0;
  s->nreq++;
  return id;
}


static unsigned g_comp_gen;	/* one more for each completion answer */
static void loc_fill (Srv *s, Loc *v, const char *uri, const Json *range);


static void notify (Srv *s, const char *method, const char *params) {
  Buf b;
  buf_init(&b);
  buf_printf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);
  send_msg(s, &b);
  buf_free(&b);
}


static DFile *dfile (const char *uri, int make);


/* a semantic token type as mme's token; -1: the highlighter's stays (keywords, strings, operators ...) */
static int sem_tok (const char *t) {
  static const struct {
    const char *name;
    int tok;
  } map[] = {
    {"namespace", T_TYPE}, {"type", T_TYPE}, {"class", T_TYPE}, {"enum", T_TYPE}, {"interface", T_TYPE},
    {"struct", T_TYPE}, {"typeParameter", T_TYPE}, {"parameter", T_VAR}, {"variable", T_VAR},
    {"property", T_VAR}, {"event", T_VAR}, {"enumMember", T_CONST}, {"function", T_FUNC},
    {"method", T_FUNC}, {"decorator", T_FUNC}, {"macro", T_STORAGE}
  };
  size_t i;
  for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (strcmp(map[i].name, t) == 0) return map[i].tok;
  return -1;
}


/* the line and the character of byte off, the way this server counts them */
static void text_pos (const char *t, size_t n, size_t off, int utf16,
                    unsigned long *line, unsigned long *ch) {
  size_t i, ls = 0;
  unsigned long ln = 0;
  if (off > n) off = n;
  for (i = 0; i < off; i++)
    if (t[i] == '\n') {
      ln++;
      ls = i + 1;
    }
  *line = ln;
  if (!utf16) {
    *ch = (unsigned long)(off - ls);
    return;
  }
  {	/* UTF-16 units, which is what a server asks for unless it took utf-8 */
    size_t j = ls, u = 0, len;
    while (j < off) {
      uint32_t cp = utf8_decode(t + j, n - j, &len);
      u += cp >= 0x10000 ? 2 : 1;
      j += len;
    }
    *ch = (unsigned long)u;
  }
}


/* a byte in the middle of a character: a change may not start or end there */
static int cont_byte (const char *t, size_t n, size_t i) {
  return i < n && ((unsigned char)t[i] & 0xC0) == 0x80;
}


/*
** textDocument/diagnostic (pull diagnostics): a server that says
** diagnosticProvider does not send a file's problems, it is asked for
** them when the file opens and after each change it is told; the last
** answer's resultId goes with the question, so "unchanged" can come back
*/
static void pull (LDoc *l) {
  Buf b;
  if (!l->s->can_pull || !l->opened) return;
  buf_init(&b);
  buf_printf(&b, "{\"textDocument\":{\"uri\":\"%s\"}", l->uri);
  if (l->s->pull_id[0]) {
    buf_puts(&b, ",\"identifier\":");
    json_put_str(&b, l->s->pull_id, strlen(l->s->pull_id));
  }
  if (l->result_id) {
    buf_puts(&b, ",\"previousResultId\":");
    json_put_str(&b, l->result_id, strlen(l->result_id));
  }
  buf_putc(&b, '}');
  request(l->s, "textDocument/diagnostic", b.s, RQ_PULL, l->d);
  buf_free(&b);
  l->repull = 0;
}


static void did_open (LDoc *l) {
  Buf b;
  Pos a;
  size_t len = 0;
  char *txt;
  a.y = a.x = 0;
  txt = doc_text(l->d, a, doc_end(l->d), &len);
  if (txt == NULL) return;
  buf_init(&b);
  buf_printf(&b, "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"%s\",\"version\":%d,\"text\":",
             l->uri, l->langid != NULL ? l->langid : l->s->lang, ++l->version);
  json_put_str(&b, txt, len);
  buf_puts(&b, "}}");
  notify(l->s, "textDocument/didOpen", b.s);
  buf_free(&b);
  free(l->last);
  l->last = txt;	/* what the server has now: didChange is found against it */
  l->nlast = len;
  l->opened = 1;
  l->sent = l->d->edits;
  pull(l);
}


/*
** What changed since the server was last told: the common prefix and the
** common suffix of the old text and the new one are what stayed, so one
** content change covers what is between them. A server that wants the file
** whole (textDocumentSync 1) still gets it whole.
*/
static void did_change (LDoc *l) {
  Buf b;
  Pos a;
  size_t len = 0, p = 0, s = 0, keep;
  char *txt;
  const char *old = l->last;
  a.y = a.x = 0;
  txt = doc_text(l->d, a, doc_end(l->d), &len);
  if (txt == NULL) return;
  buf_init(&b);
  buf_printf(&b, "{\"textDocument\":{\"uri\":\"%s\",\"version\":%d},\"contentChanges\":[{",
             l->uri, ++l->version);
  if (!l->s->sync_inc || old == NULL) buf_puts(&b, "\"text\":");
  else {
    unsigned long l0, c0, l1, c1;
    keep = len < l->nlast ? len : l->nlast;
    while (p < keep && txt[p] == old[p]) p++;
    while (s < len - p && s < l->nlast - p && txt[len - 1 - s] == old[l->nlast - 1 - s]) s++;
    while (p > 0 && cont_byte(txt, len, p)) p--;	/* both ends on a character */
    while (s > 0 && cont_byte(txt, len, len - s)) s--;
    if (p == len && p == l->nlast) {	/* the same text after all: say nothing */
      buf_free(&b);
      free(txt);
      l->version--;
      l->sent = l->d->edits;
      return;
    }
    text_pos(old, l->nlast, p, !l->s->utf8, &l0, &c0);
    text_pos(old, l->nlast, l->nlast - s, !l->s->utf8, &l1, &c1);
    buf_printf(&b, "\"range\":{\"start\":{\"line\":%lu,\"character\":%lu},"
               "\"end\":{\"line\":%lu,\"character\":%lu}},\"text\":", l0, c0, l1, c1);
  }
  if (!l->s->sync_inc || old == NULL) json_put_str(&b, txt, len);
  else json_put_str(&b, txt + p, len - s - p);
  buf_puts(&b, "}]}");
  notify(l->s, "textDocument/didChange", b.s);
  buf_free(&b);
  free(l->last);
  l->last = txt;
  l->nlast = len;
  l->sent = l->d->edits;
  pull(l);	/* the debounce that sent the change is the pull's too */
}

/* }================================================================== */


/* gopls: its inlay hints, semantic tokens and test lenses on, as VS Code's Go extension asks */
#define GOPLS_OPTIONS	"{\"hints\":{\"assignVariableTypes\":true,\"compositeLiteralFields\":true," \
  "\"compositeLiteralTypes\":true,\"constantValues\":true,\"functionTypeParameters\":true," \
  "\"parameterNames\":true,\"rangeVariableTypes\":true},\"semanticTokens\":true," \
  "\"codelenses\":{\"test\":true,\"generate\":true}}"


/*
** {==================================================================
** Servers
** ===================================================================
*/

/* "typescript-language-server --stdio": the words, the program found in PATH */
static char **split_cmd (const char *cmd) {
  char **argv = (char **)xmalloc(17 * sizeof(char *));
  int n = 0;
  while (*cmd && n < 16) {
    const char *e;
    while (*cmd == ' ') cmd++;
    if (!*cmd) break;
    for (e = cmd; *e && *e != ' '; e++) ;
    argv[n++] = xstrndup(cmd, (size_t)(e - cmd));
    cmd = e;
  }
  argv[n] = NULL;
  if (n > 0 && !path_is_sep(argv[0][0]) && !(argv[0][0] && argv[0][1] == ':')) {
    char *full = find_program(argv[0]);
    if (full == NULL) {
      int i;
      for (i = 0; i < n; i++) free(argv[i]);
      free(argv);
      return NULL;
    }
    free(argv[0]);
    argv[0] = full;
  }
  return argv;
}


static void auth_forget (void);	/* the inline server's account, below: a new one is nobody yet */

static Srv *start (const char *lang) {
  const char *cmd = settings_server(lang);
  char **argv, key[32], *root;
  int to[2], from[2], err[2], io[3], i;
  long pid;
  Srv *s;
  Buf b;
  snprintf(key, sizeof(key), " %s ", lang);
  if (cmd == NULL || !*cmd || g_nsrv == MAX_SRV || strstr(g_failed, key)) return NULL;
  argv = split_cmd(cmd);
  if (argv == NULL || os_pipe(to) != 0 || os_pipe(from) != 0) {
    if (strlen(g_failed) + strlen(key) < sizeof(g_failed)) strcat(g_failed, key);
    toast(1, "IntelliSense for %s: '%s' was not found", lang, cmd);
    if (argv)
      for (i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
    return NULL;
  }
  if (os_pipe(err) != 0) err[0] = err[1] = -1;
  io[0] = to[0];
  io[1] = from[1];
  io[2] = err[1];
  s = (Srv *)xmalloc(sizeof(Srv));
  memset(s, 0, sizeof(*s));
  snprintf(s->lang, sizeof(s->lang), "%s", lang);
  {	/* the OUTPUT view's channel: the program's name */
    char *dot;
    snprintf(s->chan, sizeof(s->chan), "%s", path_basename(argv[0]));
    if ((dot = strrchr(s->chan, '.')) != NULL && dot != s->chan) *dot = '\0';
  }
  root = xstrdup(side_root());
  {	/* the server works in the folder that is open */
    char *cwd = os_getcwd();
    os_chdir(root);
    i = os_spawn(argv[0], argv, NULL, io, 3, &s->proc, &pid);
    if (cwd) os_chdir(cwd);
    free(cwd);
  }
  os_close(to[0]);
  os_close(from[1]);
  if (err[1] >= 0) os_close(err[1]);
  if (i != 0) {
    out_log(s->chan, "[error] Could not start %s", cmd);
    os_close(to[1]);
    os_close(from[0]);
    if (err[0] >= 0) os_close(err[0]);
    free(s);
    free(root);
    if (strlen(g_failed) + strlen(key) < sizeof(g_failed)) strcat(g_failed, key);
    for (i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
    return NULL;
  }
  for (i = 0; argv[i]; i++) free(argv[i]);
  free(argv);
  s->to = to[1];
  s->from = from[0];
  s->err = err[0];
  s->pid = pid;
  out_log(s->chan, "[info] Starting %s for %s in %s", cmd, lang, root);
  if (strcmp(lang, INLINE_LANG) == 0) auth_forget();	/* checkStatus asks this one again */
  buf_init(&s->in);
  g_srv[g_nsrv++] = s;
  {	/* initialize */
    char *uri = to_uri(root);
    buf_init(&b);
    buf_printf(&b, "{\"processId\":%ld,\"rootUri\":\"%s\",\"workspaceFolders\":[{\"uri\":\"%s\",\"name\":",
               os_getpid(), uri, uri);
    json_put_str(&b, path_basename(root), strlen(path_basename(root)));
    buf_puts(&b, "}],\"clientInfo\":{\"name\":\"mme\"},\"capabilities\":{"
                 "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
                 "\"window\":{\"workDoneProgress\":true,\"showDocument\":{\"support\":true},"
                 "\"showMessage\":{\"messageActionItem\":{\"additionalPropertiesSupport\":false}}},"
                 "\"textDocument\":{\"synchronization\":{\"didSave\":false},"
                 "\"completion\":{\"completionItem\":{\"snippetSupport\":true,"
                 "\"documentationFormat\":[\"markdown\",\"plaintext\"],"
                 "\"resolveSupport\":{\"properties\":[\"documentation\",\"detail\",\"additionalTextEdits\"]}}},"
                 "\"rangeFormatting\":{},\"onTypeFormatting\":{},\"selectionRange\":{},"
                 "\"foldingRange\":{\"lineFoldingOnly\":true},\"callHierarchy\":{},\"typeHierarchy\":{},"
                 "\"definition\":{},\"publishDiagnostics\":{},\"diagnostic\":{\"relatedDocumentSupport\":true},"
                 "\"colorProvider\":{},\"documentLink\":{\"tooltipSupport\":true},"
                 "\"hover\":{\"contentFormat\":[\"plaintext\",\"markdown\"]},"
                 "\"signatureHelp\":{\"signatureInformation\":{\"parameterInformation\":{\"labelOffsetSupport\":true}}},"
                 "\"rename\":{},\"documentSymbol\":{\"hierarchicalDocumentSymbolSupport\":true},"
                 "\"references\":{},\"implementation\":{\"linkSupport\":true},"
                 "\"typeDefinition\":{\"linkSupport\":true},\"documentHighlight\":{},"
                 "\"inlayHint\":{},\"codeLens\":{},"
                 "\"semanticTokens\":{\"requests\":{\"full\":true},\"formats\":[\"relative\"],"
                 "\"tokenTypes\":[\"namespace\",\"type\",\"class\",\"enum\",\"interface\",\"struct\","
                 "\"typeParameter\",\"parameter\",\"variable\",\"property\",\"enumMember\",\"event\","
                 "\"function\",\"method\",\"macro\",\"keyword\",\"modifier\",\"comment\",\"string\","
                 "\"number\",\"regexp\",\"operator\",\"decorator\"],"
                 "\"tokenModifiers\":[\"declaration\",\"definition\",\"readonly\",\"static\","
                 "\"deprecated\",\"abstract\",\"async\",\"modification\",\"documentation\","
                 "\"defaultLibrary\"]},"
                 "\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":"
                 "[\"quickfix\",\"refactor\",\"source\",\"source.organizeImports\"]}},"
                 "\"resolveSupport\":{\"properties\":[\"edit\"]}}},"
                 "\"workspace\":{\"workspaceFolders\":true,\"configuration\":true,\"symbol\":{},"
                 "\"applyEdit\":true,\"workspaceEdit\":{\"documentChanges\":true},"
                 "\"diagnostics\":{\"refreshSupport\":true},"
                 "\"fileOperations\":{\"willRename\":true,\"didRename\":true}}}");
    /* editorInfo: Copilot's server asks who it is talking to, and a server
    ** that does not know these options ignores them. gopls wants its own at
    ** the top level too, so they join these instead of nesting under them. */
    buf_printf(&b, ",\"initializationOptions\":{\"editorInfo\":{\"name\":\"mme\",\"version\":\"%s\"},"
               "\"editorPluginInfo\":{\"name\":\"mme\",\"version\":\"%s\"}", MME_VERSION, MME_VERSION);
    if (strcmp(s->lang, "go") == 0) buf_printf(&b, ",%s", &GOPLS_OPTIONS[1]);	/* past its own brace */
    else buf_putc(&b, '}');
    buf_putc(&b, '}');
    request(s, "initialize", b.s, RQ_INIT, NULL);
    buf_free(&b);
    free(uri);
  }
  free(root);
  return s;
}


static Srv *server (const char *lang) {
  int i;
  for (i = 0; i < g_nsrv; i++)
    if (!g_srv[i]->gone && strcmp(g_srv[i]->lang, lang) == 0) return g_srv[i]->dead ? NULL : g_srv[i];
  return start(lang);
}


/* the server for lang now (not one restarted since), running or not; NULL: none started */
static Srv *srv_of (const char *lang) {
  int i;
  for (i = g_nsrv - 1; i >= 0; i--)
    if (!g_srv[i]->gone && strcmp(g_srv[i]->lang, lang) == 0) return g_srv[i];
  return NULL;
}


/* a server stopped for good: its pipes closed, its process ended; the struct stays */
static void srv_stop (Srv *s) {
  int status;
  if (!s->dead) {
    request(s, "shutdown", "null", RQ_OTHER, NULL);
    notify(s, "exit", "null");
  }
  s->dead = s->told = s->gone = 1;
  s->nprog = 0;
  os_close(s->to);
  os_close(s->from);
  if (s->err >= 0) os_close(s->err);
  s->err = -1;
  if (s->pid > 0 && os_poll_proc(s->proc, &status) == 0) os_kill(s->pid, 9);
}


static void diag_drop (const char *uri);

/*
** mme: Restart Language Server: the server of lang stops, a new one starts
** and gets the files the old one had; one that was not found is looked for
** again.
*/
void lsp_restart (const char *lang) {
  Srv *old = srv_of(lang), *ns;
  char key[32], *at;
  size_t k;
  snprintf(key, sizeof(key), " %s ", lang);
  if ((at = strstr(g_failed, key)) != NULL) memmove(at, at + strlen(key) - 1, strlen(at + strlen(key) - 1) + 1);
  if (old) {
    out_log(old->chan, "[info] Restarting the %s language server", lang);
    srv_stop(old);
  }
  ns = start(lang);
  for (k = 0; k < g_ndoc; k++) {
    LDoc *l = &g_doc[k];
    if (old == NULL || l->s != old) continue;
    diag_drop(l->uri);	/* the new one says them again */
    free(l->last);	/* what the old server had: the new one has nothing */
    l->last = NULL;
    l->nlast = 0;
    if (ns) {
      l->s = ns;
      l->opened = 0;
      l->version = 0;
    }
    else {	/* nothing to talk to: the file is opened again later */
      free(l->uri);
      free(l->langid);
      *l = g_doc[--g_ndoc];
      k--;
    }
  }
}


/* the servers that crashed lately, VS Code's rule: 5 in 3 minutes and it is not restarted */
static struct {
  char lang[16];
  long long t[5];
  int n;
  int given_up;
} g_crash[16];


static void show_output_done (void *ud, int choice) {
  if (choice == 0) on_show_output((const char *)ud);
}


/* a server ended by itself: started again, unless it keeps doing that */
static void crashed (Srv *s) {
  static char chan[16][64];
  long long now = os_now_us();
  int i, c = -1, recent = 0;
  for (i = 0; i < 16 && g_crash[i].lang[0]; i++)
    if (strcmp(g_crash[i].lang, s->lang) == 0) c = i;
  if (c < 0 && i < 16) {
    c = i;
    snprintf(g_crash[c].lang, sizeof(g_crash[c].lang), "%s", s->lang);
  }
  if (c < 0 || g_crash[c].given_up) return;
  if (g_crash[c].n == 5) {
    memmove(g_crash[c].t, g_crash[c].t + 1, 4 * sizeof(long long));
    g_crash[c].n--;
  }
  g_crash[c].t[g_crash[c].n++] = now;
  for (i = 0; i < g_crash[c].n; i++)
    if (now - g_crash[c].t[i] < 180000000LL) recent++;
  if (recent >= 5) {
    static const char *const act[] = {"Show Output"};
    char msg[200];
    g_crash[c].given_up = 1;
    snprintf(chan[c], sizeof(chan[c]), "%s", s->chan);
    snprintf(msg, sizeof(msg), "The %s server crashed 5 times in the last 3 minutes. The server will not be restarted.", s->chan);
    out_log(s->chan, "[error] %s", msg);
    toast_ask(2, s->chan, msg, act, 1, show_output_done, chan[c]);
    return;
  }
  out_log(s->chan, "[info] The %s server crashed; restarting it (%d of 5)", s->chan, recent);
  lsp_restart(s->lang);
}


/* how the server of lang is: LS_*; name gets its program's name ("gopls") */
int lsp_state (const char *lang, char *name, size_t n) {
  const char *cmd = lang ? settings_server(lang) : NULL;
  char key[32];
  Srv *s;
  if (name && n) name[0] = '\0';
  if (cmd == NULL || !*cmd) return LS_NONE;
  if (name && n) {	/* the program: its first word, no folder, no .exe */
    const char *e = strchr(cmd, ' '), *b;
    size_t len = e ? (size_t)(e - cmd) : strlen(cmd);
    char *prog = xstrndup(cmd, len), *dot;
    b = path_basename(prog);
    snprintf(name, n, "%s", b);
    if ((dot = strrchr(name, '.')) != NULL && dot != name && m_strnicmp(dot, ".exe", 4) == 0) *dot = '\0';
    free(prog);
  }
  snprintf(key, sizeof(key), " %s ", lang);
  if ((s = srv_of(lang)) != NULL) {
    if (s->dead) return LS_DEAD;
    if (!s->ready) return LS_STARTING;
    return s->nprog ? LS_BUSY : LS_READY;
  }
  return strstr(g_failed, key) ? LS_MISSING : LS_OFF;
}


/* the OUTPUT channel of lang's server ("gopls"); NULL: none */
const char *lsp_channel (const char *lang) {
  Srv *s = lang ? srv_of(lang) : NULL;
  return s ? s->chan : NULL;
}


/* the progress that runs now, of every server: how many */
int lsp_progress_count (void) {
  int i, n = 0;
  for (i = 0; i < g_nsrv; i++)
    if (!g_srv[i]->gone && !g_srv[i]->dead) n += g_srv[i]->nprog;
  return n;
}


/* the i-th: "gopls: Loading packages... (42%)" in buf; its percentage, -1 not said */
int lsp_progress_text (int i, char *buf, size_t n) {
  int k;
  for (k = 0; k < g_nsrv; k++) {
    Srv *s = g_srv[k];
    if (s->gone || s->dead) continue;
    if (i < s->nprog) {
      const Prog *p = &s->prog[s->nprog - 1 - i];	/* the newest first */
      char pc[16] = "";
      if (p->pct >= 0) snprintf(pc, sizeof(pc), " (%d%%)", p->pct);
      snprintf(buf, n, "%s: %s%s%s%s", s->chan, p->title, p->title[0] && p->msg[0] ? " " : "", p->msg, pc);
      return p->pct;
    }
    i -= s->nprog;
  }
  if (n) buf[0] = '\0';
  return -1;
}


/* $/progress: begin, report, end of a work done progress */
static void progress (Srv *s, const Json *params) {
  const Json *tk = json_get(params, "token"), *v = json_get(params, "value");
  const char *kind = json_str(json_get(v, "kind"), "");
  char token[64];
  int i, at = -1;
  if (tk == NULL || v == NULL) return;
  if (tk->type == J_STR) snprintf(token, sizeof(token), "%s", tk->str);
  else snprintf(token, sizeof(token), "%.0f", tk->num);
  for (i = 0; i < s->nprog; i++)
    if (strcmp(s->prog[i].token, token) == 0) at = i;
  if (strcmp(kind, "begin") == 0) {
    Prog *p;
    if (at < 0) {
      if (s->nprog == 8) {	/* the oldest is forgotten */
        memmove(s->prog, s->prog + 1, 7 * sizeof(Prog));
        s->nprog--;
      }
      at = s->nprog++;
    }
    p = &s->prog[at];
    snprintf(p->token, sizeof(p->token), "%s", token);
    snprintf(p->title, sizeof(p->title), "%s", json_str(json_get(v, "title"), ""));
    snprintf(p->msg, sizeof(p->msg), "%s", json_str(json_get(v, "message"), ""));
    p->pct = json_get(v, "percentage") ? inum(json_get(v, "percentage"), -1) : -1;
    out_log(s->chan, "[info] %s %s", p->title, p->msg);
  }
  else if (strcmp(kind, "report") == 0 && at >= 0) {
    Prog *p = &s->prog[at];
    if (json_get(v, "message")) snprintf(p->msg, sizeof(p->msg), "%s", json_str(json_get(v, "message"), ""));
    if (json_get(v, "percentage")) p->pct = inum(json_get(v, "percentage"), -1);
  }
  else if (strcmp(kind, "end") == 0 && at >= 0) {
    memmove(s->prog + at, s->prog + at + 1, (size_t)(s->nprog - at - 1) * sizeof(Prog));
    s->nprog--;
  }
  if (at >= 0 && at < s->nprog && s->prog[at].pct > 100) s->prog[at].pct = 100;
}


void lsp_shutdown (void) {
  int i;
  g_down = 1;
  for (i = 0; i < g_nsrv; i++) {
    Srv *s = g_srv[i];
    if (s->gone) continue;	/* stopped already */
    if (!s->dead) {
      request(s, "shutdown", "null", RQ_OTHER, NULL);
      notify(s, "exit", "null");
    }
    os_close(s->to);
    os_close(s->from);
    if (s->err >= 0) os_close(s->err);
    /* a server that ignores the exit must not outlive the editor; not when it is
    ** dead already: os_poll_proc took its handle then, and the id can be another's */
    if (s->pid > 0 && !s->dead) os_kill(s->pid, 9);
    s->dead = 1;
  }
  g_nsrv = 0;
}

/* }================================================================== */


/*
** {==================================================================
** Files
** ===================================================================
*/

/* the language id the protocol uses, for the highlighter's language */
const char *lsp_lang (const char *syntax) {
  return syntax_id(syntax);	/* a server only starts when settings.json names one for it */
}


/* the server of a document's own language (never the inline one) */
static LDoc *ldoc (const Doc *d) {
  size_t i;
  for (i = 0; i < g_ndoc; i++)
    if (g_doc[i].d == d && strcmp(g_doc[i].s->lang, INLINE_LANG) != 0) return &g_doc[i];
  return NULL;
}


/* the inline-completion server's binding of a document, when there is one */
static LDoc *ldoc_inline_only (const Doc *d) {
  size_t i;
  for (i = 0; i < g_ndoc; i++)
    if (g_doc[i].d == d && strcmp(g_doc[i].s->lang, INLINE_LANG) == 0) return &g_doc[i];
  return NULL;
}


/*
** Who answers inline suggestions for this document: the server named by
** mme.inlineCompletionServer, or the language's own when that is what
** advertised the capability (naming Copilot in mme.languageServers still
** works, it just costs that language its real server).
*/
static LDoc *ldoc_inline (const Doc *d) {
  LDoc *l = ldoc_inline_only(d);
  if (l != NULL) return l;
  l = ldoc(d);
  return (l != NULL && l->s->can_inline) ? l : NULL;
}


/* one binding of a document to a server; the file's language goes with it */
static void bind_doc (Doc *d, Srv *s, const char *langid) {
  LDoc *l;
  if (g_ndoc == g_capdoc) {
    g_capdoc = g_capdoc ? g_capdoc * 2 : 16;
    g_doc = (LDoc *)xrealloc(g_doc, g_capdoc * sizeof(LDoc));
  }
  l = &g_doc[g_ndoc++];
  memset(l, 0, sizeof(*l));
  l->d = d;
  l->s = s;
  l->langid = xstrdup(langid);
  l->uri = to_uri(d->path);
  if (s->ready) did_open(l);
}


/* one binding goes: its server is told the file is closed, and the last takes its place */
static void unbind (LDoc *l) {
  if (l->opened) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"textDocument\":{\"uri\":\"%s\"}}", l->uri);
    notify(l->s, "textDocument/didClose", b.s);
    buf_free(&b);
  }
  free(l->uri);
  free(l->last);
  free(l->langid);
  free(l->result_id);
  *l = g_doc[--g_ndoc];
}


/* is this binding for a file of this language? (its languageId says so) */
static int binds_lang (const LDoc *l, const char *lang) {
  return strcmp(l->langid != NULL ? l->langid : "", lang) == 0;
}


void lsp_open (Doc *d, const char *syntax) {
  const char *lang = lsp_lang(syntax);
  LDoc *l;
  Srv *s;
  if (d->path == NULL) return;
  /* Change Language Mode: what it is bound to is the language it had, and so
  ** is the languageId its servers were told. Those bindings go, and are made
  ** again below for the language it has now - which is another server. */
  if (lang != NULL) {
    if ((l = ldoc(d)) != NULL && !binds_lang(l, lang)) unbind(l);
    if ((l = ldoc_inline_only(d)) != NULL && !binds_lang(l, lang)) unbind(l);
  }
  if (lang != NULL && ldoc(d) == NULL && (s = server(lang)) != NULL) bind_doc(d, s, lang);
  if (ldoc_inline_only(d) == NULL) {	/* and the one that answers for every language */
    const char *cmd = settings_server(INLINE_LANG);
    if (cmd != NULL && cmd[0] != '\0' && (s = server(INLINE_LANG)) != NULL)
      bind_doc(d, s, lang != NULL ? lang : "plaintext");
  }
}


static void lens_forget (const Doc *d);	/* a closed file's code lenses, below */
static void link_forget (const Doc *d);	/* and its document links */
static void inline_forget (void);	/* the inline suggestions kept, below */
static void nedit_forget (void);	/* and the next edit, below that */
static const Doc *nedit_doc (void);
static const Doc *inline_doc (void);


void lsp_close (Doc *d) {
  int i, k;
  for (i = 0; i < g_nsrv; i++)	/* its answers still to come are dropped: d is about to go */
    for (k = 0; k < g_srv[i]->nreq; k++)
      if (g_srv[i]->req[k].d == d) {
        g_srv[i]->req[k].kind = RQ_OTHER;
        g_srv[i]->req[k].d = NULL;
      }
  lens_forget(d);	/* its lenses too: lsp_lens_run would use d after it is freed */
  link_forget(d);
  if (inline_doc() == d) inline_forget();	/* and its inline suggestions */
  if (nedit_doc() == d) nedit_forget();	/* and the next edit it was offered */
  for (i = (int)g_ndoc - 1; i >= 0; i--)	/* a document may be on two servers */
    if (g_doc[i].d == d) unbind(&g_doc[i]);	/* downwards: what fills the slot is already past */
}


int lsp_active (const Doc *d) {
  const LDoc *l = ldoc(d);
  return l && l->s->ready && !l->s->dead;
}


/* the server has the text as it is now, before it is asked about it */
static LDoc *synced (const Doc *d) {
  LDoc *l = ldoc(d);
  if (l == NULL || !l->s->ready || l->s->dead) return NULL;
  if (!l->opened) did_open(l);
  else if (l->sent != d->edits) did_change(l);
  return l;
}


static void ask (Doc *d, Pos at, const char *method, int kind) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL) return;
  snprintf(params, sizeof(params),
           "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%lu,\"character\":%lu}}",
           l->uri, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x));
  request(l->s, method, params, kind, d);
}


void lsp_hover (Doc *d, Pos at) {
  ask(d, at, "textDocument/hover", RQ_HOVER);
}


void lsp_signature (Doc *d, Pos at) {
  ask(d, at, "textDocument/signatureHelp", RQ_SIGNATURE);
}


void lsp_rename (Doc *d, Pos at, const char *name) {
  LDoc *l = synced(d);
  Buf b;
  if (l == NULL) return;
  buf_init(&b);
  buf_printf(&b, "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%lu,\"character\":%lu},\"newName\":",
             l->uri, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x));
  json_put_str(&b, name, strlen(name));
  buf_putc(&b, '}');
  request(l->s, "textDocument/rename", b.s, RQ_RENAME, d);
  buf_free(&b);
}


/* the code actions: kept as the server sent them, to be sent back */
static struct {
  Srv *s;
  char **json;
  size_t n;
} g_act;


static void ask_actions_only (Doc *d, Pos a, Pos b, int kind, const char *only);

static void ask_actions (Doc *d, Pos a, Pos b, int kind) {
  ask_actions_only(d, a, b, kind, NULL);
}


/* the code actions of a..b; only: their kind ("source.organizeImports"), NULL: every one */
static void ask_actions_only (Doc *d, Pos a, Pos b, int kind, const char *only) {
  LDoc *l = synced(d);
  const DFile *f;
  Buf q;
  size_t i;
  int first = 1;
  if (l == NULL) return;
  buf_init(&q);
  buf_printf(&q, "{\"textDocument\":{\"uri\":\"%s\"},\"range\":{\"start\":{\"line\":%lu,\"character\":%lu},"
             "\"end\":{\"line\":%lu,\"character\":%lu}},\"context\":{\"diagnostics\":[",
             l->uri, (unsigned long)a.y, (unsigned long)col_out(l->s, d, a.y, a.x),
             (unsigned long)b.y, (unsigned long)col_out(l->s, d, b.y, b.x));
  f = dfile(l->uri, 0);
  for (i = 0; f && i < f->n; i++) {	/* the problems on these lines go with it */
    const Diag *g = &f->v[i];
    if (g->b.y < a.y || g->a.y > b.y) continue;
    if (!first) buf_putc(&q, ',');
    first = 0;
    buf_printf(&q, "{\"range\":{\"start\":{\"line\":%lu,\"character\":%lu},\"end\":{\"line\":%lu,\"character\":%lu}},"
               "\"severity\":%d,\"message\":", (unsigned long)g->a.y,
               (unsigned long)col_out(l->s, d, g->a.y, g->a.x), (unsigned long)g->b.y,
               (unsigned long)col_out(l->s, d, g->b.y, g->b.x), g->sev);
    json_put_str(&q, g->msg, strlen(g->msg));
    buf_putc(&q, '}');
  }
  buf_putc(&q, ']');
  if (only) buf_printf(&q, ",\"only\":[\"%s\"]", only);
  buf_puts(&q, "}}");
  request(l->s, "textDocument/codeAction", q.s, kind, d);
  l->s->req[l->s->nreq - 1].at = a;
  buf_free(&q);
}


void lsp_actions (Doc *d, Pos a, Pos b) {
  ask_actions(d, a, b, RQ_ACTIONS);
}


/* Organize Imports, Source Action...: the whole file's actions of that kind */
void lsp_source_action (Doc *d, const char *kind, int apply) {
  Pos a, b;
  a.y = a.x = 0;
  b = doc_end(d);
  ask_actions_only(d, a, b, apply ? RQ_SOURCE : RQ_ACTIONS, kind);
  if (apply && ldoc(d) && ldoc(d)->s->nreq) ldoc(d)->s->req[ldoc(d)->s->nreq - 1].at.x = (size_t)apply;
}


int lsp_source_pending (void) {
  int i, k;
  for (i = 0; i < g_nsrv; i++)
    for (k = 0; k < g_srv[i]->nreq; k++)
      if (g_srv[i]->req[k].kind == RQ_SOURCE || g_srv[i]->req[k].kind == RQ_SOURCE_RESOLVE) return 1;
  return 0;
}


/* the inlay hints of lines y0 .. y1 */
void lsp_inlay (Doc *d, size_t y0, size_t y1) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL) return;
  snprintf(params, sizeof(params),
           "{\"textDocument\":{\"uri\":\"%s\"},\"range\":{\"start\":{\"line\":%lu,\"character\":0},"
           "\"end\":{\"line\":%lu,\"character\":0}}}", l->uri, (unsigned long)y0, (unsigned long)y1);
  request(l->s, "textDocument/inlayHint", params, RQ_INLAY, d);
  l->s->req[l->s->nreq - 1].at.y = y0;
  l->s->req[l->s->nreq - 1].at.x = y1;
}


/* the whole file's semantic tokens, for the text as it is now */
void lsp_semantic (Doc *d) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL || l->s->nsem == 0) return;
  snprintf(params, sizeof(params), "{\"textDocument\":{\"uri\":\"%s\"}}", l->uri);
  request(l->s, "textDocument/semanticTokens/full", params, RQ_SEMANTIC, d);
  l->s->req[l->s->nreq - 1].at.x = (size_t)d->edits;
}


static void run_command (Srv *s, const Json *cmd);


/* the code lenses: kept as the server sent them, to run their commands */
static struct {
  Srv *s;
  Doc *d;
  char **json;
  size_t n;
} g_lens;


/* the code lenses of a file that is closing: its Doc must not be used again */
static void lens_forget (const Doc *d) {
  if (g_lens.d == d) {
    g_lens.d = NULL;
    g_lens.s = NULL;
  }
}


void lsp_lens (Doc *d) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL) return;
  snprintf(params, sizeof(params), "{\"textDocument\":{\"uri\":\"%s\"}}", l->uri);
  request(l->s, "textDocument/codeLens", params, RQ_LENS, d);
}


/* the lenses to mme: their line and title */
static void lens_tell (void) {
  Lens *v = (Lens *)xmalloc((g_lens.n + 1) * sizeof(Lens));
  size_t i;
  for (i = 0; i < g_lens.n; i++) {
    Json *j = json_parse(g_lens.json[i], strlen(g_lens.json[i]));
    v[i].y = unum(json_get(j, "range.start.line"), 0);
    v[i].title = xstrdup(json_str(json_get(j, "command.title"), ""));
    json_free(j);
  }
  on_lens(g_lens.d, v, g_lens.n);
}


static void lenses (Srv *s, Doc *d, const Json *res) {
  size_t i;
  for (i = 0; i < g_lens.n; i++) free(g_lens.json[i]);
  free(g_lens.json);
  g_lens.json = NULL;
  g_lens.n = 0;
  g_lens.s = s;
  g_lens.d = d;
  if (res && res->type == J_ARR && res->n) {
    g_lens.json = (char **)xmalloc(res->n * sizeof(char *));
    for (i = 0; i < res->n; i++) {
      Buf b;
      buf_init(&b);
      json_write(&b, res->kid[i]);
      buf_putc(&b, '\0');
      g_lens.json[g_lens.n++] = buf_take(&b);
      if (!json_get(res->kid[i], "command") && i < 100) {	/* its title comes later */
        request(s, "codeLens/resolve", g_lens.json[i], RQ_LENS_RESOLVE, d);
        s->req[s->nreq - 1].at.x = i;
      }
    }
  }
  lens_tell();
}


void lsp_lens_run (size_t i) {
  Json *j;
  const Json *cmd;
  if (i >= g_lens.n || g_lens.s == NULL || g_lens.s->dead) return;
  j = json_parse(g_lens.json[i], strlen(g_lens.json[i]));
  cmd = json_get(j, "command");
  if (cmd && json_get(cmd, "command")) {
    const char *c = json_str(json_get(cmd, "command"), "");
    if (strcmp(c, "editor.action.showReferences") == 0 || strstr(c, "showReferences")) {	/* VS Code's own: the references there */
      const Json *at = json_get(cmd, "arguments");
      Pos p;
      p.y = unum(json_get(j, "range.start.line"), 0);
      p.x = at && at->type == J_ARR && at->n > 1 ? col_in(g_lens.s, g_lens.d, p.y, unum(json_get(at->kid[1], "character"), 0)) : 0;
      lsp_locations(g_lens.d, p, LOC_REFS);
    }
    else run_command(g_lens.s, cmd);
  }
  json_free(j);
}


/* are there code actions on line y? (the lightbulb) the answer goes to on_bulb */
void lsp_bulb (Doc *d, size_t y) {
  Pos a, b;
  a.y = b.y = y;
  a.x = 0;
  b.x = y < d->n ? d->row[y].len : 0;
  ask_actions(d, a, b, RQ_BULB);
}


void lsp_symbols (Doc *d) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL) return;
  snprintf(params, sizeof(params), "{\"textDocument\":{\"uri\":\"%s\"}}", l->uri);
  request(l->s, "textDocument/documentSymbol", params, RQ_SYMBOLS, d);
}


/* textDocument/formatting, as the file's indent says */
int lsp_format (Doc *d) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL) return 0;
  snprintf(params, sizeof(params),
           "{\"textDocument\":{\"uri\":\"%s\"},\"options\":{\"tabSize\":%d,\"insertSpaces\":%s,"
           "\"trimTrailingWhitespace\":true}}", l->uri, d->indent, d->tabs ? "false" : "true");
  request(l->s, "textDocument/formatting", params, RQ_FORMAT, d);
  return 1;
}


/* references, implementations, type definitions, a definition to peek: to on_locations */
void lsp_locations (Doc *d, Pos at, int what) {
  LDoc *l = synced(d);
  char params[1200];
  static const char *const method[] = {"textDocument/references", "textDocument/implementation",
                                       "textDocument/typeDefinition", "textDocument/definition"};
  if (l == NULL || what < 0 || what > LOC_PEEK) return;
  snprintf(params, sizeof(params),
           "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%lu,\"character\":%lu}%s}",
           l->uri, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x),
           what == LOC_REFS ? ",\"context\":{\"includeDeclaration\":true}" : "");
  request(l->s, method[what], params, RQ_LOC_REFS + what, d);
}


/* workspace/symbol: the names in the whole project like query, to on_workspace_symbols */
void lsp_workspace_symbols (Doc *d, const char *query) {
  LDoc *l = synced(d);
  Buf b;
  if (l == NULL) return;
  buf_init(&b);
  buf_puts(&b, "{\"query\":");
  json_put_str(&b, query, strlen(query));
  buf_putc(&b, '}');
  buf_putc(&b, '\0');
  request(l->s, "workspace/symbol", b.s, RQ_WSYM, d);
  buf_free(&b);
}


/* textDocument/documentHighlight: where the symbol at 'at' is, to on_highlights */
void lsp_highlights (Doc *d, Pos at) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL) return;
  snprintf(params, sizeof(params),
           "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%lu,\"character\":%lu}}",
           l->uri, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x));
  request(l->s, "textDocument/documentHighlight", params, RQ_HIGHLIGHT, d);
  l->s->req[l->s->nreq - 1].at = at;
}


void lsp_comp_resolve (Doc *d, size_t i, const char *json) {
  LDoc *l = synced(d);
  if (l == NULL || json == NULL) return;
  request(l->s, "completionItem/resolve", json, RQ_COMP_RESOLVE, d);
  l->s->req[l->s->nreq - 1].at.x = i;
  l->s->req[l->s->nreq - 1].at.y = g_comp_gen;
}


/* the formatting options, as the file's indent says */
static void fmt_options (Buf *b, const Doc *d) {
  buf_printf(b, "\"options\":{\"tabSize\":%d,\"insertSpaces\":%s,\"trimTrailingWhitespace\":true}",
             d->indent, d->tabs ? "false" : "true");
}


int lsp_format_range (Doc *d, Pos a, Pos b) {
  LDoc *l = synced(d);
  Buf q;
  if (l == NULL || !l->s->can_range) return 0;
  buf_init(&q);
  buf_printf(&q, "{\"textDocument\":{\"uri\":\"%s\"},\"range\":{\"start\":{\"line\":%lu,\"character\":%lu},"
             "\"end\":{\"line\":%lu,\"character\":%lu}},", l->uri, (unsigned long)a.y,
             (unsigned long)col_out(l->s, d, a.y, a.x), (unsigned long)b.y, (unsigned long)col_out(l->s, d, b.y, b.x));
  fmt_options(&q, d);
  buf_putc(&q, '}');
  buf_putc(&q, '\0');
  request(l->s, "textDocument/rangeFormatting", q.s, RQ_FORMAT, d);
  buf_free(&q);
  return 1;
}


const char *lsp_type_chars (const Doc *d) {
  LDoc *l = ldoc(d);
  return l && l->s->ready && !l->s->dead ? l->s->type_chars : "";
}


void lsp_format_type (Doc *d, Pos at, const char *ch) {
  LDoc *l = synced(d);
  Buf q;
  if (l == NULL) return;
  buf_init(&q);
  buf_printf(&q, "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%lu,\"character\":%lu},\"ch\":",
             l->uri, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x));
  json_put_str(&q, ch, strlen(ch));
  buf_putc(&q, ',');
  fmt_options(&q, d);
  buf_putc(&q, '}');
  buf_putc(&q, '\0');
  request(l->s, "textDocument/onTypeFormatting", q.s, RQ_ONTYPE, d);
  l->s->req[l->s->nreq - 1].at.x = (size_t)d->edits;	/* typed on since: the answer is too late */
  buf_free(&q);
}


void lsp_selection_range (Doc *d, Pos at) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL || !l->s->can_sel) {
    on_selection_ranges(d, at, NULL, 0);
    return;
  }
  snprintf(params, sizeof(params),
           "{\"textDocument\":{\"uri\":\"%s\"},\"positions\":[{\"line\":%lu,\"character\":%lu}]}",
           l->uri, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x));
  request(l->s, "textDocument/selectionRange", params, RQ_SELRANGE, d);
  l->s->req[l->s->nreq - 1].at = at;
}


void lsp_folding (Doc *d) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL || !l->s->can_fold) return;
  snprintf(params, sizeof(params), "{\"textDocument\":{\"uri\":\"%s\"}}", l->uri);
  request(l->s, "textDocument/foldingRange", params, RQ_FOLDING, d);
  l->s->req[l->s->nreq - 1].at.x = (size_t)d->edits;
}


/* call / type hierarchy: prepare at 'at', then the calls or the types (what: LOC_CALLS_IN ...) */
int lsp_hierarchy (Doc *d, Pos at, int what) {
  LDoc *l = synced(d);
  char params[1024];
  int types = what == LOC_SUPER || what == LOC_SUB;
  if (l == NULL || (types ? !l->s->can_types : !l->s->can_calls)) return 0;
  snprintf(params, sizeof(params),
           "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%lu,\"character\":%lu}}",
           l->uri, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x));
  request(l->s, types ? "textDocument/prepareTypeHierarchy" : "textDocument/prepareCallHierarchy", params, RQ_HPREP, d);
  l->s->req[l->s->nreq - 1].at.x = (size_t)what;
  return 1;
}


/* the hierarchy's answer: places, to on_locations */
static void hierarchy (Srv *s, const Json *res, int what) {
  Loc *v = NULL;
  size_t n = 0, cap = 0, i, k;
  for (i = 0; res && res->type == J_ARR && i < res->n; i++) {
    const Json *e = res->kid[i];
    const Json *item = what == LOC_CALLS_IN ? json_get(e, "from") : what == LOC_CALLS_OUT ? json_get(e, "to") : e;
    const Json *ranges = what == LOC_CALLS_IN ? json_get(e, "fromRanges") : NULL;
    const char *uri = json_str(json_get(item, "uri"), NULL);
    if (uri == NULL) continue;
    if (ranges && ranges->type == J_ARR && ranges->n) {	/* the calls themselves, in the caller */
      for (k = 0; k < ranges->n; k++) {
        if (n == cap) v = (Loc *)xrealloc(v, (cap = cap ? cap * 2 : 16) * sizeof(Loc));
        loc_fill(s, &v[n++], uri, ranges->kid[k]);
      }
    }
    else {
      const Json *r = json_get(item, "selectionRange") ? json_get(item, "selectionRange") : json_get(item, "range");
      if (r == NULL) continue;
      if (n == cap) v = (Loc *)xrealloc(v, (cap = cap ? cap * 2 : 16) * sizeof(Loc));
      loc_fill(s, &v[n++], uri, r);
    }
  }
  on_locations(what, v, n);
  for (i = 0; i < n; i++) free(v[i].path);
  free(v);
}


void lsp_complete (Doc *d, Pos at) {
  ask(d, at, "textDocument/completion", RQ_COMPLETE);
}


void lsp_define (Doc *d, Pos at) {
  ask(d, at, "textDocument/definition", RQ_DEFINE);
}

/* }================================================================== */


/*
** {==================================================================
** editor.inlineSuggest: textDocument/inlineCompletion (LSP 3.18 draft)
**
** The same request GitHub's Copilot language server, Codeium and
** Supermaven all answer: the server proposes a continuation at the
** cursor, mme draws it dim without putting it in the text. The items
** are kept as they came so the one the user takes can have its command
** run and can be reported back with the notifications Copilot expects
** (other servers drop a notification they do not know).
**
** Only one question is out at a time: asking again cancels the one
** before, both here (its Req is disowned, so a late answer is dropped,
** as a closed document's is) and at the server ($/cancelRequest).
** ===================================================================
*/

static struct {
  Srv *s;
  Doc *d;
  char **json;	/* the items as the server sent them */
  size_t n;
  int id;	/* the question out now; 0: none */
  Srv *asked;	/* who it went to */
  unsigned long edits;	/* the text it was asked about */
  Pos at;	/* and where */
} g_inl;


static const Doc *inline_doc (void) {
  return g_inl.d;
}


static void inline_forget (void) {
  size_t i;
  for (i = 0; i < g_inl.n; i++) free(g_inl.json[i]);
  free(g_inl.json);
  g_inl.json = NULL;
  g_inl.n = 0;
  g_inl.s = NULL;
  g_inl.d = NULL;
}


void lsp_inline_cancel (void) {
  Srv *s = g_inl.asked;
  int i;
  if (g_inl.id == 0 || s == NULL) return;
  for (i = 0; i < s->nreq; i++)	/* the answer is no longer wanted: dropped when it comes */
    if (s->req[i].id == g_inl.id) {
      s->req[i].kind = RQ_OTHER;
      s->req[i].d = NULL;
    }
  if (!s->dead) {
    char params[64];
    snprintf(params, sizeof(params), "{\"id\":%d}", g_inl.id);
    notify(s, "$/cancelRequest", params);
  }
  g_inl.id = 0;
  g_inl.asked = NULL;
}


int lsp_inline_able (const Doc *d) {
  const LDoc *l = ldoc_inline(d);
  return l && l->s->ready && !l->s->dead && l->s->can_inline;
}


void lsp_inline (Doc *d, Pos at, int invoked) {
  LDoc *l = ldoc_inline(d);
  Buf q;
  if (l == NULL || !l->s->ready || l->s->dead) return;
  if (!l->opened) did_open(l);	/* synced(), but for whichever server answers these */
  else if (l->sent != d->edits) did_change(l);
  lsp_inline_cancel();	/* one at a time: the server drops the old one anyway */
  buf_init(&q);
  buf_printf(&q, "{\"textDocument\":{\"uri\":\"%s\",\"version\":%d},"	/* version: Copilot's, harmless elsewhere */
             "\"position\":{\"line\":%lu,\"character\":%lu},"
             "\"context\":{\"triggerKind\":%d},"
             "\"formattingOptions\":{\"tabSize\":%d,\"insertSpaces\":%s}}",
             l->uri, l->version, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x),
             invoked ? 1 : 2, d->indent, d->tabs ? "false" : "true");
  g_inl.id = request(l->s, "textDocument/inlineCompletion", q.s, RQ_INLINE, d);
  g_inl.asked = l->s;
  g_inl.edits = d->edits;
  g_inl.at = at;
  buf_free(&q);
}


/* InlineCompletionItem[] or {"items":[...]}: to on_inline, the raw items kept here */
static void inlines (Srv *s, Doc *d, const Json *res) {
  const Json *items = res;
  InlineItem *v = NULL;
  size_t i, n = 0;
  inline_forget();
  if (items && items->type == J_OBJ) items = json_get(items, "items");
  if (items == NULL || items->type != J_ARR || items->n == 0) {
    on_inline(d, g_inl.edits, g_inl.at, NULL, 0);
    return;
  }
  g_inl.json = (char **)xmalloc(items->n * sizeof(char *));
  v = (InlineItem *)xmalloc(items->n * sizeof(InlineItem));
  for (i = 0; i < items->n; i++) {
    const Json *it = items->kid[i], *ins = json_get(it, "insertText"), *rg = json_get(it, "range");
    const char *txt;
    Buf b;
    int snip = 0;
    if (ins && ins->type == J_OBJ) {	/* {"kind":2,"value":"..."}: a snippet */
      snip = inum(json_get(ins, "kind"), 1) == 2;
      ins = json_get(ins, "value");
    }
    if ((txt = json_str(ins, NULL)) == NULL || txt[0] == '\0') continue;
    v[n].text = xstrdup(txt);
    v[n].snippet = snip;
    v[n].a = rg ? pos_in(s, d, json_get(rg, "start")) : g_inl.at;
    v[n].b = rg ? pos_in(s, d, json_get(rg, "end")) : g_inl.at;
    buf_init(&b);
    json_write(&b, it);
    buf_putc(&b, '\0');
    g_inl.json[n] = buf_take(&b);
    n++;
  }
  g_inl.s = s;
  g_inl.d = d;
  g_inl.n = n;
  on_inline(d, g_inl.edits, g_inl.at, v, n);
}


/* {"item": <the item as it came>, <more>} for the notifications Copilot listens for */
static void inline_notify (size_t i, const char *method, const char *more) {
  Buf b;
  if (i >= g_inl.n || g_inl.s == NULL || g_inl.s->dead) return;
  buf_init(&b);
  buf_printf(&b, "{\"item\":%s%s}", g_inl.json[i], more ? more : "");
  notify(g_inl.s, method, b.s);
  buf_free(&b);
}


void lsp_inline_shown (size_t i) {
  inline_notify(i, "textDocument/didShowCompletion", NULL);
}


void lsp_inline_partial (size_t i, size_t len) {
  char more[48];
  snprintf(more, sizeof(more), ",\"acceptedLength\":%lu", (unsigned long)len);
  inline_notify(i, "textDocument/didPartiallyAcceptCompletion", more);
}


void lsp_inline_accept (size_t i) {
  Json *j;
  const Json *cmd;
  if (i >= g_inl.n || g_inl.s == NULL || g_inl.s->dead) return;
  if ((j = json_parse(g_inl.json[i], strlen(g_inl.json[i]))) == NULL) return;
  if ((cmd = json_get(j, "command")) != NULL && json_get(cmd, "command"))
    run_command(g_inl.s, cmd);	/* workspace/executeCommand, the code actions' path */
  json_free(j);
}

/* }================================================================== */

/*
** {==================================================================
** Next edit suggestions: textDocument/copilotInlineEdit
**
** The same server answers a second question beside the continuation
** at the cursor: given what has just been typed, the edit that change
** calls for somewhere else in the file - the other three uses of the
** variable that was renamed on line 10. It is the same server and the
** same document, so ldoc_inline() picks it, and only one question is
** out at a time exactly as above.
**
** Nothing in the initialize answer announces the method: Copilot's
** server 1.551 advertises inlineCompletionProvider and no more. So it
** is simply asked of whatever answers inlineCompletion, and a server
** that says MethodNotFound is never asked again - which is what makes
** this cost nothing against a server that has never heard of it.
**
** The shapes, confirmed against copilot-language-server 1.551:
**   ->  textDocument/copilotInlineEdit
**       {"textDocument":{"uri":U,"version":N},"position":{"line":L,"character":C}}
**       (it validates: uri, line and character are required, version is
**       an integer, and anything else it does not know is ignored)
**   <-  {"edits":[{"text":T,"range":R,"textDocument":{"uri":U,"version":N},
**                  "cacheTelemetryContext":S,"command":C}]}, and "edits":[]
**       when it has none. The text is "text", not "newText"; the uri comes
**       back the server's own way (file:///c%3A/... for file:///C:/...),
**       which is why uri_same below undoes the escapes before comparing.
**       C names the command to run when it is taken, and which one that is
**       varies - didAcceptCompletionItem from the model, and
**       didAcceptNextEditSuggestionItem from the server's own test hook -
**       so what it names is what is run, never a command chosen here.
**   ->  textDocument/didShowInlineEdit  {"item":<the edit as it came>}
**       (it wants item.command.arguments: exactly one non-empty string)
**   ->  textDocument/reportCachedInlineEdit
**       {"opportunityId":ID,"context":S,"isShown":true,
**        "acceptance":"accepted"|"rejected"}   (all four required, and
**       this one alone refuses a field it does not know; the server takes
**       "notAccepted" there too, which mme has no use for)
** ===================================================================
*/

static struct {
  Srv *s;
  Doc *d;	/* the edit is for this text, as it was */
  char *json;	/* the edit as the server sent it: what the notifications carry */
  char *ctx;	/* its cacheTelemetryContext */
  char *oid;	/* its command's one argument: the id the server knows it by */
  int shown, told;	/* it reached the screen; reportCachedInlineEdit already went */
  int id;	/* the question out now; 0: none */
  Srv *asked;	/* who it went to */
  unsigned long edits;	/* the text it was asked about */
  Pos at;	/* and where the cursor was */
} g_ne;


static const Doc *nedit_doc (void) {
  return g_ne.d;
}


static void nedit_forget (void) {
  free(g_ne.json);
  free(g_ne.ctx);
  free(g_ne.oid);
  g_ne.json = NULL;
  g_ne.ctx = NULL;
  g_ne.oid = NULL;
  g_ne.shown = 0;
  g_ne.told = 0;
  g_ne.s = NULL;
  g_ne.d = NULL;
}


void lsp_nedit_cancel (void) {
  Srv *s = g_ne.asked;
  int i;
  if (g_ne.id == 0 || s == NULL) return;
  for (i = 0; i < s->nreq; i++)	/* the answer is no longer wanted: dropped when it comes */
    if (s->req[i].id == g_ne.id) {
      s->req[i].kind = RQ_OTHER;
      s->req[i].d = NULL;
    }
  if (!s->dead) {
    char params[64];
    snprintf(params, sizeof(params), "{\"id\":%d}", g_ne.id);
    notify(s, "$/cancelRequest", params);
  }
  g_ne.id = 0;
  g_ne.asked = NULL;
}


int lsp_nedit_able (const Doc *d) {
  const LDoc *l = ldoc_inline(d);
  return l && l->s->ready && !l->s->dead && l->s->can_inline && !l->s->no_nedit;
}


void lsp_nedit (Doc *d, Pos at) {
  LDoc *l = ldoc_inline(d);
  Buf q;
  if (l == NULL || !l->s->ready || l->s->dead || l->s->no_nedit) return;
  if (!l->opened) did_open(l);	/* synced(), but for whichever server answers these */
  else if (l->sent != d->edits) did_change(l);
  lsp_nedit_cancel();	/* one at a time, as the continuation is */
  buf_init(&q);
  buf_printf(&q, "{\"textDocument\":{\"uri\":\"%s\",\"version\":%d},"	/* version: it throws without one */
             "\"position\":{\"line\":%lu,\"character\":%lu}}",
             l->uri, l->version, (unsigned long)at.y, (unsigned long)col_out(l->s, d, at.y, at.x));
  g_ne.id = request(l->s, "textDocument/copilotInlineEdit", q.s, RQ_NEDIT, d);
  g_ne.asked = l->s;
  g_ne.edits = d->edits;
  g_ne.at = at;
  buf_free(&q);
}


/*
** Are these two the same file? The server writes the uri its own way -
** copilot-language-server answers file:///c%3A/Users/... for the
** file:///C:/Users/... it was given - so a byte comparison would call
** every edit a cross-file one. The escapes are undone and the case is
** ignored, which is right for the only place this is used.
*/
static int uri_hex (int c) {	/* the value of one hex digit, -1: not one */
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}


static int uri_char (const char **p) {	/* the next character of a uri, %XX undone */
  int c = (unsigned char)*(*p)++, h, l;
  if (c == '%' && (h = uri_hex((unsigned char)(*p)[0])) >= 0 && (l = uri_hex((unsigned char)(*p)[1])) >= 0) {
    c = h * 16 + l;
    *p += 2;
  }
  if (c == '\\') c = '/';	/* a separator is a separator */
  if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return c;
}


static int uri_same (const char *a, const char *b) {
  while (*a && *b)
    if (uri_char(&a) != uri_char(&b)) return 0;
  return *a == '\0' && *b == '\0';
}


/* {"edits":[...]}: the first usable one to on_nedit, the raw edit kept here */
static void nedits (Srv *s, Doc *d, const Json *res) {
  const Json *items = res != NULL ? json_get(res, "edits") : NULL;
  NEditItem *v = NULL;
  size_t i, n = 0;
  nedit_forget();
  if (items == NULL || items->type != J_ARR || items->n == 0) {
    on_nedit(d, g_ne.edits, g_ne.at, NULL, 0);
    return;
  }
  v = (NEditItem *)xmalloc(sizeof(NEditItem));
  for (i = 0; i < items->n && n == 0; i++) {	/* VS Code offers one at a time: the first that is whole */
    const Json *it = items->kid[i], *rg = json_get(it, "range"), *arg;
    const char *txt = json_str(json_get(it, "text"), NULL);
    const char *in = json_str(json_get(it, "textDocument.uri"), NULL);
    const LDoc *l = ldoc_inline(d);
    Buf b;
    if (txt == NULL || rg == NULL) continue;
    /* the server may answer with an edit in another file (it marks those with
    ** targetLine). mme has nowhere to point at one, so it is left alone rather
    ** than applied to the wrong text - the range would mean the wrong lines. */
    if (in != NULL && l != NULL && !uri_same(in, l->uri)) {
      out_log(s->chan, "[info] next edit is in %s, not the file being edited: not shown", in);
      continue;
    }
    v[n].text = xstrdup(txt);
    v[n].a = pos_in(s, d, json_get(rg, "start"));
    v[n].b = pos_in(s, d, json_get(rg, "end"));
    buf_init(&b);
    json_write(&b, it);
    buf_putc(&b, '\0');
    g_ne.json = buf_take(&b);
    g_ne.ctx = xstrdup(json_str(json_get(it, "cacheTelemetryContext"), ""));
    arg = json_get(it, "command.arguments");
    g_ne.oid = xstrdup(arg != NULL && arg->type == J_ARR && arg->n > 0 ? json_str(arg->kid[0], "") : "");
    n++;
  }
  if (n == 0) {
    free(v);
    v = NULL;
  }
  g_ne.s = s;
  g_ne.d = d;
  on_nedit(d, g_ne.edits, g_ne.at, v, n);
}


void lsp_nedit_shown (void) {
  Buf b;
  if (g_ne.json == NULL || g_ne.s == NULL || g_ne.s->dead) return;
  g_ne.shown = 1;
  if (g_ne.oid == NULL || g_ne.oid[0] == '\0') return;	/* no id: nothing the server could count */
  buf_init(&b);
  buf_printf(&b, "{\"item\":%s}", g_ne.json);
  notify(g_ne.s, "textDocument/didShowInlineEdit", b.s);
  buf_free(&b);
}


void lsp_nedit_accept (void) {
  Json *j;
  const Json *cmd;
  if (g_ne.json == NULL || g_ne.s == NULL || g_ne.s->dead) return;
  if ((j = json_parse(g_ne.json, strlen(g_ne.json))) == NULL) return;
  if ((cmd = json_get(j, "command")) != NULL && json_get(cmd, "command") != NULL)
    run_command(g_ne.s, cmd);	/* github.copilot.didAcceptCompletionItem, with the edit's id */
  json_free(j);
}


/*
** The edit is going away: NE_ACCEPTED (it went into the text),
** NE_REJECTED (Esc, or the command) or NE_IGNORED (something else
** happened and it was never answered). Two things go out for it:
**
**  - the command the server keeps for each, confirmed to take exactly
**    one string, the edit's id, and to answer true:
**    github.copilot.did{Accept,Reject,Ignore}NextEditSuggestionItem.
**    The accepted one is the edit's own command instead, because the
**    server names it there itself (didAcceptCompletionItem, with the
**    same id) - what it asked for is what it gets.
**
**  - reportCachedInlineEdit: the edit was answered once and then held
**    here, drawn again over every keystroke that did not change the
**    text, so by the time it ends it has been served out of this cache.
**    It goes once, and only for an edit that reached the screen.
*/
void lsp_nedit_done (int what) {
  static const char *const cmd[] = {"github.copilot.didIgnoreNextEditSuggestionItem", NULL,
                                    "github.copilot.didRejectNextEditSuggestionItem"};
  const char *ctx;
  Buf b;
  if (g_ne.s == NULL || g_ne.s->dead || g_ne.told) return;
  if (g_ne.oid == NULL || g_ne.oid[0] == '\0') return;	/* no id: nothing the server could count */
  g_ne.told = 1;
  if (what >= 0 && what <= NE_REJECTED && cmd[what] != NULL) {
    buf_init(&b);
    buf_printf(&b, "{\"command\":\"%s\",\"arguments\":[", cmd[what]);
    json_put_str(&b, g_ne.oid, strlen(g_ne.oid));
    buf_puts(&b, "]}");
    request(g_ne.s, "workspace/executeCommand", b.s, RQ_OTHER, NULL);
    buf_free(&b);
  }
  if (!g_ne.shown) return;	/* it never reached the screen: there is no cache hit to report */
  ctx = (g_ne.ctx != NULL && g_ne.ctx[0] != '\0') ? g_ne.ctx : "{}";	/* it wants at least one character */
  buf_init(&b);
  buf_puts(&b, "{\"opportunityId\":");
  json_put_str(&b, g_ne.oid, strlen(g_ne.oid));
  buf_puts(&b, ",\"context\":");
  json_put_str(&b, ctx, strlen(ctx));
  buf_printf(&b, ",\"isShown\":true,\"acceptance\":\"%s\"}",
             what == NE_ACCEPTED ? "accepted" : "rejected");
  notify(g_ne.s, "textDocument/reportCachedInlineEdit", b.s);
  buf_free(&b);
}

/* }================================================================== */


/*
** {==================================================================
** GitHub Copilot's account: signIn, signOut, checkStatus
**
** The inline-completion server keeps its own credentials, so the
** editor never sees a token: it asks the server to sign in and the
** server drives GitHub's device flow. signIn answers at once with a
** code and a page to type it in, and names a command
** (github.copilot.finishDeviceFlow) that answers only once the user
** has authorised in the browser - a minute, or never. That command is
** an ordinary async workspace/executeCommand like every other request
** here, so the editor keeps drawing and typing while it is out.
**
** The server also sends didChangeStatus whenever it starts working,
** stops, or cannot: the last one is kept and drawn in the status bar.
** ===================================================================
*/

static struct {
  int kind;	/* CS_*: what didChangeStatus last said */
  int busy, signing, known;
  long long sign_by;	/* us by which the sign-in out now must have finished; 0: none is */
  char user[64], msg[200], code[32], uri[200];
} g_auth;


static void auth_forget (void) {
  memset(&g_auth, 0, sizeof(g_auth));	/* CS_OFF, nobody signed in, nothing out */
}


/* nothing is signing in any more: the next Sign In is let through */
static void auth_stop_signing (void) {
  g_auth.signing = 0;
  g_auth.sign_by = 0;
  g_auth.code[0] = g_auth.uri[0] = '\0';
}


/* the inline-completion server; start_it: bring it up if it is not running. NULL: none is set */
static Srv *inline_srv (int start_it) {
  const char *cmd = settings_server(INLINE_LANG);
  if (cmd == NULL || cmd[0] == '\0') return NULL;
  return start_it ? server(INLINE_LANG) : srv_of(INLINE_LANG);
}


int lsp_inline_configured (void) {
  const char *cmd = settings_server(INLINE_LANG);
  return cmd != NULL && cmd[0] != '\0';
}


void lsp_inline_status (InlineStatus *out) {
  const Srv *s = inline_srv(0);
  memset(out, 0, sizeof(*out));
  out->kind = g_auth.kind;
  out->busy = g_auth.busy;
  out->signing = g_auth.signing;
  out->known = g_auth.known;
  out->ready = s != NULL && s->ready && !s->dead;
  snprintf(out->user, sizeof(out->user), "%s", g_auth.user);
  snprintf(out->msg, sizeof(out->msg), "%s", g_auth.msg);
  snprintf(out->code, sizeof(out->code), "%s", g_auth.code);
  snprintf(out->uri, sizeof(out->uri), "%s", g_auth.uri);
  snprintf(out->chan, sizeof(out->chan), "%s", s != NULL ? s->chan : "");
}


/*
** A {"status":...,"user":...} answer: checkStatus's, signOut's, and the
** one the device-flow command ends with. Only these say who is signed
** in; didChangeStatus says how the server is, which is not the same.
*/
static void auth_account (const Json *res) {
  const char *st = json_str(json_get(res, "status"), NULL);
  const char *user = json_str(json_get(res, "user"), "");
  int in;
  if (st == NULL) return;	/* a server that does not answer these: nothing is claimed */
  g_auth.known = 1;
  in = strcmp(st, "OK") == 0 || strcmp(st, "MaybeOk") == 0 || strcmp(st, "AlreadySignedIn") == 0;
  snprintf(g_auth.user, sizeof(g_auth.user), "%s", in ? user : "");
  if (!in) {
    int no_sub = strcmp(st, "NotAuthorized") == 0;
    g_auth.kind = CS_INACTIVE;
    if (g_auth.msg[0] == '\0' || no_sub)	/* a didChangeStatus that says why keeps saying it */
      snprintf(g_auth.msg, sizeof(g_auth.msg), "%s",
               no_sub ? "This account has no GitHub Copilot subscription" : "Not signed in");
  }
  else if (g_auth.kind == CS_OFF || g_auth.kind == CS_INACTIVE) {	/* a didChangeStatus of its own wins */
    g_auth.kind = CS_NORMAL;
    g_auth.msg[0] = '\0';
  }
}


/* the "kind" and "message" of a status, wherever it came from */
static void auth_kind (Srv *s, const Json *p) {
  const char *kind = json_str(json_get(p, "kind"), NULL);
  const char *m = json_str(json_get(p, "message"), NULL);
  if (kind != NULL) {
    if (strcmp(kind, "Error") == 0) g_auth.kind = CS_ERROR;
    else if (strcmp(kind, "Warning") == 0) g_auth.kind = CS_WARNING;
    else if (strcmp(kind, "Inactive") == 0) g_auth.kind = CS_INACTIVE;
    else g_auth.kind = CS_NORMAL;
  }
  if (m != NULL) snprintf(g_auth.msg, sizeof(g_auth.msg), "%s", m);
  if (m != NULL && m[0] && g_auth.kind >= CS_WARNING)	/* the output keeps what the status bar only hints at */
    out_log(s->chan, "[%s] %s", g_auth.kind == CS_ERROR ? "error" : "warning", m);
}


/* didChangeStatus: {"busy":bool,"kind":"Normal"|"Error"|"Warning"|"Inactive","message":"..."} */
static void auth_change (Srv *s, const Json *p) {
  g_auth.busy = json_bool(json_get(p, "busy"), 0);
  auth_kind(s, p);
}


/*
** didChangeStatus/v2, which Copilot's server 1.551 sends beside the one
** above: the same news split by category, and the account it is signed
** in to comes with it instead of waiting for a checkStatus.
**
**   {"statuses":[{"category":"auth","kind":"Normal","result":{"status":"OK","user":"..."}},
**                {"category":"completion","busy":false},
**                {"category":"cls","kind":"Normal","inactive":false}]}
*/
static void auth_change_v2 (Srv *s, const Json *p) {
  const Json *v = json_get(p, "statuses");
  size_t i;
  if (v == NULL || v->type != J_ARR) return;
  for (i = 0; i < v->n; i++) {
    const Json *e = v->kid[i], *busy = json_get(e, "busy"), *res = json_get(e, "result");
    if (busy != NULL) g_auth.busy = json_bool(busy, 0);
    if (res != NULL) auth_account(res);
    else if (strcmp(json_str(json_get(e, "category"), ""), "completion") != 0) auth_kind(s, e);
    if (json_bool(json_get(e, "inactive"), 0)) g_auth.kind = CS_INACTIVE;
  }
}


void lsp_inline_check (void) {
  Srv *s = inline_srv(1);
  if (s == NULL || s->dead || !s->ready) return;	/* not up yet: initialize's answer asks for it */
  request(s, "checkStatus", "{\"options\":{}}", RQ_CHECK, NULL);
}


/*
** The command signIn named. It answers when the user has authorised in
** the browser, or not at all, so nothing waits for it.
*/
static void auth_device (Srv *s, const Json *cmd) {
  const char *name = json_str(json_get(cmd, "command"), "");
  const Json *args = json_get(cmd, "arguments");
  Buf b;
  buf_init(&b);
  buf_puts(&b, "{\"command\":");
  json_put_str(&b, name, strlen(name));
  buf_puts(&b, ",\"arguments\":");
  if (args != NULL) json_write(&b, args);
  else buf_puts(&b, "[]");
  buf_putc(&b, '}');
  request(s, "workspace/executeCommand", b.s, RQ_DEVICE, NULL);
  buf_free(&b);
}


/* signIn's answer: already in, or the device flow to walk the user through */
static void auth_prompt (Srv *s, const Json *res) {
  const char *st = json_str(json_get(res, "status"), "");
  const char *code = json_str(json_get(res, "userCode"), NULL);
  const char *uri = json_str(json_get(res, "verificationUri"), NULL);
  const Json *cmd = json_get(res, "command");
  double secs;
  if (strcmp(st, "PromptUserDeviceFlow") != 0 || code == NULL || uri == NULL) {
    auth_stop_signing();	/* no flow came of it: Sign In works again at once */
    auth_account(res);
    if (g_auth.user[0]) toast_src(0, s->chan, "Already signed in to GitHub Copilot as %s", g_auth.user);
    else toast_src(1, s->chan, "The server offered no sign-in%s%s", st[0] ? ": " : "", st);
    return;
  }
  g_auth.signing = 1;
  /* the code expires: after that the flow is over whether the server says so
  ** or not, and the next Sign In must not be turned away by this one */
  secs = json_num(json_get(res, "expiresIn"), 900);
  if (!(secs >= 1 && secs <= 86400)) secs = 900;
  g_auth.sign_by = os_now_us() + (long long)secs * 1000000LL;
  snprintf(g_auth.code, sizeof(g_auth.code), "%s", code);
  snprintf(g_auth.uri, sizeof(g_auth.uri), "%s", uri);
  clip_set(code, strlen(code));	/* it is pasted into the page: VS Code copies it too */
  on_show_document(NULL, uri, -1, -1);	/* the browser, by window/showDocument's own path */
  out_log(s->chan, "[info] device flow: paste %s at %s", code, uri);
  toast_src(0, s->chan, "GitHub Copilot: paste the code %s at %s (it is on the clipboard)", code, uri);
  if (cmd != NULL && json_get(cmd, "command") != NULL) auth_device(s, cmd);
  else {	/* nothing to wait for: the server wants to be asked again */
    g_auth.signing = 0;
    g_auth.sign_by = 0;
  }
}


/* the device flow ended: the user authorised, or it timed out */
static void auth_done (Srv *s, const Json *msg) {
  const Json *err = json_get(msg, "error");
  auth_stop_signing();
  if (err != NULL) {
    snprintf(g_auth.msg, sizeof(g_auth.msg), "%s", json_str(json_get(err, "message"), "the sign-in did not finish"));
    g_auth.kind = CS_ERROR;
    out_log(s->chan, "[error] sign-in: %s", g_auth.msg);
    toast_src(2, s->chan, "GitHub Copilot sign-in failed: %s", g_auth.msg);
    return;
  }
  auth_account(json_get(msg, "result"));
  if (g_auth.user[0]) toast_src(0, s->chan, "Signed in to GitHub Copilot as %s", g_auth.user);
  else toast_src(1, s->chan, "GitHub Copilot sign-in did not finish: %s",
                 json_str(json_get(msg, "result.status"), "no status"));
}


void lsp_inline_signin (void) {
  Srv *s = inline_srv(1);
  if (s == NULL) {
    toast(0, "No inline-completion server is set in mme.inlineCompletionServer.");
    return;
  }
  if (g_auth.signing) {
    if (g_auth.code[0]) toast(0, "A sign-in is already running: paste %s at %s", g_auth.code, g_auth.uri);
    else toast(0, "A sign-in is already running.");
    return;
  }
  if (s->dead || !s->ready) {
    toast(0, "The inline-completion server is still starting.");
    return;
  }
  /* from here, not from the answer: signIn takes a moment, and two of them
  ** start two device flows with two codes, of which only one is on screen */
  g_auth.signing = 1;
  g_auth.sign_by = os_now_us() + 60000000LL;	/* until signIn itself has answered */
  g_auth.code[0] = g_auth.uri[0] = '\0';
  request(s, "signIn", "{}", RQ_SIGNIN, NULL);
}


/*
** A device flow is a request like any other, and the server may simply
** never answer it: GitHub's own command waits for a browser that may never
** come back. Nothing but this would ever clear g_auth.signing then, and
** every later Sign In would be turned away with "a sign-in is already
** running" for as long as the editor ran. It is over when the code the
** server named has expired, or when the server it went to is gone.
*/
static void auth_idle (void) {
  const Srv *s;
  if (!g_auth.signing) return;
  s = srv_of(INLINE_LANG);
  if (s != NULL && !s->dead && (g_auth.sign_by == 0 || os_now_us() < g_auth.sign_by)) return;
  if (s != NULL) out_log(s->chan, "[warning] the sign-in did not finish: it can be started again");
  auth_stop_signing();
}


void lsp_inline_signout (void) {
  Srv *s = inline_srv(0);
  if (s == NULL || s->dead || !s->ready) {
    toast(0, "The inline-completion server is not running.");
    return;
  }
  request(s, "signOut", "{}", RQ_SIGNOUT, NULL);
}

/* }================================================================== */


/*
** {==================================================================
** Diagnostics
** ===================================================================
*/

static DFile *dfile (const char *uri, int make) {
  size_t i;
  for (i = 0; i < g_ndiag; i++)
    if (strcmp(g_diag[i].uri, uri) == 0) return &g_diag[i];
  if (!make) return NULL;
  if (g_ndiag == g_capdiag) {
    g_capdiag = g_capdiag ? g_capdiag * 2 : 16;
    g_diag = (DFile *)xrealloc(g_diag, g_capdiag * sizeof(DFile));
  }
  memset(&g_diag[g_ndiag], 0, sizeof(DFile));
  g_diag[g_ndiag].uri = xstrdup(uri);
  return &g_diag[g_ndiag++];
}


/* the server's problems of uri go (a restarted server says them again); a task's stay */
static void diag_drop (const char *uri) {
  DFile *f = dfile(uri, 0);
  size_t i, ns;
  if (f == NULL) return;
  ns = f->n - f->nt;
  for (i = 0; i < ns; i++) free(f->v[i].msg);
  memmove(f->v, f->v + ns, f->nt * sizeof(Diag));
  f->n = f->nt;
}


/* the server's problems of uri, pushed or pulled, take the place of its last ones */
static void diag_set (Srv *s, const char *uri, const Json *list) {
  DFile *f;
  const LDoc *l = NULL;
  size_t i;
  if (uri == NULL || list == NULL || list->type != J_ARR) return;
  f = dfile(uri, 1);
  {	/* the server's go, the task's stay after the new ones */
    Diag *old = f->v;
    size_t ns = f->n - f->nt;
    for (i = 0; i < ns; i++) free(old[i].msg);
    f->v = (Diag *)xmalloc((list->n + f->nt + 1) * sizeof(Diag));
    if (f->nt) memcpy(f->v + list->n, old + ns, f->nt * sizeof(Diag));
    free(old);
  }
  f->n = 0;
  for (i = 0; i < g_ndoc; i++)
    if (strcmp(g_doc[i].uri, uri) == 0) l = &g_doc[i];
  for (i = 0; i < list->n; i++) {
    const Json *dg = list->kid[i];
    Diag *v = &f->v[f->n++];
    const Json *st = json_get(dg, "range.start"), *en = json_get(dg, "range.end");
    v->sev = inum(json_get(dg, "severity"), 1);
    v->msg = xstrdup(json_str(json_get(dg, "message"), ""));
    if (l) {
      v->a = pos_in(s, l->d, st);
      v->b = pos_in(s, l->d, en);
    }
    else {
      v->a.y = unum(json_get(st, "line"), 0);
      v->a.x = unum(json_get(st, "character"), 0);
      v->b.y = unum(json_get(en, "line"), 0);
      v->b.x = unum(json_get(en, "character"), 0);
    }
  }
  if (f->nt) memmove(f->v + f->n, f->v + list->n, f->nt * sizeof(Diag));	/* the task's, after */
  f->n += f->nt;
}


/* textDocument/publishDiagnostics: the server says them by itself */
static void diagnostics (Srv *s, const Json *params) {
  diag_set(s, json_str(json_get(params, "uri"), NULL), json_get(params, "diagnostics"));
}


/* one file's report of a pull: "full" takes the place of its problems, "unchanged" keeps them */
static void pull_report (Srv *s, const char *uri, const Json *rep) {
  const char *id = json_str(json_get(rep, "resultId"), NULL);
  size_t i;
  if (strcmp(json_str(json_get(rep, "kind"), ""), "full") == 0) diag_set(s, uri, json_get(rep, "items"));
  for (i = 0; i < g_ndoc; i++)
    if (g_doc[i].s == s && strcmp(g_doc[i].uri, uri) == 0) {
      free(g_doc[i].result_id);
      g_doc[i].result_id = id ? xstrdup(id) : NULL;
    }
}


/* textDocument/diagnostic's answer: the file's report, and those of the files it affects */
static void pulled (Srv *s, const Doc *d, const Json *msg) {
  const Json *res = json_get(msg, "result"), *rel = json_get(res, "relatedDocuments");
  size_t i;
  for (i = 0; i < g_ndoc; i++)
    if (g_doc[i].d == d && g_doc[i].s == s) break;
  if (i == g_ndoc) return;
  if (json_get(msg, "error")) {	/* ServerCancelled: asked again in a while, unless it says not to */
    if (inum(json_get(msg, "error.code"), 0) == -32802 && json_bool(json_get(msg, "error.data.retriggerRequest"), 1)) {
      g_doc[i].repull = 1;
      g_doc[i].repull_at = os_now_us();
    }
    return;
  }
  if (res == NULL || res->type != J_OBJ) return;
  pull_report(s, g_doc[i].uri, res);
  for (i = 0; rel && rel->type == J_OBJ && i < rel->n; i++) pull_report(s, rel->kid[i]->key, rel->kid[i]);
}


static char *path_uri (const char *native);

/* a task's problems in a file take the place of its last ones */
void lsp_task_diags (const char *path, const Diag *v, size_t n) {
  char *uri = path_uri(path);
  DFile *f = dfile(uri, 1);
  size_t i;
  free(uri);
  for (i = f->n - f->nt; i < f->n; i++) free(f->v[i].msg);
  f->n -= f->nt;
  f->v = (Diag *)xrealloc(f->v, (f->n + n + 1) * sizeof(Diag));
  for (i = 0; i < n; i++) {
    f->v[f->n + i] = v[i];
    f->v[f->n + i].msg = xstrdup(v[i].msg);
  }
  f->n += n;
  f->nt = n;
}


void lsp_task_clear (void) {
  size_t k, i;
  for (k = 0; k < g_ndiag; k++) {
    DFile *f = &g_diag[k];
    for (i = f->n - f->nt; i < f->n; i++) free(f->v[i].msg);
    f->n -= f->nt;
    f->nt = 0;
  }
}


/* the uri of a file, the last one asked kept (it is asked for every line drawn) */
static char *path_uri (const char *native) {
  static char *last_path, *last_uri;
  if (last_path == NULL || strcmp(last_path, native) != 0) {
    free(last_path);
    free(last_uri);
    last_path = xstrdup(native);
    last_uri = to_uri(native);
  }
  return xstrdup(last_uri);
}


const Diag *lsp_diags (const Doc *d, size_t *n) {
  const LDoc *l = ldoc(d);
  const DFile *f = l ? dfile(l->uri, 0) : NULL;
  if (l == NULL && d->path && g_ndiag > 0) {	/* no server: a task's problems */
    char *uri = path_uri(d->path);
    f = dfile(uri, 0);
    free(uri);
  }
  *n = f ? f->n : 0;
  return f ? f->v : NULL;
}


size_t lsp_diag_files (void) {
  return g_ndiag;
}


/* the problems of file i; *path: malloc'd */
const Diag *lsp_diag_file (size_t i, char **path, size_t *n) {
  *path = lsp_path(g_diag[i].uri);
  *n = g_diag[i].n;
  return g_diag[i].v;
}


void lsp_counts (int *errors, int *warnings) {
  size_t i, j;
  *errors = *warnings = 0;
  for (i = 0; i < g_ndiag; i++)
    for (j = 0; j < g_diag[i].n; j++) {
      if (g_diag[i].v[j].sev == 1) (*errors)++;
      else if (g_diag[i].v[j].sev == 2) (*warnings)++;
    }
}

/* }================================================================== */


/*
** {==================================================================
** Colors and links: editor.colorDecorators, editor.links
** ===================================================================
*/

static void one_edit (Srv *s, const Json *e, TextEdit *v);

/* the document links: kept as the server sent them, for documentLink/resolve */
static struct {
  Srv *s;
  const Doc *d;
  char **json;
  size_t n;
} g_link;


void lsp_colors (Doc *d) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL || !l->s->can_color) return;
  snprintf(params, sizeof(params), "{\"textDocument\":{\"uri\":\"%s\"}}", l->uri);
  request(l->s, "textDocument/documentColor", params, RQ_COLOR, d);
  l->s->req[l->s->nreq - 1].at.x = (size_t)d->edits;
}


/* ColorInformation[]: where each color is, and what it is */
static void colors (Srv *s, Doc *d, unsigned long edits, const Json *res) {
  static const char *const ch[] = {"color.red", "color.green", "color.blue", "color.alpha"};
  DocColor *v = NULL;
  size_t k, n = 0;
  int q;
  if (res && res->type == J_ARR && res->n) v = (DocColor *)xmalloc(res->n * sizeof(DocColor));
  for (k = 0; v && k < res->n; k++) {
    v[n].a = pos_in(s, d, json_get(res->kid[k], "range.start"));
    v[n].b = pos_in(s, d, json_get(res->kid[k], "range.end"));
    for (q = 0; q < 4; q++) {
      double c = json_num(json_get(res->kid[k], ch[q]), 1);
      v[n].rgba[q] = c < 0 ? 0 : c > 1 ? 1 : c;
    }
    if (v[n].a.y < d->n) n++;
  }
  on_colors(d, edits, v, n);
}


/* the ways the server writes color c, which stands in the text where it says */
void lsp_color_pres (Doc *d, const DocColor *c) {
  LDoc *l = synced(d);
  Buf b;
  if (l == NULL || !l->s->can_color) return;
  buf_init(&b);
  buf_printf(&b, "{\"textDocument\":{\"uri\":\"%s\"},\"color\":{\"red\":%.6g,\"green\":%.6g,\"blue\":%.6g,"
             "\"alpha\":%.6g},\"range\":{\"start\":{\"line\":%lu,\"character\":%lu},"
             "\"end\":{\"line\":%lu,\"character\":%lu}}}", l->uri, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3],
             (unsigned long)c->a.y, (unsigned long)col_out(l->s, d, c->a.y, c->a.x),
             (unsigned long)c->b.y, (unsigned long)col_out(l->s, d, c->b.y, c->b.x));
  request(l->s, "textDocument/colorPresentation", b.s, RQ_COLOR_PRES, d);
  buf_free(&b);
}


/* ColorPresentation[]: a label, and the edits that write it (its textEdit first) */
static void color_pres (Srv *s, Doc *d, const Json *res) {
  ColorPres *v = NULL;
  size_t k, n = 0;
  if (res && res->type == J_ARR && res->n) v = (ColorPres *)xmalloc(res->n * sizeof(ColorPres));
  for (k = 0; v && k < res->n; k++) {
    const Json *p = res->kid[k], *te = json_get(p, "textEdit"), *more = json_get(p, "additionalTextEdits");
    size_t q, nm = more && more->type == J_ARR ? more->n : 0;
    v[n].label = xstrdup(json_str(json_get(p, "label"), "?"));
    v[n].edit = (TextEdit *)xmalloc((nm + 1) * sizeof(TextEdit));
    v[n].n = 0;
    if (te) one_edit(s, te, &v[n].edit[v[n].n++]);
    for (q = 0; q < nm && te; q++) one_edit(s, more->kid[q], &v[n].edit[v[n].n++]);	/* with no textEdit mme writes the label */
    n++;
  }
  on_color_pres(d, v, n);
}


void lsp_links (Doc *d) {
  LDoc *l = synced(d);
  char params[1024];
  if (l == NULL || !l->s->can_link) return;
  snprintf(params, sizeof(params), "{\"textDocument\":{\"uri\":\"%s\"}}", l->uri);
  request(l->s, "textDocument/documentLink", params, RQ_LINK, d);
  l->s->req[l->s->nreq - 1].at.x = (size_t)d->edits;
}


/* DocumentLink[]: the ranges to mme, the links kept here to be opened */
static void links (Srv *s, Doc *d, unsigned long edits, const Json *res) {
  DocLink *v = NULL;
  size_t k, n = 0;
  for (k = 0; k < g_link.n; k++) free(g_link.json[k]);
  free(g_link.json);
  g_link.json = NULL;
  g_link.n = 0;
  g_link.s = s;
  g_link.d = d;
  if (res && res->type == J_ARR && res->n) {
    v = (DocLink *)xmalloc(res->n * sizeof(DocLink));
    g_link.json = (char **)xmalloc(res->n * sizeof(char *));
  }
  for (k = 0; v && k < res->n; k++) {
    const char *tip = json_str(json_get(res->kid[k], "tooltip"), NULL);
    v[n].a = pos_in(s, d, json_get(res->kid[k], "range.start"));
    v[n].b = pos_in(s, d, json_get(res->kid[k], "range.end"));
    v[n].tip = tip ? xstrdup(tip) : NULL;
    g_link.json[n] = json_text(res->kid[k]);
    n++;
  }
  g_link.n = n;
  on_links(d, edits, v, n);
}


/* a link's target: a file (at the line its fragment says, #L10 or #L10,5) or a URL in the browser */
static void link_follow (const char *target) {
  char *uri, *hash, *path;
  long line = -1, col = -1;
  if (strncmp(target, "file://", 7) != 0) {
    on_show_document(NULL, target, -1, -1);
    return;
  }
  uri = xstrdup(target);
  if ((hash = strchr(uri, '#')) != NULL) {
    char *e;
    *hash++ = '\0';
    if (*hash == 'L') hash++;
    line = strtol(hash, &e, 10) - 1;
    if (e != hash && (*e == ',' || *e == ':')) col = strtol(e + 1, NULL, 10) - 1;
    if (e == hash) line = -1;
  }
  path = lsp_path(uri);
  on_show_document(path, NULL, line, col);
  free(path);
  free(uri);
}


void lsp_link_open (Doc *d, size_t i) {
  Json *lk;
  const char *target;
  if (g_link.d != d || i >= g_link.n || g_link.s == NULL || g_link.s->dead) return;
  lk = json_parse(g_link.json[i], strlen(g_link.json[i]));
  target = json_str(json_get(lk, "target"), NULL);
  if (target) link_follow(target);
  else if (g_link.s->link_resolve) request(g_link.s, "documentLink/resolve", g_link.json[i], RQ_LINK_RESOLVE, d);
  else toast(0, "Failed to open this link because it has no target");
  json_free(lk);
}


/* a file that is closing: its links go with it */
static void link_forget (const Doc *d) {
  if (g_link.d == d) g_link.d = NULL;
}

/* }================================================================== */


/*
** {==================================================================
** Answers
** ===================================================================
*/

unsigned lsp_comp_gen (void) {
  return g_comp_gen;
}


/* one TextEdit as mme's (no path) */
static void one_edit (Srv *s, const Json *e, TextEdit *v) {
  const Json *st = json_get(e, "range.start"), *en = json_get(e, "range.end");
  v->path = NULL;
  v->l0 = unum(json_get(st, "line"), 0);
  v->c0 = unum(json_get(st, "character"), 0);
  v->l1 = unum(json_get(en, "line"), 0);
  v->c1 = unum(json_get(en, "character"), 0);
  v->text = xstrdup(json_str(json_get(e, "newText"), ""));
  v->utf16 = !s->utf8;
}


/* TextEdit[] as mme's (no path), NULL: none */
static TextEdit *text_edits (Srv *s, const Json *res, size_t *n) {
  TextEdit *v = NULL;
  size_t j;
  *n = 0;
  if (res == NULL || res->type != J_ARR || res->n == 0) return NULL;
  v = (TextEdit *)xmalloc(res->n * sizeof(TextEdit));
  for (j = 0; j < res->n; j++) one_edit(s, res->kid[j], &v[(*n)++]);
  return v;
}


/* documentation: a string or MarkupContent; NULL none */
static char *doc_text_of (const Json *doc) {
  const char *t = doc == NULL ? NULL : doc->type == J_STR ? doc->str : json_str(json_get(doc, "value"), NULL);
  return t && *t ? xstrdup(t) : NULL;
}


static void completion (Srv *s, Doc *d, const Json *res) {
  const Json *items = (res && res->type == J_ARR) ? res : json_get(res, "items");
  CompItem *v;
  size_t i, n = 0;
  g_comp_gen++;
  if (items == NULL || items->type != J_ARR) {
    on_completion(d, NULL, 0);
    return;
  }
  v = (CompItem *)xmalloc((items->n + 1) * sizeof(CompItem));
  for (i = 0; i < items->n; i++) {
    const Json *it = items->kid[i], *te = json_get(it, "textEdit");
    const char *label = json_str(json_get(it, "label"), NULL);
    const char *ins = json_str(json_get(it, "insertText"), NULL);
    CompItem *c;
    if (label == NULL) continue;
    c = &v[n++];
    memset(c, 0, sizeof(*c));
    c->label = xstrdup(label);
    c->detail = xstrdup(json_str(json_get(it, "detail"), ""));
    c->filter = xstrdup(json_str(json_get(it, "filterText"), label));
    c->sort = xstrdup(json_str(json_get(it, "sortText"), label));
    c->kind = inum(json_get(it, "kind"), 1);
    if (te) {	/* TextEdit, or InsertReplaceEdit: its insert range */
      const Json *range = json_get(te, "range") ? json_get(te, "range") : json_get(te, "insert");
      ins = json_str(json_get(te, "newText"), ins);
      if (range) {
        c->has_range = 1;
        c->a = pos_in(s, d, json_get(range, "start"));
        c->b = pos_in(s, d, json_get(range, "end"));
      }
    }
    c->snippet = json_num(json_get(it, "insertTextFormat"), 1) == 2;	/* mme plays its tab stops */
    c->insert = xstrdup(ins ? ins : label);
    c->doc = doc_text_of(json_get(it, "documentation"));
    c->extra = text_edits(s, json_get(it, "additionalTextEdits"), &c->nextra);
    if (s->can_resolve && i < 2000) {	/* its documentation and imports may come later */
      Buf b;
      buf_init(&b);
      json_write(&b, it);
      buf_putc(&b, '\0');
      c->json = buf_take(&b);
    }
  }
  on_completion(d, v, n);
}


static void definition (Srv *s, const Json *res) {
  const Json *loc = res;
  const char *uri;
  const Json *range;
  if (loc && loc->type == J_ARR) loc = loc->n ? loc->kid[0] : NULL;
  if (loc == NULL || loc->type != J_OBJ) {
    toast(1, "No definition found");
    return;
  }
  uri = json_str(json_get(loc, "uri"), json_str(json_get(loc, "targetUri"), NULL));
  range = json_get(loc, "range") ? json_get(loc, "range") : json_get(loc, "targetSelectionRange");
  if (uri && range) {
    char *path = lsp_path(uri);
    Pos p;
    size_t i;
    const Doc *d = NULL;
    for (i = 0; i < g_ndoc; i++)	/* the column in bytes, when the file is open */
      if (strcmp(g_doc[i].uri, uri) == 0) d = g_doc[i].d;
    p.y = unum(json_get(range, "start.line"), 0);
    p.x = unum(json_get(range, "start.character"), 0);
    if (d) p.x = col_in(s, d, p.y, p.x);
    on_definition(path, p, !s->utf8 && d == NULL);
    free(path);
  }
}


/* the open file with this uri, NULL: none (its columns are then the server's) */
static const Doc *uri_doc (const char *uri) {
  size_t i;
  for (i = 0; i < g_ndoc; i++)
    if (strcmp(g_doc[i].uri, uri) == 0) return g_doc[i].d;
  return NULL;
}


static void loc_fill (Srv *s, Loc *v, const char *uri, const Json *range) {
  const Doc *d = uri_doc(uri);
  v->path = lsp_path(uri);
  v->a.y = unum(json_get(range, "start.line"), 0);
  v->a.x = unum(json_get(range, "start.character"), 0);
  v->b.y = unum(json_get(range, "end.line"), (double)v->a.y);
  v->b.x = unum(json_get(range, "end.character"), (double)v->a.x);
  v->utf16 = 0;
  if (d) {
    v->a.x = col_in(s, d, v->a.y, v->a.x);
    v->b.x = col_in(s, d, v->b.y, v->b.x);
  }
  else v->utf16 = !s->utf8;
}


/* Location, Location[] or LocationLink[]: the places, to on_locations */
static void locations (Srv *s, const Json *res, int what) {
  Loc *v;
  size_t n = 0, i, cnt;
  if (res == NULL || (res->type != J_ARR && res->type != J_OBJ)) {
    on_locations(what, NULL, 0);
    return;
  }
  cnt = res->type == J_ARR ? res->n : 1;
  v = (Loc *)xmalloc((cnt + 1) * sizeof(Loc));
  for (i = 0; i < cnt; i++) {
    const Json *l = res->type == J_ARR ? res->kid[i] : res;
    const char *uri = json_str(json_get(l, "uri"), json_str(json_get(l, "targetUri"), NULL));
    const Json *range = json_get(l, "range") ? json_get(l, "range") : json_get(l, "targetSelectionRange");
    if (uri == NULL || range == NULL) continue;
    loc_fill(s, &v[n++], uri, range);
  }
  on_locations(what, v, n);
  for (i = 0; i < n; i++) free(v[i].path);
  free(v);
}


/* SymbolInformation[] or WorkspaceSymbol[]: to on_workspace_symbols */
static void workspace_symbols (Srv *s, const Json *res) {
  WSym *v;
  size_t n = 0, i;
  if (res == NULL || res->type != J_ARR) {
    on_workspace_symbols(NULL, 0);
    return;
  }
  v = (WSym *)xmalloc((res->n + 1) * sizeof(WSym));
  for (i = 0; i < res->n; i++) {
    const Json *e = res->kid[i];
    const char *uri = json_str(json_get(e, "location.uri"), NULL);
    const Json *range = json_get(e, "location.range");
    Loc l;
    if (uri == NULL) continue;
    if (range) loc_fill(s, &l, uri, range);
    else {
      l.path = lsp_path(uri);
      l.a.y = l.a.x = 0;
      l.utf16 = 0;
    }
    v[n].name = xstrdup(json_str(json_get(e, "name"), "?"));
    v[n].detail = xstrdup(json_str(json_get(e, "containerName"), ""));
    v[n].kind = inum(json_get(e, "kind"), 12);
    v[n].path = l.path;
    v[n].p = l.a;
    v[n].utf16 = l.utf16;
    n++;
  }
  on_workspace_symbols(v, n);
  for (i = 0; i < n; i++) {
    free(v[i].name);
    free(v[i].detail);
    free(v[i].path);
  }
  free(v);
}


/* DocumentHighlight[]: the ranges, pairs a, b */
static void highlights (Srv *s, Doc *d, Pos at, const Json *res) {
  Pos *v;
  size_t n = 0, i;
  if (res == NULL || res->type != J_ARR) {
    on_highlights(d, at, NULL, 0);
    return;
  }
  v = (Pos *)xmalloc((res->n * 2 + 1) * sizeof(Pos));
  for (i = 0; i < res->n; i++) {
    const Json *st = json_get(res->kid[i], "range.start"), *en = json_get(res->kid[i], "range.end");
    if (st == NULL || en == NULL) continue;
    v[n * 2] = pos_in(s, d, st);
    v[n * 2 + 1] = pos_in(s, d, en);
    n++;
  }
  on_highlights(d, at, v, n);
  free(v);
}


/* DocumentSymbol (with children) or SymbolInformation: a flat list with depths */
static void add_symbols (const Json *list, int depth, Sym **v, size_t *n, size_t *cap) {
  size_t i;
  for (i = 0; list && list->type == J_ARR && i < list->n; i++) {
    const Json *s = list->kid[i];
    const Json *range = json_get(s, "range") ? json_get(s, "range") : json_get(s, "location.range");
    Sym *y;
    if (range == NULL) continue;
    if (*n == *cap) {
      *cap = *cap ? *cap * 2 : 64;
      *v = (Sym *)xrealloc(*v, *cap * sizeof(Sym));
    }
    y = &(*v)[(*n)++];
    y->name = xstrdup(json_str(json_get(s, "name"), "?"));
    y->kind = inum(json_get(s, "kind"), 12);
    y->line = unum(json_get(range, "start.line"), 0);
    y->end = unum(json_get(range, "end.line"), (double)y->line);
    y->depth = depth + (json_get(s, "containerName") && !json_get(s, "children") ? 1 : 0);
    add_symbols(json_get(s, "children"), depth + 1, v, n, cap);
  }
}


static int g_confirm;	/* the edit is a rename's: on_edit_confirm */

/* a WorkspaceEdit: the edits of each file, handed to on_edit */
static void workspace_edit (Srv *s, const Json *we) {
  const Json *changes = json_get(we, "changes"), *dc = json_get(we, "documentChanges");
  TextEdit *v = NULL;
  size_t n = 0, cap = 0, i, j;
#define ADD_EDITS(uri, list)	do { \
    for (j = 0; list && j < list->n; j++) { \
      const Json *e = list->kid[j], *st = json_get(e, "range.start"), *en = json_get(e, "range.end"); \
      if (n == cap) { cap = cap ? cap * 2 : 16; v = (TextEdit *)xrealloc(v, cap * sizeof(TextEdit)); } \
      v[n].path = lsp_path(uri); \
      v[n].l0 = unum(json_get(st, "line"), 0); \
      v[n].c0 = unum(json_get(st, "character"), 0); \
      v[n].l1 = unum(json_get(en, "line"), 0); \
      v[n].c1 = unum(json_get(en, "character"), 0); \
      v[n].text = xstrdup(json_str(json_get(e, "newText"), "")); \
      v[n].utf16 = !s->utf8; \
      n++; \
    } } while (0)
  if (dc && dc->type == J_ARR) {
    for (i = 0; i < dc->n; i++) {
      const char *uri = json_str(json_get(dc->kid[i], "textDocument.uri"), NULL);
      const Json *edits = json_get(dc->kid[i], "edits");
      if (uri) ADD_EDITS(uri, edits);
    }
  }
  else if (changes && changes->type == J_OBJ) {
    for (i = 0; i < changes->n; i++) ADD_EDITS(changes->kid[i]->key, changes->kid[i]);
  }
#undef ADD_EDITS
  if (g_confirm) on_edit_confirm(v, n);
  else on_edit(v, n);
  for (i = 0; i < n; i++) {
    free(v[i].path);
    free(v[i].text);
  }
  free(v);
}


/* MarkupContent, a MarkedString, or a list of them: the text to show */
static void hover (const Json *res) {
  const Json *c = json_get(res, "contents");
  Buf b;
  size_t i, n;
  if (c == NULL || c->type == J_NULL) {
    on_hover(NULL);
    return;
  }
  buf_init(&b);
  n = c->type == J_ARR ? c->n : 1;
  for (i = 0; i < n; i++) {
    const Json *part = c->type == J_ARR ? c->kid[i] : c;
    const char *lang = json_str(json_get(part, "language"), NULL);
    const char *text = part->type == J_STR ? part->str : json_str(json_get(part, "value"), "");
    if (b.len) buf_puts(&b, "\n---\n");
    if (lang) buf_printf(&b, "```%s\n%s\n```", lang, text);
    else buf_puts(&b, text);
  }
  buf_putc(&b, '\0');
  on_hover(b.s);
  buf_free(&b);
}


/* MarkupContent or a string: its text (malloc'd), "" when none */
static char *markup (const Json *j) {
  if (j == NULL) return xstrdup("");
  if (j->type == J_STR) return xstrdup(j->str);
  return xstrdup(json_str(json_get(j, "value"), ""));
}


/* SignatureHelp: every overload, its active parameter as a range of its label and that parameter's doc */
static void signature (const Json *res) {
  const Json *sigs = json_get(res, "signatures");
  int as = inum(json_get(res, "activeSignature"), 0), k, n;
  SigInfo *v;
  if (sigs == NULL || sigs->type != J_ARR || sigs->n == 0) {
    on_signatures(NULL, 0, 0);
    return;
  }
  if (as < 0 || (size_t)as >= sigs->n) as = 0;
  n = sigs->n > 32 ? 32 : (int)sigs->n;
  v = (SigInfo *)xmalloc((size_t)n * sizeof(SigInfo));
  for (k = 0; k < n; k++) {
    const Json *sg = sigs->kid[k], *params = json_get(sg, "parameters");
    const char *label = json_str(json_get(sg, "label"), "");
    int ap = inum(json_get(sg, "activeParameter"), json_num(json_get(res, "activeParameter"), 0));
    SigInfo *si = &v[k];
    si->label = xstrdup(label);
    si->a0 = si->a1 = 0;
    si->doc = markup(json_get(sg, "documentation"));
    si->pdoc = xstrdup("");
    if (params && params->type == J_ARR && ap >= 0 && (size_t)ap < params->n) {
      const Json *pl = json_get(params->kid[ap], "label");
      free(si->pdoc);
      si->pdoc = markup(json_get(params->kid[ap], "documentation"));
      if (pl && pl->type == J_STR) {	/* the parameter's text: where it is in the label */
        const char *f = strstr(label, pl->str);
        if (f) {
          si->a0 = (size_t)(f - label);
          si->a1 = si->a0 + pl->len;
        }
      }
      else if (pl && pl->type == J_ARR && pl->n == 2) {	/* [start, end]: UTF-16 units */
        size_t u0 = unum(pl->kid[0], 0), u1 = unum(pl->kid[1], 0), i = 0, u = 0, len;
        size_t ln = strlen(label);
        while (i < ln && u < u0) {
          u += utf8_decode(label + i, ln - i, &len) >= 0x10000 ? 2 : 1;
          i += len;
        }
        si->a0 = i;
        while (i < ln && u < u1) {
          u += utf8_decode(label + i, ln - i, &len) >= 0x10000 ? 2 : 1;
          i += len;
        }
        si->a1 = i;
      }
    }
  }
  on_signatures(v, n, as < n ? as : 0);
}


static void actions (Srv *s, const Json *res) {
  size_t i;
  char **titles;
  for (i = 0; i < g_act.n; i++) free(g_act.json[i]);
  free(g_act.json);
  g_act.json = NULL;
  g_act.n = 0;
  g_act.s = s;
  if (res == NULL || res->type != J_ARR || res->n == 0) {
    on_actions(NULL, 0);
    return;
  }
  g_act.json = (char **)xmalloc(res->n * sizeof(char *));
  titles = (char **)xmalloc(res->n * sizeof(char *));
  for (i = 0; i < res->n; i++) {
    Buf b;
    buf_init(&b);
    json_write(&b, res->kid[i]);
    buf_putc(&b, '\0');
    g_act.json[i] = buf_take(&b);
    titles[i] = (char *)json_str(json_get(res->kid[i], "title"), "?");
  }
  g_act.n = res->n;
  on_actions((const char *const *)titles, res->n);
  free(titles);
}


/* a Command: the server does it (and may send back a workspace/applyEdit) */
static void run_command (Srv *s, const Json *cmd) {
  Buf b;
  const Json *args = json_get(cmd, "arguments");
  buf_init(&b);
  buf_puts(&b, "{\"command\":");
  json_put_str(&b, json_str(json_get(cmd, "command"), ""), strlen(json_str(json_get(cmd, "command"), "")));
  if (args) {
    buf_puts(&b, ",\"arguments\":");
    json_write(&b, args);
  }
  buf_putc(&b, '}');
  request(s, "workspace/executeCommand", b.s, RQ_OTHER, NULL);
  buf_free(&b);
}


/* a CodeAction (or a bare Command) is done: its edit, then its command */
static void do_action (Srv *s, const Json *a) {
  const Json *edit = json_get(a, "edit"), *cmd = json_get(a, "command");
  if (edit) workspace_edit(s, edit);
  if (cmd && cmd->type == J_STR) run_command(s, a);	/* a Command itself */
  else if (cmd && cmd->type == J_OBJ) run_command(s, cmd);
}


void lsp_action_run (size_t i) {
  Json *a;
  if (i >= g_act.n || g_act.s == NULL || g_act.s->dead) return;
  a = json_parse(g_act.json[i], strlen(g_act.json[i]));
  if (a == NULL) return;
  if (!json_get(a, "edit") && !json_get(a, "command") && json_get(a, "data"))	/* the server fills it in */
    request(g_act.s, "codeAction/resolve", g_act.json[i], RQ_RESOLVE, NULL);
  else do_action(g_act.s, a);
  json_free(a);
}


/* a showMessageRequest waiting for its button: the answer goes back with its id */
typedef struct Question {
  Srv *s;
  char id[80];	/* as JSON: "7" or "\"abc\"" */
  char act[4][48];
  int n;
} Question;


static void question_done (void *ud, int choice) {
  Question *q = (Question *)ud;
  Buf b;
  buf_init(&b);
  buf_printf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", q->id);
  if (choice >= 0 && choice < q->n) {
    buf_puts(&b, "{\"title\":");
    json_put_str(&b, q->act[choice], strlen(q->act[choice]));
    buf_puts(&b, "}}");
  }
  else buf_puts(&b, "null}");
  if (!q->s->dead) send_msg(q->s, &b);	/* a server gone takes no answer */
  buf_free(&b);
  free(q);
}


/* window/showDocument: a file (at its selection) or a URL in the browser */
static int show_document (Srv *s, const Json *params) {
  const char *uri = json_str(json_get(params, "uri"), NULL);
  const Json *sel = json_get(params, "selection.start");
  long line = -1, col = -1;
  char *path;
  (void)s;
  if (uri == NULL) return 0;
  if (json_bool(json_get(params, "external"), 0) || strncmp(uri, "file://", 7) != 0) {
    on_show_document(NULL, uri, -1, -1);
    return 1;
  }
  if (sel) {
    line = (long)unum(json_get(sel, "line"), 0);
    col = (long)unum(json_get(sel, "character"), 0);
  }
  path = lsp_path(uri);
  on_show_document(path, NULL, line, col);
  free(path);
  return 1;
}


/* a request of the server: answered with nothing, which every server takes */
static void answer (Srv *s, const Json *msg) {
  const Json *id = json_get(msg, "id");
  const char *method = json_str(json_get(msg, "method"), "");
  Buf b;
  if (strcmp(method, "window/showMessageRequest") == 0 && json_get(msg, "params.actions") &&
      json_get(msg, "params.actions")->type == J_ARR && json_get(msg, "params.actions")->n > 0) {
    const Json *acts = json_get(msg, "params.actions");	/* a question: answered when a button is picked */
    int type = inum(json_get(msg, "params.type"), 3);
    const char *titles[4];
    Question *q = (Question *)xmalloc(sizeof(Question));
    size_t i;
    memset(q, 0, sizeof(*q));
    q->s = s;
    if (id->type == J_STR) {
      Buf t;
      buf_init(&t);
      json_put_str(&t, id->str, id->len);
      buf_putc(&t, '\0');
      snprintf(q->id, sizeof(q->id), "%s", t.s);
      buf_free(&t);
    }
    else snprintf(q->id, sizeof(q->id), "%.0f", id->num);
    for (i = 0; i < acts->n && q->n < 4; i++) {
      snprintf(q->act[q->n], sizeof(q->act[0]), "%s", json_str(json_get(acts->kid[i], "title"), "?"));
      titles[q->n] = q->act[q->n];
      q->n++;
    }
    out_log(s->chan, "[info] %s", json_str(json_get(msg, "params.message"), ""));
    toast_ask(type == 1 ? 2 : type == 2 ? 1 : 0, s->chan, json_str(json_get(msg, "params.message"), ""), titles, q->n,
              question_done, q);
    return;
  }
  buf_init(&b);
  buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  if (id->type == J_STR) json_put_str(&b, id->str, id->len);
  else buf_printf(&b, "%.0f", id->num);	/* as it came (a cast of a huge one is undefined) */
  if (strcmp(method, "window/showMessageRequest") == 0) {	/* no buttons: a notification, answered at once */
    int type = inum(json_get(msg, "params.type"), 3);
    toast_src(type == 1 ? 2 : type == 2 ? 1 : 0, s->chan, "%s", json_str(json_get(msg, "params.message"), ""));
    buf_puts(&b, ",\"result\":null}");
  }
  else if (strcmp(method, "window/showDocument") == 0)
    buf_printf(&b, ",\"result\":{\"success\":%s}}", show_document(s, json_get(msg, "params")) ? "true" : "false");
  else if (strcmp(method, "workspace/applyEdit") == 0) {	/* a command's edit: it is done here */
    workspace_edit(s, json_get(msg, "params.edit"));
    buf_puts(&b, ",\"result\":{\"applied\":true}}");
  }
  else if (strcmp(method, "workspace/configuration") == 0) {	/* a null for every item asked; gopls its options */
    const Json *items = json_get(msg, "params.items");
    size_t i, n = items ? items->n : 0;
    buf_puts(&b, ",\"result\":[");
    for (i = 0; i < n; i++) {
      if (i) buf_putc(&b, ',');
      if (strcmp(json_str(json_get(items->kid[i], "section"), ""), "gopls") == 0) buf_puts(&b, GOPLS_OPTIONS);
      else buf_puts(&b, "null");
    }
    buf_puts(&b, "]}");
  }
  else buf_puts(&b, ",\"result\":null}");
  send_msg(s, &b);
  buf_free(&b);
  if (strcmp(method, "workspace/diagnostic/refresh") == 0) {	/* every file's problems asked for again */
    size_t k;
    for (k = 0; k < g_ndoc; k++)
      if (g_doc[k].s == s) pull(&g_doc[k]);
  }
}


static void handle (Srv *s, const Json *msg) {
  const Json *id = json_get(msg, "id"), *method = json_get(msg, "method");
  int i;
  if (method && id) {
    answer(s, msg);
    return;
  }
  if (method) {
    const char *mth = json_str(method, "");	/* not method->str: it is NULL when "method" is not a string */
    if (strcmp(mth, "textDocument/publishDiagnostics") == 0)
      diagnostics(s, json_get(msg, "params"));
    else if (strcmp(mth, "$/progress") == 0) progress(s, json_get(msg, "params"));
    else if (strcmp(mth, "didChangeStatus") == 0 || strcmp(mth, "statusNotification") == 0)
      auth_change(s, json_get(msg, "params"));	/* Copilot's own: how the inline server is */
    else if (strcmp(mth, "didChangeStatus/v2") == 0) auth_change_v2(s, json_get(msg, "params"));
    else if (strcmp(mth, "window/showMessage") == 0 || strcmp(mth, "window/logMessage") == 0) {
      static const char *const level[] = {"info", "error", "warning", "info", "info"};
      int type = inum(json_get(msg, "params.type"), 4);
      const char *text = json_str(json_get(msg, "params.message"), "");
      out_log(s->chan, "[%s] %s", level[type >= 1 && type <= 4 ? type : 0], text);
      if (mth[7] == 's' && type >= 1 && type <= 3)	/* a notification, as VS Code shows it */
        toast_src(type == 1 ? 2 : type == 2 ? 1 : 0, s->chan, "%s", text);
    }
    return;
  }
  if (id == NULL) return;
  for (i = 0; i < s->nreq; i++) {
    Req r;
    if (s->req[i].id != inum(id, -1)) continue;
    r = s->req[i];
    memmove(s->req + i, s->req + i + 1, (size_t)(s->nreq - i - 1) * sizeof(Req));
    s->nreq--;
    if (r.kind == RQ_INIT) {
      size_t k;
      const char *enc = json_str(json_get(msg, "result.capabilities.positionEncoding"), "utf-16");
      const Json *sy = json_get(msg, "result.capabilities.textDocumentSync");
      s->utf8 = strcmp(enc, "utf-8") == 0;
      if (sy != NULL && sy->type == J_OBJ) sy = json_get(sy, "change");	/* the long form */
      s->sync_inc = sy != NULL && (int)json_num(sy, 0) == 2;
      s->ready = 1;
      {	/* the semantic tokens' legend: its types as mme's tokens */
        const Json *ty = json_get(msg, "result.capabilities.semanticTokensProvider.legend.tokenTypes");
        const Json *mo = json_get(msg, "result.capabilities.semanticTokensProvider.legend.tokenModifiers");
        size_t q;
        s->nsem = 0;
        s->readonly_bit = -1;
        for (q = 0; ty && ty->type == J_ARR && q < ty->n && q < 64; q++) s->semtok[s->nsem++] = (signed char)sem_tok(json_str(ty->kid[q], ""));
        for (q = 0; mo && mo->type == J_ARR && q < mo->n && q < 31; q++)
          if (strcmp(json_str(mo->kid[q], ""), "readonly") == 0) s->readonly_bit = (int)q;
      }
      {	/* what else it does */
        const Json *c = json_get(msg, "result.capabilities"), *ot;
        const Json *p;
        s->can_resolve = json_bool(json_get(c, "completionProvider.resolveProvider"), 0);
        p = json_get(c, "documentRangeFormattingProvider");
        s->can_range = p && (p->type == J_OBJ || (p->type == J_BOOL && p->b));
        p = json_get(c, "selectionRangeProvider");
        s->can_sel = p && (p->type == J_OBJ || (p->type == J_BOOL && p->b));
        p = json_get(c, "foldingRangeProvider");
        s->can_fold = p && (p->type == J_OBJ || (p->type == J_BOOL && p->b));
        p = json_get(c, "callHierarchyProvider");
        s->can_calls = p && (p->type == J_OBJ || (p->type == J_BOOL && p->b));
        p = json_get(c, "typeHierarchyProvider");
        s->can_types = p && (p->type == J_OBJ || (p->type == J_BOOL && p->b));
        p = json_get(c, "inlineCompletionProvider");
        s->can_inline = p && (p->type == J_OBJ || (p->type == J_BOOL && p->b));
        p = json_get(c, "diagnosticProvider");
        s->can_pull = p && p->type == J_OBJ;
        snprintf(s->pull_id, sizeof(s->pull_id), "%s", json_str(json_get(p, "identifier"), ""));
        p = json_get(c, "colorProvider");
        s->can_color = p && (p->type == J_OBJ || (p->type == J_BOOL && p->b));
        p = json_get(c, "documentLinkProvider");
        s->can_link = p && (p->type == J_OBJ || (p->type == J_BOOL && p->b));
        s->link_resolve = json_bool(json_get(p, "resolveProvider"), 0);
        free(s->will_ren);
        free(s->did_ren);
        s->will_ren = json_text(json_get(c, "workspace.fileOperations.willRename.filters"));
        s->did_ren = json_text(json_get(c, "workspace.fileOperations.didRename.filters"));
        s->type_chars[0] = '\0';
        if ((ot = json_get(c, "documentOnTypeFormattingProvider")) != NULL) {
          const Json *more = json_get(ot, "moreTriggerCharacter");
          size_t q, k = 0;
          const char *f = json_str(json_get(ot, "firstTriggerCharacter"), "");
          if (f[0] && !f[1]) s->type_chars[k++] = f[0];
          for (q = 0; more && more->type == J_ARR && q < more->n && k + 1 < sizeof(s->type_chars); q++) {
            const char *m = json_str(more->kid[q], "");
            if (m[0] && !m[1]) s->type_chars[k++] = m[0];
          }
          s->type_chars[k] = '\0';
        }
      }
      notify(s, "initialized", "{}");
      for (k = 0; k < g_ndoc; k++)
        if (g_doc[k].s == s) did_open(&g_doc[k]);
      if (strcmp(s->lang, INLINE_LANG) == 0) lsp_inline_check();	/* who is signed in, before anything is asked of it */
    }
    else if (r.kind == RQ_SIGNIN) auth_prompt(s, json_get(msg, "result"));
    else if (r.kind == RQ_DEVICE) auth_done(s, msg);
    else if (r.kind == RQ_CHECK) auth_account(json_get(msg, "result"));
    else if (r.kind == RQ_SIGNOUT) {
      g_auth.msg[0] = '\0';	/* whatever it last said about itself is past */
      auth_account(json_get(msg, "result"));
      g_auth.user[0] = '\0';	/* whatever it answered, the credentials are gone */
      g_auth.known = 1;
      toast_src(0, s->chan, "Signed out of GitHub Copilot");
    }
    else if (r.kind == RQ_COMPLETE) completion(s, r.d, json_get(msg, "result"));
    else if (r.kind == RQ_DEFINE) definition(s, json_get(msg, "result"));
    else if (r.kind == RQ_HOVER) hover(json_get(msg, "result"));
    else if (r.kind == RQ_SIGNATURE) signature(json_get(msg, "result"));
    else if (r.kind == RQ_RENAME) {
      const Json *err = json_get(msg, "error.message");
      if (err) toast(1, "%s", json_str(err, "Rename failed"));
      else if (json_get(msg, "result")) {
        g_confirm = 1;
        workspace_edit(s, json_get(msg, "result"));
        g_confirm = 0;
      }
    }
    else if (r.kind == RQ_ACTIONS) actions(s, json_get(msg, "result"));
    else if (r.kind == RQ_RESOLVE && json_get(msg, "result")) do_action(s, json_get(msg, "result"));
    else if (r.kind >= RQ_LOC_REFS && r.kind <= RQ_LOC_PEEK) {
      if (json_get(msg, "error")) on_locations(r.kind - RQ_LOC_REFS, NULL, 0);
      else locations(s, json_get(msg, "result"), r.kind - RQ_LOC_REFS);
    }
    else if (r.kind == RQ_WSYM) workspace_symbols(s, json_get(msg, "result"));
    else if (r.kind == RQ_INLAY) {	/* InlayHint[]: the label a string, or parts */
      const Json *res = json_get(msg, "result");
      InlayHint *v = NULL;
      size_t k, n = 0;
      if (res && res->type == J_ARR && res->n) v = (InlayHint *)xmalloc(res->n * sizeof(InlayHint));
      for (k = 0; res && res->type == J_ARR && k < res->n; k++) {
        const Json *h = res->kid[k], *lb = json_get(h, "label");
        Buf b;
        size_t q;
        buf_init(&b);
        if (json_bool(json_get(h, "paddingLeft"), 0)) buf_putc(&b, ' ');
        if (lb && lb->type == J_STR) buf_puts(&b, lb->str);
        else for (q = 0; lb && lb->type == J_ARR && q < lb->n; q++) buf_puts(&b, json_str(json_get(lb->kid[q], "value"), ""));
        if (json_bool(json_get(h, "paddingRight"), 0)) buf_putc(&b, ' ');
        buf_putc(&b, '\0');
        for (q = 0; b.s[q]; q++)
          if (b.s[q] == '\n' || b.s[q] == '\t') b.s[q] = ' ';
        v[n].at = pos_in(s, r.d, json_get(h, "position"));
        v[n].label = buf_take(&b);
        v[n].color = 0;
        n++;
      }
      on_inlay(r.d, r.at.y, r.at.x, v, n);
    }
    else if (r.kind == RQ_SEMANTIC) {	/* relative: line, start, length, type, modifiers */
      const Json *data = json_get(msg, "result.data");
      SemTok *v = NULL;
      size_t k, n = 0, line = 0, ch = 0;
      if (data && data->type == J_ARR && data->n >= 5) v = (SemTok *)xmalloc((data->n / 5) * sizeof(SemTok));
      for (k = 0; v && k + 5 <= data->n; k += 5) {
        size_t dl = unum(data->kid[k], 0), ds = unum(data->kid[k + 1], 0), len = unum(data->kid[k + 2], 0);
        int ty = inum(data->kid[k + 3], -1), mods = inum(data->kid[k + 4], 0), t;
        line += dl;
        ch = dl ? ds : ch + ds;
        if (ty < 0 || ty >= s->nsem || (t = s->semtok[ty]) < 0 || line >= r.d->n) continue;
        if (t == T_VAR && s->readonly_bit >= 0 && s->readonly_bit < 31 && (mods >> s->readonly_bit) & 1) t = T_CONST;
        v[n].y = line;
        v[n].x0 = col_in(s, r.d, line, ch);
        v[n].x1 = col_in(s, r.d, line, ch + len);
        v[n].tok = t;
        n++;
      }
      on_semantic(r.d, (unsigned long)r.at.x, v, n);
    }
    else if (r.kind == RQ_INLINE) {
      g_inl.id = 0;
      g_inl.asked = NULL;
      inlines(s, r.d, json_get(msg, "result"));
    }
    else if (r.kind == RQ_NEDIT) {
      g_ne.id = 0;
      g_ne.asked = NULL;
      if (inum(json_get(msg, "error.code"), 0) == -32601) {	/* it has never heard of it: never again */
        s->no_nedit = 1;
        nedit_forget();
        on_nedit(r.d, g_ne.edits, g_ne.at, NULL, 0);
      }
      else nedits(s, r.d, json_get(msg, "result"));
    }
    else if (r.kind == RQ_LENS) lenses(s, r.d, json_get(msg, "result"));
    else if (r.kind == RQ_LENS_RESOLVE) {
      const Json *res = json_get(msg, "result");
      if (res && r.d == g_lens.d && r.at.x < g_lens.n) {
        Buf b;
        buf_init(&b);
        json_write(&b, res);
        buf_putc(&b, '\0');
        free(g_lens.json[r.at.x]);
        g_lens.json[r.at.x] = buf_take(&b);
        lens_tell();
      }
    }
    else if (r.kind == RQ_BULB) {	/* the source actions (organize imports ...) do not count, as in VS Code */
      const Json *res = json_get(msg, "result");
      size_t k, n = 0;
      int fix = 0;
      for (k = 0; res && res->type == J_ARR && k < res->n; k++) {
        const char *kd = json_str(json_get(res->kid[k], "kind"), "");
        if (strncmp(kd, "source", 6) == 0) continue;
        n++;
        if (strncmp(kd, "quickfix", 8) == 0 || json_bool(json_get(res->kid[k], "isPreferred"), 0)) fix = 1;
      }
      on_bulb(r.d, r.at.y, n, fix);
    }
    else if (r.kind == RQ_HIGHLIGHT) highlights(s, r.d, r.at, json_get(msg, "result"));
    else if (r.kind == RQ_COMP_RESOLVE) {	/* the item again, with its documentation and imports */
      const Json *res = json_get(msg, "result");
      size_t n = 0;
      TextEdit *ex = res ? text_edits(s, json_get(res, "additionalTextEdits"), &n) : NULL;
      char *doc = res ? doc_text_of(json_get(res, "documentation")) : NULL;
      on_comp_resolve(r.d, (unsigned)r.at.y, r.at.x, res ? json_str(json_get(res, "detail"), NULL) : NULL, doc, ex, n);
      free(doc);
    }
    else if (r.kind == RQ_ONTYPE) {	/* a failure says nothing */
      size_t n, j;
      TextEdit *v = text_edits(s, json_get(msg, "result"), &n);
      if (r.d->edits == (unsigned long)r.at.x) on_format(r.d, v, n, 0);
      for (j = 0; j < n; j++) free(v[j].text);
      free(v);
    }
    else if (r.kind == RQ_SOURCE) {	/* organize imports: the first action there is, done */
      const Json *res = json_get(msg, "result"), *a = res && res->type == J_ARR && res->n ? res->kid[0] : NULL;
      if (a == NULL) {
        if (r.at.x != 2) toast(0, "No organize imports action available");	/* 2: a save's, quiet */
      }
      else if (!json_get(a, "edit") && !json_get(a, "command") && json_get(a, "data")) {	/* the server fills it in */
        Buf b;
        buf_init(&b);
        json_write(&b, a);
        buf_putc(&b, '\0');
        request(s, "codeAction/resolve", b.s, RQ_SOURCE_RESOLVE, r.d);
        buf_free(&b);
      }
      else do_action(s, a);
    }
    else if (r.kind == RQ_SOURCE_RESOLVE && json_get(msg, "result")) do_action(s, json_get(msg, "result"));
    else if (r.kind == RQ_SELRANGE) {	/* SelectionRange[]: the first, with its parents */
      const Json *res = json_get(msg, "result"), *sr = res && res->type == J_ARR && res->n ? res->kid[0] : NULL;
      Pos *v = NULL;
      size_t n = 0, cap = 0;
      while (sr && sr->type == J_OBJ && n < 200) {
        if (2 * n + 2 > cap) v = (Pos *)xrealloc(v, (cap = cap ? cap * 2 : 32) * sizeof(Pos));
        v[2 * n] = pos_in(s, r.d, json_get(sr, "range.start"));
        v[2 * n + 1] = pos_in(s, r.d, json_get(sr, "range.end"));
        n++;
        sr = json_get(sr, "parent");
      }
      on_selection_ranges(r.d, r.at, v, n);
      free(v);
    }
    else if (r.kind == RQ_FOLDING) {
      const Json *res = json_get(msg, "result");
      FoldRange *v = NULL;
      size_t n = 0, k;
      if (res && res->type == J_ARR && res->n) v = (FoldRange *)xmalloc(res->n * sizeof(FoldRange));
      for (k = 0; v && k < res->n; k++) {
        const char *kind = json_str(json_get(res->kid[k], "kind"), "");
        v[n].y0 = unum(json_get(res->kid[k], "startLine"), 0);
        v[n].y1 = unum(json_get(res->kid[k], "endLine"), 0);
        v[n].comment = strcmp(kind, "comment") == 0;
        v[n].region = strcmp(kind, "region") == 0;
        if (v[n].y1 > v[n].y0) n++;
      }
      on_folding(r.d, (unsigned long)r.at.x, v, n);
    }
    else if (r.kind == RQ_HPREP) {	/* the item at the cursor: now its calls, or its types */
      const Json *res = json_get(msg, "result"), *item = res && res->type == J_ARR && res->n ? res->kid[0] : NULL;
      static const char *const method[] = {"callHierarchy/incomingCalls", "callHierarchy/outgoingCalls",
                                           "typeHierarchy/supertypes", "typeHierarchy/subtypes"};
      int what = (int)r.at.x;
      if (item == NULL) on_locations(what, NULL, 0);
      else {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"item\":");
        json_write(&b, item);
        buf_puts(&b, "}");
        buf_putc(&b, '\0');
        request(s, method[what - LOC_CALLS_IN], b.s, RQ_HIER, r.d);
        s->req[s->nreq - 1].at.x = (size_t)what;
        buf_free(&b);
      }
    }
    else if (r.kind == RQ_HIER) hierarchy(s, json_get(msg, "result"), (int)r.at.x);
    else if (r.kind == RQ_FORMAT) {	/* TextEdit[] of this file */
      const Json *res = json_get(msg, "result");
      TextEdit *v = NULL;
      size_t n = 0, j;
      if (res && res->type == J_ARR && res->n) {
        v = (TextEdit *)xmalloc(res->n * sizeof(TextEdit));
        for (j = 0; j < res->n; j++) {
          const Json *st = json_get(res->kid[j], "range.start"), *en = json_get(res->kid[j], "range.end");
          v[n].path = NULL;
          v[n].l0 = unum(json_get(st, "line"), 0);
          v[n].c0 = unum(json_get(st, "character"), 0);
          v[n].l1 = unum(json_get(en, "line"), 0);
          v[n].c1 = unum(json_get(en, "character"), 0);
          v[n].text = xstrdup(json_str(json_get(res->kid[j], "newText"), ""));
          v[n].utf16 = !s->utf8;
          n++;
        }
      }
      on_format(r.d, v, n, json_get(msg, "error") != NULL);
      for (j = 0; j < n; j++) free(v[j].text);
      free(v);
    }
    else if (r.kind == RQ_SYMBOLS) {
      Sym *v = NULL;
      size_t n = 0, cap = 0;
      add_symbols(json_get(msg, "result"), 0, &v, &n, &cap);
      on_symbols(r.d, v, n);
    }
    else if (r.kind == RQ_PULL) pulled(s, r.d, msg);
    else if (r.kind == RQ_COLOR) colors(s, r.d, (unsigned long)r.at.x, json_get(msg, "result"));
    else if (r.kind == RQ_COLOR_PRES) color_pres(s, r.d, json_get(msg, "result"));
    else if (r.kind == RQ_LINK) links(s, r.d, (unsigned long)r.at.x, json_get(msg, "result"));
    else if (r.kind == RQ_LINK_RESOLVE) {
      const char *target = json_str(json_get(msg, "result.target"), NULL);
      if (target) link_follow(target);
      else toast(0, "Failed to open this link because it has no target");	/* VS Code's words */
    }
    else if (r.kind == RQ_WILL_RENAME) {	/* only while the rename waits: a late one would edit the old paths */
      if (g_will > 0) {
        g_will--;
        if (json_get(msg, "result") && json_get(msg, "result")->type == J_OBJ) workspace_edit(s, json_get(msg, "result"));
      }
    }
    return;
  }
}


/* the Content-Length a header says, (size_t)-1 when it is not one we can use */
static size_t hdr_len (const char *s) {
  size_t v = 0;
  while (*s == ' ' || *s == '	') s++;
  if (*s < '0' || *s > '9') return (size_t)-1;	/* a sign, or nothing: strtoul would make it huge */
  for (; *s >= '0' && *s <= '9'; s++) {
    if (v > (64u << 20)) return (size_t)-1;	/* no message of ours is that big */
    v = v * 10 + (size_t)(*s - '0');
  }
  return v > (64u << 20) ? (size_t)-1 : v;
}


/* the whole messages that came in s->in */
static int messages (Srv *s) {
  int got = 0;
  for (;;) {
    char *hdr_end, *cl;
    size_t body, hlen;
    Json *j;
    if (s->in.len == 0) break;
    s->in.s[s->in.len] = '\0';
    hdr_end = strstr(s->in.s, "\r\n\r\n");
    if (hdr_end == NULL) break;
    hlen = (size_t)(hdr_end - s->in.s) + 4;
    cl = strstr(s->in.s, "Content-Length:");
    if (cl == NULL || cl > hdr_end) {	/* no length: drop the header */
      memmove(s->in.s, s->in.s + hlen, s->in.len - hlen);
      s->in.len -= hlen;
      continue;
    }
    body = hdr_len(cl + 15);
    if (body == (size_t)-1) {	/* not a length we will ever see: it is not talking LSP */
      out_log(s->chan, "[error] Content-Length out of range: the server is not talking LSP");
      s->dead = 1;
      break;
    }
    if (s->in.len < hlen + body) break;
    trace("<< ", s->in.s + hlen, body);
    j = json_parse(s->in.s + hlen, body);
    if (j) {
      handle(s, j);
      json_free(j);
      got = 1;
    }
    memmove(s->in.s, s->in.s + hlen + body, s->in.len - hlen - body);
    s->in.len -= hlen + body;
  }
  return got;
}


/* what the server wrote, into s->in */
static void srv_read (Srv *s) {
  char chunk[65536];
  while (!s->dead && os_wait_readable(s->from, 0) == 1) {
    long n = os_read(s->from, chunk, sizeof(chunk));
    if (n <= 0) {
      s->dead = 1;
      break;
    }
    trace("-- read ", "", 0);
    buf_putn(&s->in, chunk, (size_t)n);
    buf_putc(&s->in, '\0');	/* room for the NUL messages() puts */
    s->in.len--;
  }
}


int lsp_poll (void) {
  int i, got = 0;
  size_t k;
  for (i = 0; i < g_nsrv; i++) {
    Srv *s = g_srv[i];
    char chunk[65536];
    int status;
    if (s->dead) continue;
    srv_read(s);
    while (s->err >= 0 && os_wait_readable(s->err, 0) == 1) {	/* what it says on stderr: its channel */
      long n = os_read(s->err, chunk, sizeof(chunk));
      if (n <= 0) {
        os_close(s->err);
        s->err = -1;
        break;
      }
      out_append(s->chan, chunk, (size_t)n);
    }
    if (!s->dead && os_poll_proc(s->proc, &status) == 1) s->dead = 1;
    if (s->dead && !s->told) {
      out_log(s->chan, "[error] The %s language server stopped", s->lang);
      s->told = 1;
      s->nprog = 0;
      if (!s->gone && !g_down) crashed(s);	/* started again, VS Code's way */
    }
    got |= messages(s);
  }
  for (k = 0; k < g_ndoc; k++) {	/* edits: the server gets the text when the typing stops */
    LDoc *l = &g_doc[k];
    long long now;
    if (l->repull && !l->s->dead && os_now_us() - l->repull_at >= SYNC_WAIT) pull(l);	/* it cancelled the last */
    if (!(l->s->ready && !l->s->dead && l->opened && l->sent != l->d->edits)) continue;
    now = os_now_us();
    if (l->seen != l->d->edits) {	/* still typing: wait for a pause */
      l->seen = l->d->edits;
      l->seen_at = now;
      continue;
    }
    if (now - l->seen_at >= SYNC_WAIT) did_change(l);
  }
  auth_idle();	/* a device flow nobody will ever answer is given up on */
  return got;
}

/* }================================================================== */


/*
** {==================================================================
** File operations: the Explorer renames or moves a file, and the
** servers that asked for it (workspace.fileOperations) update what
** points at it - the imports of TypeScript, JavaScript ...
** ===================================================================
*/

#define WILL_WAIT	5000000	/* us a rename waits for the servers' edits at most */

static int fold_case (int c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}


/* the LSP's glob p against the path s ('/' between folders): * ** ? {a,b} [a-z] [!a] */
static int glob_match (const char *p, const char *s, int nocase) {
  while (*p) {
    if (*p == '*') {
      int deep = p[1] == '*';
      p += deep ? 2 : 1;
      if (deep && *p == '/' && glob_match(p + 1, s, nocase)) return 1;	/* "**" "/": no folder too */
      for (;; s++) {
        if (glob_match(p, s, nocase)) return 1;
        if (*s == '\0' || (!deep && *s == '/')) return 0;
      }
    }
    if (*p == '{') {	/* {a,b}rest: a then rest, b then rest */
      const char *e = strchr(p, '}'), *a = p + 1;
      if (e == NULL) return 0;
      while (a <= e) {
        const char *c = a;
        char buf[512];
        size_t n;
        while (c < e && *c != ',') c++;
        n = (size_t)(c - a);
        if (n + strlen(e + 1) < sizeof(buf)) {
          memcpy(buf, a, n);
          strcpy(buf + n, e + 1);
          if (glob_match(buf, s, nocase)) return 1;
        }
        a = c + 1;
      }
      return 0;
    }
    if (*s == '\0') return 0;
    if (*p == '[') {	/* a class: [abc], [a-z], [!a] */
      const char *q = p + 1;
      int neg = 0, hit = 0, c = nocase ? fold_case((unsigned char)*s) : (unsigned char)*s;
      if (*q == '!' || *q == '^') neg = 1, q++;
      for (; *q && *q != ']'; q++) {
        int lo = nocase ? fold_case((unsigned char)*q) : (unsigned char)*q, hi = lo;
        if (q[1] == '-' && q[2] && q[2] != ']') {
          hi = nocase ? fold_case((unsigned char)q[2]) : (unsigned char)q[2];
          q += 2;
        }
        if (c >= lo && c <= hi) hit = 1;
      }
      if (*q != ']' || hit == neg || *s == '/') return 0;
      p = q + 1;
      s++;
      continue;
    }
    if (*p == '?') {
      if (*s == '/') return 0;
    }
    else if (nocase ? fold_case((unsigned char)*p) != fold_case((unsigned char)*s) : *p != *s) return 0;
    p++;
    s++;
  }
  return *s == '\0';
}


/* does one of the server's filters (FileOperationFilter[], as JSON) take this file or folder? */
static int ren_wanted (const char *filters, const char *path, int dir) {
  Json *f;
  char *p;
  size_t i;
  int yes = 0;
  if (filters == NULL || (f = json_parse(filters, strlen(filters))) == NULL) return 0;
  p = xstrdup(path);
  for (i = 0; p[i]; i++)
    if (p[i] == '\\' && path_is_sep('\\')) p[i] = '/';
  for (i = 0; f->type == J_ARR && i < f->n && !yes; i++) {
    const Json *x = f->kid[i];
    const char *m = json_str(json_get(x, "pattern.matches"), "");
    if (strcmp(json_str(json_get(x, "scheme"), "file"), "file") != 0) continue;
    if ((strcmp(m, "file") == 0 && dir) || (strcmp(m, "folder") == 0 && !dir)) continue;
    yes = glob_match(json_str(json_get(x, "pattern.glob"), ""), p, json_bool(json_get(x, "pattern.options.ignoreCase"), 0));
  }
  free(p);
  json_free(f);
  return yes;
}


/* RenameFilesParams: {"files":[{"oldUri":...,"newUri":...}]} */
static char *rename_params (const char *from, const char *to) {
  char *a = to_uri(from), *b = to_uri(to);
  Buf q;
  buf_init(&q);
  buf_printf(&q, "{\"files\":[{\"oldUri\":\"%s\",\"newUri\":\"%s\"}]}", a, b);
  buf_putc(&q, '\0');
  free(a);
  free(b);
  return buf_take(&q);
}


/*
** workspace/willRenameFiles: VS Code waits for the answers before the
** file moves, and so does this; the edits they send (the imports of the
** files that point at it) are made in the editor, the files open with them
*/
void lsp_will_rename (const char *from, const char *to, int dir) {
  char *params = NULL;
  long long end = os_now_us() + WILL_WAIT;
  int i;
  g_will = 0;
  for (i = 0; i < g_nsrv; i++) {
    Srv *s = g_srv[i];
    if (s->gone || s->dead || !s->ready || !ren_wanted(s->will_ren, from, dir)) continue;
    if (params == NULL) params = rename_params(from, to);
    request(s, "workspace/willRenameFiles", params, RQ_WILL_RENAME, NULL);
    g_will++;
  }
  free(params);
  while (g_will > 0 && os_now_us() < end) {
    int alive = 0;
    for (i = 0; i < g_nsrv && g_will > 0; i++) {
      Srv *s = g_srv[i];
      if (s->dead || s->gone) continue;
      alive = 1;
      if (os_wait_readable(s->from, 10) == 1) {
        srv_read(s);
        messages(s);
      }
    }
    if (!alive) break;
  }
  g_will = 0;	/* what did not answer in time is not waited for: the file moves without its edits */
}


void lsp_did_rename (const char *from, const char *to, int dir) {
  char *params = NULL;
  int i;
  for (i = 0; i < g_nsrv; i++) {
    Srv *s = g_srv[i];
    if (s->gone || s->dead || !s->ready || !(ren_wanted(s->did_ren, from, dir) || ren_wanted(s->did_ren, to, dir)))
      continue;
    if (params == NULL) params = rename_params(from, to);
    notify(s, "workspace/didRenameFiles", params);
  }
  free(params);
}

/* }================================================================== */
