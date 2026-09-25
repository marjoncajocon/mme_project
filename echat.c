/*
** echat.c - Chat, like VS Code's: Claude in the secondary side bar, and
** Inline Chat in the editor
**
** The Chat view (Ctrl+Alt+I) is VS Code's: the talk so far, the answers'
** Markdown drawn by emd.c (code blocks in their language's colors, each
** with Copy, Insert and Apply on its top row), and the box to type in at
** the bottom. The editor's selection - or the lines it shows, when nothing
** is selected - goes with the question, as VS Code's implicit context does:
** a chip in the box names it, and a click on it leaves it out. Inline Chat
** (Ctrl+I in the editor) asks for a change of the selection, or for code at
** the cursor, and shows Claude's code in the text as a diff - the next edit
** suggestion's - to take with Ctrl+Enter or Tab, or to throw away with Esc.
**
** Claude is Anthropic's Messages API. C has no library for it: curl runs in
** the background, POST {baseUrl}/v1/messages with "stream": true, and what
** it prints - the response's head, then the server-sent events - is read as
** it comes, so the answer grows on the screen. The key never goes on curl's
** command line, where every program could read it: the url, the headers and
** the body go to curl's stdin as a config file (curl -K -), and nothing of
** it is written to disk or to OUTPUT. Each question sends the whole talk
** again: the API remembers nothing between requests.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define CHAT_MODEL	"claude-opus-5"	/* mme.chat.model's default */
#define CHAT_URL	"https://api.anthropic.com"	/* mme.chat.baseUrl's, and $ANTHROPIC_BASE_URL's */
#define CHAT_MAX_TOKENS	64000	/* streamed: room for a long answer */
#define INPUT_ROWS	8	/* the box to type in grows to this many rows */
#define NEAR_LINES	400	/* Inline Chat sends this many lines around the selection */

#define ICON_ACCOUNT	0xEB99	/* codicon account: the user's turn */
#define ICON_SPARKLE	0xEC10	/* codicon sparkle: Claude's */
#define ICON_SEND	0xEC0F	/* codicon send */
#define ICON_STOP	0xEAD7	/* codicon debug-stop */
#define ICON_ADD	0xEA60	/* codicon add: New Chat */
#define ICON_CLOSE	0xEA76	/* codicon close */
#define ICON_EYE	0xEA70	/* codicon eye: the context goes with the question */
#define ICON_EYE_OFF	0xEAE7	/* codicon eye-closed */

#define CLAUDE_RGB	0xD97757	/* Claude's color, on its sparkle */


static const char sys_chat[] =
  "You are Claude, a coding assistant in mme, a text editor for the terminal that looks and works "
  "like VS Code. Answer in Markdown. Put code in fenced code blocks that name their language: the "
  "editor offers to insert a block at the cursor or to put it in place of the selection. A message "
  "may begin with an <attachment>: the part of the user's file that they have selected, or the "
  "lines their editor shows. The view is narrow, so be brief.";

static const char sys_inline[] =
  "You are Claude, editing code in the user's text editor. The user's file comes with the place to "
  "change marked. Reply with only the code that goes there - no explanation, no Markdown code "
  "fences - written in the file's language, with its indentation and style.";


/*
** {==================================================================
** The talk so far
** ===================================================================
*/

enum { TU_USER, TU_CLAUDE, TU_NOTE };	/* a notice (an error, a refusal) is shown, never sent */

typedef struct Turn {
  int who;	/* TU_* */
  Buf text;	/* what shows: what was typed, what streamed in, the notice */
  char *sent;	/* TU_USER: the message as it went, its context before it */
  char *ref;	/* TU_USER: the context's chip ("main.c:10-20"); NULL none */
  const Doc *doc;	/* TU_USER: the text the context came from, as it was, */
  unsigned long edits;
  Pos a, b;	/* and the selection there (a == b: none), for Apply */
} Turn;

typedef struct Hit {	/* a place of the view the mouse can use */
  int y, x0, x1;
  int what;	/* HIT_* */
  size_t i;	/* HIT_COPY ...: the code block */
} Hit;

enum { HIT_COPY, HIT_INSERT, HIT_APPLY, HIT_KEY };

static struct {
  Turn *t;
  size_t n, cap;
  Doc doc;	/* the talk as Markdown: what emd.c draws */
  int doc_ok;	/* doc_init was done */
  int stale;	/* doc is made again before it is drawn */
  size_t *fence;	/* the lines of doc where Claude's code blocks start */
  size_t *fence_turn;	/* the question each one answers */
  char **code;	/* and their code */
  size_t nfence, capfence;
  size_t top;	/* the talk's first row shown */
  int follow;	/* the view stays at the bottom while the answer comes */
  Buf in;	/* the box to type in */
  size_t at;	/* the cursor in it, a byte */
  int in_top;	/* its first row shown */
  int no_ctx;	/* the implicit context left out (its chip clicked) */
  int shown;	/* the secondary side bar is open */
  char *last;	/* the last question: Up in an empty box brings it back */
  /* where things were drawn, for the mouse */
  int x, y, w, h;
  int new_x, close_x;
  int chip_y, send_x, send_y;
  int box_y0, box_y1, in_y, in_rows;
  int tr_y, tr_h;
  Hit hit[64];
  int nhit;
} C;


static Turn *turn_add (int who, const char *s, size_t n) {
  Turn *t;
  if (C.n == C.cap) {
    C.cap = C.cap ? C.cap * 2 : 16;
    C.t = (Turn *)xrealloc(C.t, C.cap * sizeof(Turn));
  }
  t = &C.t[C.n++];
  memset(t, 0, sizeof(*t));
  t->who = who;
  buf_init(&t->text);
  if (s) buf_putn(&t->text, s, n);
  C.stale = 1;
  C.follow = 1;
  return t;
}


static void note (const char *msg) {
  turn_add(TU_NOTE, msg, strlen(msg));
}


static void fences_clear (void) {
  size_t i;
  for (i = 0; i < C.nfence; i++) free(C.code[i]);
  C.nfence = 0;
}


static void talk_clear (void) {
  size_t i;
  for (i = 0; i < C.n; i++) {
    buf_free(&C.t[i].text);
    free(C.t[i].sent);
    free(C.t[i].ref);
  }
  C.n = 0;
  C.top = 0;
  C.follow = 1;
  C.stale = 1;
  fences_clear();
}


/* a line that opens or closes a code block: ``` or ~~~, spaces before it */
static int is_fence (const char *s, size_t n, char *ch) {
  size_t i = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i + 3 > n || (s[i] != '`' && s[i] != '~') || s[i + 1] != s[i] || s[i + 2] != s[i]) return 0;
  *ch = s[i];
  return 1;
}


static void fence_add (size_t line, size_t turn, const Buf *code) {
  if (C.nfence == C.capfence) {
    C.capfence = C.capfence ? C.capfence * 2 : 16;
    C.fence = (size_t *)xrealloc(C.fence, C.capfence * sizeof(size_t));
    C.fence_turn = (size_t *)xrealloc(C.fence_turn, C.capfence * sizeof(size_t));
    C.code = (char **)xrealloc(C.code, C.capfence * sizeof(char *));
  }
  C.fence[C.nfence] = line;
  C.fence_turn[C.nfence] = turn;
  C.code[C.nfence] = xstrndup(code->s ? code->s : "", code->len);
  C.nfence++;
}


/*
** A turn's text into the page, line by line (*line counts them). Its code
** blocks are noted when it is Claude's (keep), and one left open is
** closed, so that the turns after it are not taken for code.
*/
static void put_text (Buf *b, const char *s, size_t n, size_t *line, int keep, size_t turn) {
  size_t i = 0, fl = 0;
  char open = 0, ch;
  Buf code;
  if (n == 0) return;
  buf_init(&code);
  while (i <= n) {
    const char *e = memchr(s + i, '\n', n - i);
    size_t len = e ? (size_t)(e - (s + i)) : n - i;
    if (i == n && n > 0 && s[n - 1] == '\n') break;	/* the last newline ends nothing more */
    if (open == 0 && is_fence(s + i, len, &ch)) {
      open = ch;
      fl = *line;
      code.len = 0;
    }
    else if (open && is_fence(s + i, len, &ch) && ch == open) {
      if (keep) fence_add(fl, turn, &code);
      open = 0;
    }
    else if (open) {
      if (code.len) buf_putc(&code, '\n');
      buf_putn(&code, s + i, len);
    }
    buf_putn(b, s + i, len);
    buf_putc(b, '\n');
    (*line)++;
    if (e == NULL) break;
    i += len + 1;
  }
  if (open) {	/* still coming, or never closed */
    if (keep) fence_add(fl, turn, &code);
    buf_puts(b, open == '`' ? "```\n" : "~~~\n");
    (*line)++;
  }
  buf_free(&code);
}


/* the talk as one Markdown page: a name over each turn, like VS Code's */
static void build_doc (int busy) {
  Buf b;
  size_t i, line = 0, ask = 0;
  if (!C.doc_ok) {
    doc_init(&C.doc);
    C.doc_ok = 1;
  }
  fences_clear();
  buf_init(&b);
  for (i = 0; i < C.n; i++) {
    const Turn *t = &C.t[i];
    if (i > 0) {
      buf_putc(&b, '\n');
      line++;
    }
    if (t->who == TU_USER) {
      ask = i;
      buf_puts(&b, "\xEE\xAE\x99 **You**\n\n");	/* codicon account */
      line += 2;
      put_text(&b, t->text.s, t->text.len, &line, 0, i);
      if (t->ref) {
        buf_printf(&b, "\n`%s`\n", t->ref);
        line += 2;
      }
    }
    else if (t->who == TU_CLAUDE) {
      buf_puts(&b, "\xEE\xB0\x90 **Claude**\n\n");	/* codicon sparkle */
      line += 2;
      if (t->text.len == 0 && busy && i + 1 == C.n) {
        buf_printf(&b, "*%s Working...*\n", ui_spinner());
        line++;
      }
      else put_text(&b, t->text.s, t->text.len, &line, 1, ask);
    }
    else {
      const char *s = t->text.s ? t->text.s : "";
      buf_puts(&b, "> ");
      line++;
      for (; *s; s++) {
        buf_putc(&b, *s);
        if (*s == '\n') {
          buf_puts(&b, "> ");
          line++;
        }
      }
      buf_putc(&b, '\n');
    }
  }
  doc_set_text(&C.doc, b.s ? b.s : "", b.len);
  buf_free(&b);
  C.stale = 0;
}

/* }================================================================== */


/*
** {==================================================================
** The request: curl in the background
** ===================================================================
*/

static struct {
  int on;	/* curl is running */
  int inl;	/* for Inline Chat, else for the view */
  OsProc proc;
  long pid;
  int out, err;	/* its stdout and stderr; -1: closed */
  int head;	/* the response's head is being read */
  int proxy;	/* the head is a proxy's "Connection established": the real one follows */
  int status;	/* the HTTP status; 0: none came */
  char retry[32];	/* retry-after */
  char stop[32];	/* message_delta's stop_reason */
  Buf line;	/* a line not ended yet */
  Buf body;	/* a failure's body */
  Buf errs;	/* curl's stderr */
  Buf answer;	/* Inline Chat's code */
  char *error;	/* an error event's message */
  long long t0;
} J;

/* Inline Chat's request: what it is for, and what it replaces */
static struct {
  int busy;	/* the answer is coming */
  const Doc *doc;
  unsigned long edits;
  Pos a, b;
  char prompt[512];
} IC;


static const char *cfg (const char *key, const char *def) {
  return json_str(settings_get(key), def);
}


static const char *model (void) {
  const char *m = cfg("mme\\.chat\\.model", CHAT_MODEL);
  return *m ? m : CHAT_MODEL;
}


/* mme.chat.apiKey, else $ANTHROPIC_API_KEY, else $ANTHROPIC_AUTH_TOKEN (sent as a bearer token); NULL: none */
static char *api_key (int *bearer) {
  const char *s = cfg("mme\\.chat\\.apiKey", "");
  char *e;
  *bearer = 0;
  if (*s) return xstrdup(s);
  e = os_getenv("ANTHROPIC_API_KEY");
  if (e && *e) return e;
  free(e);
  e = os_getenv("ANTHROPIC_AUTH_TOKEN");
  if (e && *e) {
    *bearer = 1;
    return e;
  }
  free(e);
  return NULL;
}


static int has_key (void) {
  int bearer;
  char *k = api_key(&bearer);
  int r = k != NULL;
  free(k);
  return r;
}


/* mme.chat.baseUrl, else $ANTHROPIC_BASE_URL, else Anthropic's; no / at the end */
static char *base_url (void) {
  const char *s = cfg("mme\\.chat\\.baseUrl", "");
  char *u;
  size_t n;
  if (*s) u = xstrdup(s);
  else {
    u = os_getenv("ANTHROPIC_BASE_URL");
    if (u == NULL || *u == '\0') {
      free(u);
      u = xstrdup(CHAT_URL);
    }
  }
  n = strlen(u);
  while (n > 0 && u[n - 1] == '/') u[--n] = '\0';
  return u;
}


static char *curl_exe (void) {
#ifdef _WIN32
  char *root = os_getenv("SystemRoot");	/* Windows' own first, as the Extensions view does */
  if (root) {
    char *sys = path_join(root, "System32"), *p = path_join(sys, "curl.exe");
    free(root);
    free(sys);
    if (os_is_exec(p)) return p;
    free(p);
  }
#endif
  return find_program("curl");
}


/* name = "value" for curl's config file: \ and " escaped, as curl reads them back */
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


static void wipe_buf (Buf *b) {	/* it held the key */
  if (b->s) memset(b->s, 0, b->cap);
  buf_free(b);
}


static void job_close (void) {
  if (J.out >= 0) os_close(J.out);
  if (J.err >= 0) os_close(J.err);
  J.out = J.err = -1;
  buf_free(&J.line);
  buf_free(&J.body);
  buf_free(&J.errs);
  buf_free(&J.answer);
  free(J.error);
  J.error = NULL;
  J.on = 0;
}


/*
** curl started on the request body; 0 it runs. Its arguments name no
** secret: everything else goes in on its stdin.
*/
static int job_start (const char *body, size_t n, int inl, int nmsg) {
  char *exe = curl_exe(), *key, *url, *argv[12];
  int in[2], out[2], err[2], io[3], bearer, a = 0, fb;
  Buf cfgb, h;
  size_t done = 0;
  if (J.on) return -1;
  if (exe == NULL) {
    toast(1, "curl was not found: Chat needs it to reach Claude");
    return -1;
  }
  key = api_key(&bearer);
  if (key == NULL) {
    free(exe);
    return -1;
  }
  url = base_url();
  fb = json_bool(settings_get("mme\\.chat\\.fallbacks"), 1);
  buf_init(&cfgb);
  buf_init(&h);
  buf_printf(&h, "%s/v1/messages", url);
  cfg_put(&cfgb, "url", h.s, h.len);
  h.len = 0;
  buf_printf(&h, bearer ? "Authorization: Bearer %s" : "x-api-key: %s", key);
  cfg_put(&cfgb, "header", h.s, h.len);
  wipe_buf(&h);
  memset(key, 0, strlen(key));
  free(key);
  cfg_put(&cfgb, "header", "anthropic-version: 2023-06-01", 29);
  cfg_put(&cfgb, "header", "content-type: application/json", 30);
  if (fb) cfg_put(&cfgb, "header", "anthropic-beta: server-side-fallback-2026-07-01", 47);
  cfg_put(&cfgb, "data-binary", body, n);
  argv[a++] = exe;
  argv[a++] = (char *)"-sS";	/* no progress, but its errors */
  argv[a++] = (char *)"-N";	/* no buffering: the events as they come */
  argv[a++] = (char *)"-i";	/* the head too: the status, retry-after */
  argv[a++] = (char *)"--connect-timeout";
  argv[a++] = (char *)"30";
  argv[a++] = (char *)"-K";
  argv[a++] = (char *)"-";
  argv[a] = NULL;
  if (os_pipe(in) != 0) {
    wipe_buf(&cfgb);
    free(exe);
    free(url);
    return -1;
  }
  if (os_pipe(out) != 0) {
    os_close(in[0]);
    os_close(in[1]);
    wipe_buf(&cfgb);
    free(exe);
    free(url);
    return -1;
  }
  if (os_pipe(err) != 0) err[0] = err[1] = -1;
  io[0] = in[0];
  io[1] = out[1];
  io[2] = err[1];
  memset(&J, 0, sizeof(J));
  if (os_spawn(exe, argv, NULL, io, 3, &J.proc, &J.pid) != 0) {
    os_close(in[0]);
    os_close(in[1]);
    os_close(out[0]);
    os_close(out[1]);
    if (err[0] >= 0) {
      os_close(err[0]);
      os_close(err[1]);
    }
    wipe_buf(&cfgb);
    toast(1, "curl could not be started");
    free(exe);
    free(url);
    return -1;
  }
  os_close(in[0]);
  os_close(out[1]);
  if (err[1] >= 0) os_close(err[1]);
  while (done < cfgb.len) {	/* curl reads its config before anything else */
    long w = os_write(in[1], cfgb.s + done, cfgb.len - done);
    if (w <= 0) break;
    done += (size_t)w;
  }
  os_close(in[1]);
  wipe_buf(&cfgb);
  J.on = 1;
  J.inl = inl;
  J.out = out[0];
  J.err = err[0];
  J.head = 1;
  J.t0 = os_now_us();
  out_log("Chat", "POST %s/v1/messages (%s, %d message%s%s)", url, model(), nmsg, nmsg == 1 ? "" : "s",
          inl ? ", Inline Chat" : "");
  free(exe);
  free(url);
  return 0;
}


/* the request's body: the model, the system prompt, then the messages (added by body_msg) */
static void body_start (Buf *b, const char *system) {
  const char *m = model();
  buf_puts(b, "{\"model\": ");
  json_put_str(b, m, strlen(m));
  buf_printf(b, ", \"max_tokens\": %d, \"stream\": true, \"system\": ", CHAT_MAX_TOKENS);
  json_put_str(b, system, strlen(system));
  if (json_bool(settings_get("mme\\.chat\\.fallbacks"), 1)) buf_puts(b, ", \"fallbacks\": \"default\"");	/* a refusal is answered by another model */
  buf_puts(b, ", \"messages\": [");
}


static void body_msg (Buf *b, int *first, const char *role, const char *s, size_t n) {
  buf_puts(b, *first ? "{\"role\": \"" : ", {\"role\": \"");
  buf_puts(b, role);
  buf_puts(b, "\", \"content\": ");
  json_put_str(b, s, n);
  buf_putc(b, '}');
  *first = 0;
}


/*
** The talk as messages: the notices left out, Claude's empty turns (a
** stopped answer) too, and two turns of one side in a row made one, so the
** roles take turns as the API wants. It ends on the user's question.
*/
static int body_talk (Buf *b) {
  size_t i;
  int first = 1, count = 0, role = -1;
  Buf m;
  buf_init(&m);
  for (i = 0; i < C.n; i++) {
    const Turn *t = &C.t[i];
    const char *s = t->who == TU_USER ? t->sent : t->text.s;
    size_t n = t->who == TU_USER ? strlen(t->sent) : t->text.len;
    if (t->who == TU_NOTE || n == 0) continue;
    if (role >= 0 && role != t->who) {
      body_msg(b, &first, role == TU_USER ? "user" : "assistant", m.s, m.len);
      count++;
      m.len = 0;
    }
    if (m.len) buf_puts(&m, "\n\n");
    buf_putn(&m, s, n);
    role = t->who;
  }
  if (role >= 0) {
    body_msg(b, &first, role == TU_USER ? "user" : "assistant", m.s, m.len);
    count++;
  }
  buf_free(&m);
  buf_puts(b, "]}");
  return count;
}


/* an error's message from the API's body ({"error": {"message": ...}}), NULL none */
static char *api_message (const char *s, size_t n) {
  Json *j = json_parse(s, n);
  char *r = NULL;
  if (j) {
    const char *m = json_str(json_get(j, "error.message"), NULL);
    if (m && *m) r = xstrdup(m);
    json_free(j);
  }
  return r;
}


/* what went wrong, in words: a status that is not 200 */
static void http_error (char *out, size_t n) {
  char *m = api_message(J.body.s ? J.body.s : "", J.body.len);
  const char *said = m ? m : "";
  int s = J.status;
  if (s == 401)
    snprintf(out, n, "The API key was not accepted (401%s%s). Check `mme.chat.apiKey` in settings.json, "
             "or `ANTHROPIC_API_KEY`.",*said ? ": " : "", said);
  else if (s == 403) snprintf(out, n, "The API key may not do this (403%s%s).", *said ? ": " : "", said);
  else if (s == 429 && J.retry[0])
    snprintf(out, n, "Rate limited (429): try again in %s seconds.%s%s", J.retry, *said ? " " : "", said);
  else if (s == 429) snprintf(out, n, "Rate limited (429): try again in a moment.%s%s", *said ? " " : "", said);
  else if (s == 529) snprintf(out, n, "Anthropic's API is overloaded (529): try again in a moment.");
  else if (s >= 500) snprintf(out, n, "The API had a server error (%d)%s%s. Try again in a moment.", s, *said ? ": " : "", said);
  else snprintf(out, n, "The request was refused (%d)%s%s", s, *said ? ": " : "", said);
  free(m);
}


/* curl's own complaint (it could not connect ...), its first line without "curl: (7) " */
static void curl_error (char *out, size_t n) {
  const char *s = J.errs.s ? J.errs.s : "", *e;
  char *u = base_url();
  if (strncmp(s, "curl: ", 6) == 0) s += 6;
  if (*s == '(' && (e = strchr(s, ')')) != NULL) s = e + 1;
  while (*s == ' ') s++;
  e = strchr(s, '\n');
  snprintf(out, n, "Could not reach %s%s%.*s", u, *s ? ": " : ".", (int)(e ? (size_t)(e - s) : strlen(s)), s);
  if (e && e > s && e[-1] == '\r') out[strlen(out) - 1] = '\0';
  free(u);
}


static void inline_done (const char *text, size_t n);

/* curl is done: what came is put where it belongs, or what went wrong is told */
static void job_finish (void) {
  char msg[700];
  int code = os_wait(J.proc), inl = J.inl;
  Turn *t = C.n && C.t[C.n - 1].who == TU_CLAUDE ? &C.t[C.n - 1] : NULL;
  msg[0] = '\0';
  if (J.status == 0) curl_error(msg, sizeof(msg));
  else if (J.status != 200) http_error(msg, sizeof(msg));
  else if (J.error) snprintf(msg, sizeof(msg), "The answer broke off: %s", J.error);
  out_log("Chat", "HTTP %d%s%s, curl exit code %d, %lld ms", J.status, J.stop[0] ? ", stop_reason " : "", J.stop,
          code, (os_now_us() - J.t0) / 1000);
  if (msg[0]) out_log("Chat", "[error] %s", msg);
  if (inl) {
    IC.busy = 0;
    if (msg[0]) toast(2, "Inline Chat: %s", msg);
    else if (strcmp(J.stop, "refusal") == 0) toast(1, "Inline Chat: Claude declined to make this change.");
    else inline_done(J.answer.s ? J.answer.s : "", J.answer.len);
  }
  else {
    if (msg[0] && t && t->text.len == 0) {	/* nothing came: the turn is the notice */
      t->who = TU_NOTE;
      buf_puts(&t->text, msg);
    }
    else if (msg[0]) note(msg);
    else if (strcmp(J.stop, "refusal") == 0 && t && t->text.len == 0) {
      t->who = TU_NOTE;
      buf_puts(&t->text, "Claude declined to answer this request.");
    }
    else if (strcmp(J.stop, "refusal") == 0) note("Claude declined to go on with this answer.");
    else if (strcmp(J.stop, "max_tokens") == 0) note("The answer reached the maximum length and was cut off here.");
    else if (t && t->text.len == 0) {
      t->who = TU_NOTE;
      buf_puts(&t->text, "Claude gave no answer.");
    }
    C.stale = 1;
  }
  job_close();
}


/* Stop: curl is ended, what came so far stays */
static void job_cancel (void) {
  int inl = J.inl;
  if (!J.on) return;
  if (J.pid > 0) os_kill(J.pid, 9);
  os_wait(J.proc);
  out_log("Chat", "canceled");
  job_close();
  if (inl) IC.busy = 0;
  else if (C.n && C.t[C.n - 1].who == TU_CLAUDE && C.t[C.n - 1].text.len == 0) {
    C.t[C.n - 1].who = TU_NOTE;
    buf_puts(&C.t[C.n - 1].text, "Canceled.");
  }
  C.stale = 1;
}


/* one event's data: the text grows, the stop reason is kept */
static void on_data (const char *s, size_t n) {
  Json *j;
  const char *type;
  while (n && *s == ' ') {
    s++;
    n--;
  }
  if ((j = json_parse(s, n)) == NULL) return;
  type = json_str(json_get(j, "type"), "");
  if (strcmp(type, "content_block_delta") == 0 &&
      strcmp(json_str(json_get(j, "delta.type"), ""), "text_delta") == 0) {	/* thinking_delta, signature_delta: not shown */
    const Json *tx = json_get(j, "delta.text");
    if (tx && tx->type == J_STR) {
      if (J.inl) buf_putn(&J.answer, tx->str, tx->len);
      else if (C.n && C.t[C.n - 1].who == TU_CLAUDE) {
        buf_putn(&C.t[C.n - 1].text, tx->str, tx->len);
        C.stale = 1;
      }
    }
  }
  else if (strcmp(type, "message_delta") == 0) {
    const char *r = json_str(json_get(j, "delta.stop_reason"), NULL);
    if (r) snprintf(J.stop, sizeof(J.stop), "%s", r);
  }
  else if (strcmp(type, "error") == 0) {
    const char *m = json_str(json_get(j, "error.message"), "error");
    free(J.error);
    J.error = xstrdup(m);
  }
  json_free(j);
}


/* a line of curl's output: the head (HTTP/2 200, retry-after: 5), then the events */
static void on_line (const char *s, size_t n) {
  if (n && s[n - 1] == '\r') n--;
  if (J.head) {
    if (n >= 5 && memcmp(s, "HTTP/", 5) == 0) {
      const char *sp = memchr(s, ' ', n);
      J.status = sp ? atoi(sp + 1) : 0;
      J.proxy = n > 11 && m_strnicmp(s + n - 11, "established", 11) == 0;
    }
    else if (n == 0 && J.status) {	/* the head ended; a 100 Continue or a proxy's is followed by the real one */
      if (J.status < 200 || J.proxy) J.status = J.proxy = 0;
      else J.head = 0;
    }
    else if (n > 12 && m_strnicmp(s, "retry-after:", 12) == 0) {
      size_t k = 12;
      while (k < n && s[k] == ' ') k++;
      snprintf(J.retry, sizeof(J.retry), "%.*s", (int)(n - k), s + k);
    }
    return;
  }
  if (J.status != 200) {	/* a failure: its body says why */
    buf_putn(&J.body, s, n);
    buf_putc(&J.body, '\n');
    return;
  }
  if (n > 5 && memcmp(s, "data:", 5) == 0) on_data(s + 5, n - 5);	/* "event:" names the same type the data has */
}


static void feed (const char *s, size_t n) {
  size_t i, from = 0;
  for (i = 0; i < n; i++) {
    if (s[i] != '\n') continue;
    if (J.line.len) {
      buf_putn(&J.line, s + from, i - from);
      on_line(J.line.s, J.line.len);
      J.line.len = 0;
    }
    else on_line(s + from, i - from);
    from = i + 1;
  }
  if (from < n) buf_putn(&J.line, s + from, n - from);
}


/* while the editor waits for a key: what curl printed; 1 when the view must be drawn again */
int chat_idle (void) {
  char chunk[16384];
  int k, changed = 0;
  long n;
  if (!J.on) return 0;
  while (J.err >= 0 && os_wait_readable(J.err, 0) == 1) {
    n = os_read(J.err, chunk, sizeof(chunk));
    if (n <= 0) {
      os_close(J.err);
      J.err = -1;
      break;
    }
    buf_putn(&J.errs, chunk, (size_t)n);
  }
  for (k = 0; k < 64 && J.out >= 0 && os_wait_readable(J.out, 0) == 1; k++) {	/* not all at once: the keys come in between */
    n = os_read(J.out, chunk, sizeof(chunk));
    if (n <= 0) {
      os_close(J.out);
      J.out = -1;
      break;
    }
    feed(chunk, (size_t)n);
    changed = 1;
  }
  if (J.out < 0) {	/* it ended: the rest of what it said went to stderr */
    while (J.err >= 0) {
      n = os_read(J.err, chunk, sizeof(chunk));
      if (n <= 0) {
        os_close(J.err);
        J.err = -1;
        break;
      }
      buf_putn(&J.errs, chunk, (size_t)n);
    }
    if (J.line.len) {
      on_line(J.line.s, J.line.len);
      J.line.len = 0;
    }
    job_finish();
    changed = 1;
  }
  return changed || (J.on && !J.inl);	/* the spinner turns */
}

/* }================================================================== */


/*
** {==================================================================
** Asking
** ===================================================================
*/

/* a path to show and send: from the folder open when it is in it */
static const char *rel_path (const char *path) {
  const char *root = side_root();
  size_t n = root ? strlen(root) : 0;
  if (path == NULL) return "untitled";
  if (n && m_fnncmp(path, root, n) == 0 && path_is_sep(path[n])) return path + n + 1;
  return path;
}


/* the chip of the context: "main.c:10-20", or "main.c" for the lines shown */
static void chip_label (const EdCtx *c, char *out, size_t n) {
  const char *name = c->path ? path_basename(c->path) : "Untitled";
  if (!c->sel) snprintf(out, n, "%s", name);
  else if (c->y0 == c->y1) snprintf(out, n, "%s:%lu", name, (unsigned long)c->y0 + 1);
  else snprintf(out, n, "%s:%lu-%lu", name, (unsigned long)c->y0 + 1, (unsigned long)c->y1 + 1);
}


static const char no_key_msg[] =
  "Chat needs an Anthropic API key: put it in settings.json as \"mme.chat.apiKey\" "
  "(Chat: Set API Key... does it), or set ANTHROPIC_API_KEY in the environment.";
static const char no_key_md[] =	/* the same, for the talk: the names as code, so _ is no italics */
  "Chat needs an Anthropic API key: put it in settings.json as `mme.chat.apiKey` "
  "(**Chat: Set API Key...** does it), or set `ANTHROPIC_API_KEY` in the environment.";


/* Enter in the box: the question, with its context, goes */
static void send_question (void) {
  EdCtx c;
  Turn *u;
  Buf sent, body;
  size_t n = C.in.len;
  const char *s = C.in.s ? C.in.s : "";
  int nmsg;
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\n')) n--;
  while (n && (*s == ' ' || *s == '\n')) {
    s++;
    n--;
  }
  if (n == 0 || J.on) return;
  if (!has_key()) {
    note(no_key_md);
    return;
  }
  u = turn_add(TU_USER, s, n);
  buf_init(&sent);
  if (!C.no_ctx && editor_context(&c, 1)) {	/* VS Code's implicit context: the selection, else what is shown */
    char ref[256];
    chip_label(&c, ref, sizeof(ref));
    u->ref = xstrdup(ref);
    u->doc = c.doc;
    u->edits = c.edits;
    u->a = c.a;
    u->b = c.sel ? c.b : c.a;
    buf_puts(&sent, "<attachment path=\"");
    buf_puts(&sent, rel_path(c.path));
    buf_printf(&sent, "\" lines=\"%lu-%lu\" language=\"%s\" kind=\"%s\">\n", (unsigned long)c.y0 + 1,
               (unsigned long)c.y1 + 1, c.lang, c.sel ? "selection" : "visible lines");
    buf_puts(&sent, c.text ? c.text : "");
    if (sent.len && sent.s[sent.len - 1] != '\n') buf_putc(&sent, '\n');
    buf_puts(&sent, "</attachment>\n\n");
    editor_ctx_free(&c);
  }
  buf_putn(&sent, s, n);
  buf_putc(&sent, '\0');
  u->sent = buf_take(&sent);
  free(C.last);
  C.last = xstrndup(s, n);
  C.in.len = 0;
  C.at = 0;
  C.in_top = 0;
  turn_add(TU_CLAUDE, NULL, 0);
  buf_init(&body);
  body_start(&body, sys_chat);
  nmsg = body_talk(&body);
  if (job_start(body.s, body.len, 0, nmsg) != 0) {
    C.t[C.n - 1].who = TU_NOTE;
    buf_puts(&C.t[C.n - 1].text, "curl could not be started: Chat runs it to reach Claude.");
  }
  buf_free(&body);
}


/* New Chat: the talk goes (and the answer on its way) */
static void new_chat (void) {
  if (J.on && !J.inl) job_cancel();
  talk_clear();
  C.in.len = 0;
  C.at = 0;
  C.in_top = 0;
}

/* }================================================================== */


/*
** {==================================================================
** Proposing code in the editor: Apply, and Inline Chat's answer
** ===================================================================
*/

/*
** text in place of a .. b of d, shown as a diff to accept. A selection of
** whole lines ends at the start of the next one; its last newline stays
** out of it, so the lines under it are not touched.
*/
static int propose (const Doc *d, unsigned long edits, Pos a, Pos b, const char *text, size_t n) {
  char *t;
  int r;
  while (n && text[n - 1] == '\n') n--;
  if (b.x == 0 && b.y > a.y) {
    b.y--;
    b.x = d->row[b.y].len;
  }
  t = xstrndup(text, n);
  r = editor_propose(d, edits, a, b, t);
  free(t);
  return r;
}


/* Apply: the block in place of the selection, else of what the question was asked about; 1 shown */
static int apply_block (size_t i) {
  EdCtx c;
  const char *code = C.code[i];
  const Turn *q = C.fence_turn[i] < C.n ? &C.t[C.fence_turn[i]] : NULL;
  if (editor_context(&c, 0)) {
    if (c.sel && propose(c.doc, c.edits, c.a, c.b, code, strlen(code))) {
      editor_ctx_free(&c);
      toast(0, "Ctrl+Enter accepts the change, Esc discards it");
      return 1;
    }
    if (q && q->doc == c.doc && q->edits == c.edits && pos_cmp(q->a, q->b) < 0 &&
        propose(q->doc, q->edits, q->a, q->b, code, strlen(code))) {
      editor_ctx_free(&c);
      toast(0, "Ctrl+Enter accepts the change, Esc discards it");
      return 1;
    }
    editor_ctx_free(&c);
  }
  toast(0, "Select the code to replace in the editor, then Apply");
  return 0;
}


/* Copy, Insert, Apply; 2 when the editor is to get the keys (a diff to accept) */
static int code_action (int what, size_t i) {
  const char *code;
  if (i >= C.nfence) return 1;
  code = C.code[i];
  if (what == HIT_COPY) {
    clip_set(code, strlen(code));
    toast(0, "Copied");
  }
  else if (what == HIT_INSERT) {
    if (!editor_put(code, strlen(code))) toast(0, "Open a file to insert the code into");
  }
  else if (apply_block(i)) return 2;
  return 1;
}


/* Claude's code, fences taken off if it wrote them anyway */
static void inline_done (const char *s, size_t n) {
  char ch;
  const char *e;
  if (n && (e = memchr(s, '\n', n)) != NULL && is_fence(s, (size_t)(e - s), &ch)) {
    n -= (size_t)(e + 1 - s);
    s = e + 1;
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\r')) n--;
    if (n >= 3 && s[n - 1] == ch && s[n - 2] == ch && s[n - 3] == ch) {
      n -= 3;
      while (n && s[n - 1] != '\n') n--;	/* the fence's own line */
    }
  }
  if (n == 0 || (n == 1 && *s == '\n')) {
    toast(0, "Inline Chat: Claude proposed no change");
    return;
  }
  if (!propose(IC.doc, IC.edits, IC.a, IC.b, s, n))
    toast(1, "Inline Chat: the file changed while Claude worked; its answer was dropped");
}


/* Inline Chat's question: the file around the selection, the place marked */
static void inline_send (const EdCtx *c, const char *prompt) {
  Buf q, body;
  size_t len, y0, y1;
  char *s;
  int first = 1;
  Pos a = c->a, b = c->sel ? c->b : c->a;
  const Doc *d = c->doc;
  y0 = a.y > NEAR_LINES ? a.y - NEAR_LINES : 0;
  y1 = b.y + NEAR_LINES < d->n ? b.y + NEAR_LINES : d->n - 1;
  buf_init(&q);
  buf_printf(&q, "The file is %s (language %s). ", rel_path(c->path), c->lang);
  if (c->sel) buf_puts(&q, "The part to change is between <selection> and </selection>.\n<file>\n");
  else buf_puts(&q, "The cursor is at <cursor/>.\n<file>\n");
  {
    Pos p0;
    p0.y = y0;
    p0.x = 0;
    s = doc_text(d, p0, a, &len);
    buf_putn(&q, s, len);
    free(s);
    if (c->sel) {
      buf_puts(&q, "<selection>");
      s = doc_text(d, a, b, &len);
      buf_putn(&q, s, len);
      free(s);
      buf_puts(&q, "</selection>");
    }
    else buf_puts(&q, "<cursor/>");
    {
      Pos p1;
      p1.y = y1;
      p1.x = d->row[y1].len;
      s = doc_text(d, b, p1, &len);
      buf_putn(&q, s, len);
      free(s);
    }
  }
  buf_puts(&q, "\n</file>\n\n");
  buf_puts(&q, prompt);
  buf_puts(&q, c->sel ? "\n\nReply with only the code that replaces the selection."
                      : "\n\nReply with only the code to insert at <cursor/>.");
  buf_init(&body);
  body_start(&body, sys_inline);
  body_msg(&body, &first, "user", q.s, q.len);
  buf_puts(&body, "]}");
  IC.doc = d;
  IC.edits = c->edits;
  IC.a = a;
  IC.b = b;
  snprintf(IC.prompt, sizeof(IC.prompt), "%s", prompt);
  if (job_start(body.s, body.len, 1, 1) == 0) IC.busy = 1;
  buf_free(&body);
  buf_free(&q);
}

/* }================================================================== */


/*
** {==================================================================
** Inline Chat's box, over the lines it is about
** ===================================================================
*/

/*
** The box: its input on the first row, what to press (or that Claude
** works) on the second. Over the line y when there is room above it, else
** under it. The cursor's column when it is being typed in, else -1.
*/
static int inline_box (int x, int y, int w, int top, int bottom, const char *text, int typing) {
  int by = y - 3 >= top ? y - 3 : y + 1, i, cx;
  const char *hint;
  char h[128];
  if (w > 80) w = 80;
  if (w < 20 || by + 3 > bottom) return -1;
  scr_box(x, by, w, 3, S_BOX);
  for (i = 0; i < w; i++) scr_put_rgb(x + i, by, 0x2500, ui_color(C_ACCENT), ui_color(C_MENU_BG), 0);	/* its top edge, lit */
  scr_fill(x + 1, by + 1, w - 2, S_INPUT);
  scr_put_rgb(x + 2, by + 1, ICON_SPARKLE, CLAUDE_RGB, ui_color(C_INPUT_BG), 0);
  if (*text == '\0' && typing)
    scr_putsw(x + 4, by + 1, w - 6, IC.a.y == IC.b.y && IC.a.x == IC.b.x ? "Ask Claude to write code here"
                                                                         : "Ask Claude to edit the selection", S_INPUT_HINT);
  {
    const char *t = text;	/* the end of a long one, where the typing goes on */
    while (*t && (int)str_cols(t) > w - 7) t++;
    while (((unsigned char)*t & 0xC0) == 0x80) t++;
    cx = x + 4 + scr_putsw(x + 4, by + 1, w - 6, t, typing ? S_INPUT_ON : S_INPUT_HINT);
  }
  if (typing) hint = "Enter to send, Esc to close";
  else {
    snprintf(h, sizeof(h), "%s Generating...  Esc to stop", ui_spinner());
    hint = h;
  }
  scr_putsw(x + 2, by + 2, w - 4, hint, S_BOX_DIM);
  return typing ? cx : -1;
}


/* while its answer comes: the line it is for (0: none, or another text) */
int chat_inline_at (const Doc *d, Pos *at) {
  if (!IC.busy || IC.doc != d) return 0;
  *at = IC.a;
  return 1;
}


void chat_inline_draw (int x, int y, int w, int top, int bottom) {
  inline_box(x, y, w, top, bottom, IC.prompt, 0);
}


/* under Inline Chat's diff: VS Code's Accept and Discard */
void chat_inline_bar (int x, int y, int w) {
  int cx = x + 1, k, x0;
  if (w < 40) return;
  cx += scr_putsw(cx, y, 20, " Accept ", S_TEXT);
  for (k = x + 1; k < cx; k++) {	/* the primary button: white on blue */
    scr_set_bg(k, y, 0x0078D4);
    scr_set_fg(k, y, 0xFFFFFF);
  }
  cx += scr_putsw(cx, y, 20, " Ctrl+Enter", S_GUTTER) + 2;
  x0 = cx;
  cx += scr_putsw(cx, y, 20, " Discard ", S_TEXT);
  for (k = x0; k < cx; k++) scr_set_bg(k, y, ui_color(C_INPUT_BG));
  scr_putsw(cx, y, 20, " Esc", S_GUTTER);
}


/* the prompt, typed in the box over the selection; NULL: Esc */
static char *inline_ask (const EdCtx *c) {
  char text[512];
  size_t len = 0;
  text[0] = '\0';
  IC.a = c->a;
  IC.b = c->sel ? c->b : c->a;
  for (;;) {
    int k, code, cx;
    ui_background();
    cx = inline_box(c->x, c->y, c->w, c->top, c->bottom, text, 1);
    if (cx < 0) {	/* no room: the quick input at the top instead */
      char *s = ask_text(c->sel ? "Ask Claude to edit the selection" : "Ask Claude to write code here", NULL);
      return s;
    }
    scr_cursor(cx, c->y - 3 >= c->top ? c->y - 2 : c->y + 2);
    scr_flush();
    k = term_key(-1);
    code = KEY_CODE(k);
    if (k == K_NONE) continue;
    if (code == K_ESC) return NULL;
    if (code == K_ENTER) return len ? xstrdup(text) : NULL;
    if (code == K_BS) {
      while (len > 0 && ((unsigned char)text[len - 1] & 0xC0) == 0x80) len--;
      if (len > 0) len--;
      text[len] = '\0';
    }
    else if (code == K_PASTE) {
      Buf b;
      size_t i;
      buf_init(&b);
      term_paste(&b);
      for (i = 0; i < b.len && len + 1 < sizeof(text); i++) text[len++] = b.s[i] == '\n' ? ' ' : b.s[i];
      text[len] = '\0';
      buf_free(&b);
    }
    else if (IS_TEXT(k) && len + 5 < sizeof(text)) {
      len += (size_t)utf8_encode((uint32_t)k, text + len);
      text[len] = '\0';
    }
    else if (code == K_MOUSE && term_mouse.press && !term_mouse.drag && term_mouse.button == 0) {
      int by = c->y - 3 >= c->top ? c->y - 3 : c->y + 1;
      if (term_mouse.y < by || term_mouse.y > by + 2) return NULL;	/* a click outside closes it */
    }
  }
}


static void inline_start (void) {
  EdCtx c;
  char *prompt;
  if (!editor_context(&c, 0)) {
    toast(0, "Inline Chat works in a text editor: open a file first");
    return;
  }
  if (!has_key()) {
    toast(1, "%s", no_key_msg);
    editor_ctx_free(&c);
    return;
  }
  if (J.on) {
    toast(0, "Claude is still answering: wait, or stop it with Esc");
    editor_ctx_free(&c);
    return;
  }
  prompt = inline_ask(&c);
  if (prompt && *prompt) inline_send(&c, prompt);
  free(prompt);
  editor_ctx_free(&c);
}


/* a key in the editor while Inline Chat works: Esc stops it */
int chat_editor_key (int k) {
  if (!IC.busy || KEY_CODE(k) != K_ESC || (k & (KM_CTRL | KM_ALT | KM_SHIFT))) return 0;
  job_cancel();
  toast(0, "Inline Chat stopped");
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** The view: drawing
** ===================================================================
*/

typedef struct Seg {	/* a row of the box to type in: bytes a .. b */
  size_t a, b;
} Seg;

/* the rows the text takes at w columns: cut at a newline, and where it is full */
static int input_rows (int w, Seg *seg, int max) {
  const char *s = C.in.s ? C.in.s : "";
  size_t i = 0, n = C.in.len, start = 0;
  int cols = 0, k = 0;
  while (i < n) {
    size_t len;
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    int cw = cp == '\n' ? 0 : uc_width(cp);
    if (len == 0) len = 1;
    if (cp == '\n') {
      if (k < max) {
        seg[k].a = start;
        seg[k].b = i;
      }
      k++;
      start = i + len;
      cols = 0;
    }
    else if (cols + cw > w && cols > 0) {
      if (k < max) {
        seg[k].a = start;
        seg[k].b = i;
      }
      k++;
      start = i;
      cols = cw;
    }
    else cols += cw;
    i += len;
  }
  if (k < max) {
    seg[k].a = start;
    seg[k].b = n;
  }
  return k + 1;
}


/* the row the cursor is on, and its column there */
static int cursor_row (const Seg *seg, int n, int *col) {
  int r;
  for (r = n - 1; r > 0 && seg[r].a > C.at; r--)
    ;
  {	/* columns, not characters */
    size_t i = seg[r].a, len;
    int c = 0;
    while (i < C.at) {
      c += uc_width(utf8_decode(C.in.s + i, C.at - i, &len));
      i += len ? len : 1;
    }
    *col = c;
  }
  return r;
}


/* a line in the middle of w columns, dim or not */
static void put_center (int x, int y, int w, const char *s, int st) {
  int n = (int)str_cols(s);
  scr_putsw(x + (n < w ? (w - n) / 2 : 0), y, w, s, st);
}


/* words wrapped at w columns, each row in the middle; the rows used */
static int put_wrapped (int x, int y, int w, int maxrows, const char *s, int st) {
  int rows = 0;
  while (*s && rows < maxrows) {
    const char *e = s, *cut = NULL;
    char line[256];
    int cols = 0;
    while (*e && *e != '\n') {
      size_t len;
      uint32_t cp = utf8_decode(e, strlen(e), &len);
      if (cp == ' ') cut = e;
      if (cols + uc_width(cp) > w) break;
      cols += uc_width(cp);
      e += len ? len : 1;
    }
    if (*e && *e != '\n' && cut && cut > s) e = cut;
    snprintf(line, sizeof(line), "%.*s", (int)(e - s), s);
    put_center(x, y + rows, w, line, st);
    rows++;
    s = e;
    while (*s == ' ' || *s == '\n') s++;
  }
  return rows;
}


/* no talk yet: what Chat is, and how to give it a key when there is none */
static void draw_welcome (int x, int y, int w, int h) {
  int r = y + (h > 12 ? h / 4 : 1), end = y + h;
  scr_put_rgb(x + w / 2, r, ICON_SPARKLE, CLAUDE_RGB, ui_color(C_SIDE_BG), 0);
  r += 2;
  if (r < end) put_center(x, r++, w, "Ask Claude", S_SIDE_TITLE);
  r++;
  if (r < end)
    r += put_wrapped(x + 2, r, w - 4, end - r, "Claude can make mistakes, so check what it writes.", S_SIDE_DIM);
  r++;
  if (!has_key() && r < end) {
    r += put_wrapped(x + 2, r, w - 4, end - r, no_key_msg, S_SIDE);
    r++;
    if (r < end && C.nhit < (int)(sizeof(C.hit) / sizeof(C.hit[0]))) {
      const char *l = "Set API Key...";
      int n = (int)str_cols(l), lx = x + (w - n) / 2;
      scr_putsw(lx, r, n, l, S_SIDE);
      scr_underline(lx, r, n, ui_color(C_ICON_BLUE));
      C.hit[C.nhit].y = r;
      C.hit[C.nhit].x0 = lx;
      C.hit[C.nhit].x1 = lx + n;
      C.hit[C.nhit].what = HIT_KEY;
      C.nhit++;
    }
  }
  else if (r < end) {
    const char *k = cmd_keys(CMD_INLINE_CHAT);
    char t[200];
    snprintf(t, sizeof(t), "The selection, or the lines the editor shows, goes with the question. "
             "%s in the editor asks for a change right there.", *k ? k : "Inline Chat: Start");
    put_wrapped(x + 2, r, w - 4, end - r, t, S_SIDE_DIM);
  }
}


/* Copy, Insert and Apply on the top row of each code block shown */
static void draw_block_actions (int x, int y, int w, int h, size_t top) {
  size_t i, total = md_rows(&C.doc, w);
  static const char *const label[] = {"Copy", "Insert", "Apply"};
  for (i = 0; i < C.nfence; i++) {
    size_t row = md_row_of_line(&C.doc, w, C.fence[i]);
    int k, cx, sy;
    while (row + 1 < total && md_line_of_row(&C.doc, w, row + 1) == C.fence[i]) row++;	/* the blank row before it is the fence's too */
    if (row < top || row >= top + (size_t)h) continue;
    sy = y + (int)(row - top);
    cx = x + w - 3;
    for (k = 2; k >= 0; k--) {
      int n = (int)strlen(label[k]);
      cx -= n;
      if (cx < x + 4 || C.nhit >= (int)(sizeof(C.hit) / sizeof(C.hit[0]))) break;
      scr_putsw(cx, sy, n, label[k], S_TEXT);
      {
        int q;
        for (q = 0; q < n; q++) {
          scr_set_bg(cx + q, sy, ui_color(C_INPUT_BG));
          scr_set_fg(cx + q, sy, ui_color(C_ICON_BLUE));
        }
      }
      C.hit[C.nhit].y = sy;
      C.hit[C.nhit].x0 = cx;
      C.hit[C.nhit].x1 = cx + n;
      C.hit[C.nhit].what = HIT_COPY + k;
      C.hit[C.nhit].i = i;
      C.nhit++;
      cx -= 2;
    }
  }
}


/* the box's edge: lit while it has the keys */
static void box_edge (int x, int y, uint32_t ch, int focus) {
  scr_put_rgb(x, y, ch, ui_color(focus ? C_ACCENT : C_BORDER), ui_color(C_SIDE_BG), 0);
}


void chat_draw (int x, int y, int w, int h, int focus) {
  EdCtx c;
  Seg seg[64];
  int i, has_ctx, nrows, rows, crow, ccol, bh, by, iw, busy = J.on && !J.inl;
  C.x = x;
  C.y = y;
  C.w = w;
  C.h = h;
  C.nhit = 0;
  for (i = 0; i < h; i++) scr_fill(x, y + i, w, S_SIDE);
  if (h < 1 || w < 16) return;
  /* the title: CHAT, New Chat and close */
  scr_puts(x + 2, y, "CHAT", S_SIDE_HEAD);
  C.new_x = x + w - 5;
  C.close_x = x + w - 3;
  scr_put(C.new_x, y, ICON_ADD, S_SIDE);
  scr_put(C.close_x, y, ICON_CLOSE, S_SIDE);
  if (h < 9) return;
  /* the box to type in, at the bottom */
  has_ctx = editor_context(&c, 0);
  iw = w - 6;
  nrows = input_rows(iw, seg, 64);
  rows = nrows < INPUT_ROWS ? nrows : INPUT_ROWS;
  crow = cursor_row(seg, nrows < 64 ? nrows : 64, &ccol);
  if (crow < C.in_top) C.in_top = crow;
  if (crow >= C.in_top + rows) C.in_top = crow - rows + 1;
  if (C.in_top > nrows - rows) C.in_top = nrows - rows;
  if (C.in_top < 0) C.in_top = 0;
  bh = 2 + (has_ctx ? 1 : 0) + rows + 1;
  by = y + h - bh;
  C.box_y0 = by;
  C.box_y1 = by + bh;
  /* the talk */
  C.tr_y = y + 1;
  C.tr_h = by - C.tr_y;
  if (C.n == 0) draw_welcome(x, C.tr_y, w, C.tr_h);
  else if (C.tr_h > 0) {
    size_t total;
    if (C.stale || busy) build_doc(busy);
    total = md_rows(&C.doc, w);
    if (C.follow) C.top = total > (size_t)C.tr_h ? total - (size_t)C.tr_h : 0;
    md_draw(&C.doc, x, C.tr_y, w, C.tr_h, &C.top);
    if (C.top + (size_t)C.tr_h >= total) C.follow = 1;
    draw_block_actions(x, C.tr_y, w, C.tr_h, C.top);
    side_bar(x, C.tr_y, w, C.tr_h, total, C.top, (size_t)C.tr_h);
  }
  /* the box */
  box_edge(x + 1, by, 0x256D, focus);
  box_edge(x + w - 2, by, 0x256E, focus);
  box_edge(x + 1, by + bh - 1, 0x2570, focus);
  box_edge(x + w - 2, by + bh - 1, 0x256F, focus);
  for (i = x + 2; i < x + w - 2; i++) {
    box_edge(i, by, 0x2500, focus);
    box_edge(i, by + bh - 1, 0x2500, focus);
  }
  for (i = by + 1; i < by + bh - 1; i++) {
    box_edge(x + 1, i, 0x2502, focus);
    box_edge(x + w - 2, i, 0x2502, focus);
    scr_fill(x + 2, i, w - 4, S_INPUT);
  }
  i = by + 1;
  C.chip_y = -1;
  if (has_ctx) {	/* the implicit context: its file's icon and name, the eye that leaves it out */
    char label[256];
    int st, off = C.no_ctx, cx = x + 3;
    uint32_t ic = file_icon(c.path ? path_basename(c.path) : "untitled", &st);
    chip_label(&c, label, sizeof(label));
    cx += scr_put(cx, i, ic, off ? S_INPUT_HINT : st) + 1;
    cx += scr_putsw(cx, i, x + w - 6 - cx, label, off ? S_INPUT_HINT : S_INPUT);
    if (cx + 2 < x + w - 6) scr_putsw(cx + 1, i, x + w - 6 - cx - 1, c.sel ? "Current selection" : "Current file", S_INPUT_HINT);
    scr_put(x + w - 4, i, off ? ICON_EYE_OFF : ICON_EYE, S_INPUT_HINT);
    C.chip_y = i;
    i++;
    editor_ctx_free(&c);
  }
  C.in_y = i;
  C.in_rows = rows;
  if (C.in.len == 0) scr_putsw(x + 3, i, iw, "Ask Claude", S_INPUT_HINT);
  else {
    int r;
    for (r = 0; r < rows && C.in_top + r < nrows && C.in_top + r < 64; r++) {
      const Seg *sg = &seg[C.in_top + r];
      char *t = xstrndup(C.in.s + sg->a, sg->b - sg->a);
      scr_putsw(x + 3, i + r, iw, t, S_INPUT_ON);
      free(t);
    }
  }
  i += rows;
  {	/* the model, and send (or stop while it answers) */
    const char *m = model();
    int mw = (int)str_cols(m);
    C.send_x = x + w - 4;
    C.send_y = i;
    if (x + w - 6 - mw > x + 3) scr_putsw(x + w - 6 - mw, i, mw, m, S_INPUT_HINT);
    if (busy) scr_put_rgb(C.send_x, i, ICON_STOP, 0xF14C4C, ui_color(C_INPUT_BG), 0);
    else scr_put(C.send_x, i, ICON_SEND, C.in.len ? S_INPUT_ON : S_INPUT_HINT);
  }
  if (focus && crow >= C.in_top && crow < C.in_top + rows) scr_cursor(x + 3 + ccol, C.in_y + crow - C.in_top);
}

/* }================================================================== */


/*
** {==================================================================
** The view: keys and the mouse
** ===================================================================
*/

static void in_insert (const char *s, size_t n) {
  Buf b;
  buf_init(&b);
  buf_putn(&b, C.in.s ? C.in.s : "", C.at);
  buf_putn(&b, s, n);
  buf_putn(&b, C.in.s ? C.in.s + C.at : "", C.in.len - C.at);
  buf_free(&C.in);
  C.in = b;
  C.at += n;
}


static size_t prev_char (size_t at) {
  if (at == 0) return 0;
  at--;
  while (at > 0 && ((unsigned char)C.in.s[at] & 0xC0) == 0x80) at--;
  return at;
}


static size_t next_char (size_t at) {
  if (at >= C.in.len) return C.in.len;
  at++;
  while (at < C.in.len && ((unsigned char)C.in.s[at] & 0xC0) == 0x80) at++;
  return at;
}


/* Up / Down in the box: the row above or below, at the same column */
static int in_vertical (int d) {
  Seg seg[64];
  int n = input_rows(C.w > 6 ? C.w - 6 : 10, seg, 64), col, r, c = 0;
  size_t i;
  if (n > 64) n = 64;
  r = cursor_row(seg, n, &col) + d;
  if (r < 0 || r >= n) return 0;
  for (i = seg[r].a; i < seg[r].b;) {
    size_t len;
    int cw = uc_width(utf8_decode(C.in.s + i, seg[r].b - i, &len));
    if (c + cw > col) break;
    c += cw;
    i += len ? len : 1;
  }
  C.at = i;
  return 1;
}


static void scroll_talk (int d) {
  size_t total = C.doc_ok ? md_rows(&C.doc, C.w) : 0;
  if (d < 0) {
    C.follow = 0;
    C.top = C.top > (size_t)-d ? C.top - (size_t)-d : 0;
  }
  else {
    C.top += (size_t)d;
    if (C.top + (size_t)C.tr_h >= total) C.follow = 1;
  }
}


int chat_key (int k) {
  int code = KEY_CODE(k), mods = k & (KM_CTRL | KM_ALT | KM_SHIFT);
  if (code == K_ESC && !mods) {	/* Esc stops the answer; else the editor gets the keys */
    if (J.on && !J.inl) {
      job_cancel();
      return 1;
    }
    return 0;
  }
  if (code == K_ENTER && !mods) {
    send_question();
    return 1;
  }
  if (code == K_ENTER && (mods & (KM_SHIFT | KM_ALT))) {	/* Shift+Enter: a new line in the question */
    in_insert("\n", 1);
    return 1;
  }
  if (k == CTRL('l') || k == ('l' | KM_CTRL)) {	/* VS Code's New Chat in the view */
    new_chat();
    return 1;
  }
  if (code == K_PASTE) {
    Buf b;
    buf_init(&b);
    term_paste(&b);
    in_insert(b.s ? b.s : "", b.len);
    buf_free(&b);
    return 1;
  }
  if (k == CTRL('v')) {
    size_t n;
    const char *s = clip_get(&n);
    if (s) in_insert(s, n);
    return 1;
  }
  if (IS_TEXT(k)) {
    char u[4];
    in_insert(u, (size_t)utf8_encode((uint32_t)k, u));
    return 1;
  }
  if (mods & (KM_CTRL | KM_ALT)) {
    if (code == K_UP || code == K_DOWN) {	/* Ctrl+Up / Down: the talk scrolls */
      scroll_talk(code == K_UP ? -1 : 1);
      return 1;
    }
    return 0;
  }
  switch (code) {
    case K_BS:
      if (C.at > 0) {
        size_t p = prev_char(C.at);
        memmove(C.in.s + p, C.in.s + C.at, C.in.len - C.at);
        C.in.len -= C.at - p;
        C.at = p;
      }
      return 1;
    case K_DEL:
      if (C.at < C.in.len) {
        size_t q = next_char(C.at);
        memmove(C.in.s + C.at, C.in.s + q, C.in.len - q);
        C.in.len -= q - C.at;
      }
      return 1;
    case K_LEFT: C.at = prev_char(C.at); return 1;
    case K_RIGHT: C.at = next_char(C.at); return 1;
    case K_HOME:
      while (C.at > 0 && C.in.s[C.at - 1] != '\n') C.at--;
      return 1;
    case K_END:
      while (C.at < C.in.len && C.in.s[C.at] != '\n') C.at++;
      return 1;
    case K_UP:
      if (C.in.len == 0 && C.last) {	/* the last question again, as VS Code's history */
        in_insert(C.last, strlen(C.last));
        return 1;
      }
      if (!in_vertical(-1)) scroll_talk(-1);
      return 1;
    case K_DOWN:
      if (!in_vertical(1)) scroll_talk(1);
      return 1;
    case K_PGUP: scroll_talk(-(C.tr_h > 2 ? C.tr_h - 1 : 1)); return 1;
    case K_PGDN: scroll_talk(C.tr_h > 2 ? C.tr_h - 1 : 1); return 1;
  }
  return 0;
}


/* a click or the wheel in the view (the caller gave it the keys); 2: the editor is to have them */
int chat_mouse (const Mouse *m) {
  int i;
  if (m->wheel) {
    if (m->y >= C.tr_y && m->y < C.tr_y + C.tr_h) scroll_talk(m->wheel * wheel_step(m->mods));
    return 1;
  }
  if (m->button != 0 || !m->press || m->drag) return 1;
  if (m->y == C.y && m->x == C.new_x) {
    new_chat();
    return 1;
  }
  if (m->y == C.y && m->x == C.close_x) {
    C.shown = 0;
    return 1;
  }
  if (m->y == C.chip_y && C.chip_y >= 0) {	/* the context chip: in or out */
    C.no_ctx = !C.no_ctx;
    return 1;
  }
  if (m->y == C.send_y && m->x >= C.send_x - 1 && m->x <= C.send_x + 1) {
    if (J.on && !J.inl) job_cancel();
    else send_question();
    return 1;
  }
  for (i = 0; i < C.nhit; i++)
    if (m->y == C.hit[i].y && m->x >= C.hit[i].x0 && m->x < C.hit[i].x1) {
      if (C.hit[i].what == HIT_KEY) chat_command(CMD_CHAT_SET_KEY);
      else return code_action(C.hit[i].what, C.hit[i].i);
      return 1;
    }
  if (m->y >= C.in_y && m->y < C.in_y + C.in_rows) {	/* in the box: the cursor goes there */
    Seg seg[64];
    int n = input_rows(C.w - 6, seg, 64), r = C.in_top + m->y - C.in_y, c = 0, col = m->x - (C.x + 3);
    size_t k;
    if (n > 64) n = 64;
    if (r >= n) r = n - 1;
    for (k = seg[r].a; k < seg[r].b;) {
      size_t len;
      int cw = uc_width(utf8_decode(C.in.s + k, seg[r].b - k, &len));
      if (c + cw > col) break;
      c += cw;
      k += len ? len : 1;
    }
    C.at = k;
  }
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** The secondary side bar, and the commands
** ===================================================================
*/

int chat_shown (void) {
  return C.shown;
}


/* its columns, out of the editor area's; 0: no room for it */
int chat_width (int area_w) {
  int w = area_w * 2 / 5;
  if (w > 64) w = 64;
  if (w < 32) w = 32;
  return area_w - w - 1 >= 30 ? w : 0;
}


int chat_command (int cmd) {
  switch (cmd) {
    case CMD_CHAT_OPEN:
      C.shown = 1;
      return 1;
    case CMD_CHAT_TOGGLE:
      C.shown = !C.shown;
      return C.shown ? 1 : 2;
    case CMD_CHAT_NEW:
      new_chat();
      C.shown = 1;
      return 1;
    case CMD_CHAT_CLEAR:
      new_chat();
      return C.shown ? 1 : 0;
    case CMD_CHAT_STOP:
      if (J.on) job_cancel();
      return 0;
    case CMD_CHAT_CONTEXT:
      C.no_ctx = !C.no_ctx;
      toast(0, "The current file %s with the question", C.no_ctx ? "no longer goes" : "goes");
      return 0;
    case CMD_CHAT_SET_KEY: {
      char *k = ask_text("Anthropic API key (it goes into settings.json as \"mme.chat.apiKey\")", NULL);
      if (k && *k) {
        settings_put("mme.chat.apiKey", k);
        settings_load();
        toast(0, "The API key was saved: Chat can ask Claude");
      }
      if (k) memset(k, 0, strlen(k));
      free(k);
      return C.shown ? 1 : 0;
    }
    case CMD_INLINE_CHAT:
      inline_start();
      return 2;
  }
  return 0;
}

/* }================================================================== */
