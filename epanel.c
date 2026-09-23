/*
** epanel.c - the panel under the editor: VS Code's integrated terminal
**
** The shell runs behind a pseudo terminal and its output goes through the
** very same core that mmc-term is made of (tpty.c, tvt.c, tgrid.c, copied
** from mmc as they are); the grid it keeps is drawn in the panel with
** VS Code's terminal colors. Keys go back to the shell as a terminal
** would send them.
**
** Terminals come in groups: a split puts one more next to the one in front,
** in its group, and the panel shows the group in front side by side. With
** more than one terminal the tabs list is on the right, like VS Code's.
**
** Shell integration, like VS Code's: the shell marks its prompts and
** commands with OSC 633 (or FinalTerm's 133); tvt.c leaves those, so they
** are read here on the way to it. Each command gets a circle by its prompt
** (blue: it went well, red: it failed), and they are what Run Recent
** Command, Go to Recent Directory and Ctrl+Up / Ctrl+Down go by.
*/

#include "mme.h"
#include "mterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* a command shell integration told of: its prompt's line, and how it went */
typedef struct Mark {
  long line;	/* the prompt's line, as Term.base counts */
  int ran;	/* a command was run from it */
  int exit;	/* its exit code; -1 running (or not said), -2 ended, the code not said */
  char *cmd;	/* what was run, or NULL */
} Mark;

enum { SH_OTHER, SH_CMD, SH_PS, SH_BASH };	/* the shells shell integration knows */

/* one terminal: the shell, its screen */
typedef struct Term {
  Pty *pty;
  Grid *g;
  Vt vt;
  int cols, rows;
  char title[128];
  char name[64];	/* the shell's name, for its tab */
  int grp;	/* the terminals of a group are shown side by side */
  int x, y, w, h;	/* where it was drawn; w 0: not shown */
  int task;	/* a task runs in it: its id (etask.c reads its output) */
  int done;	/* the task ended: a key closes it */
  int kind;	/* SH_*: how to quote a path for it */
  long base;	/* the number of screen row 0: every line that went up into the scrollback counts */
  int sb_len0, sb_head0;	/* the scrollback when base was counted last */
  int ost;	/* the OSC scanner: 0 text, 1 after ESC, 2 in an OSC, 3 ESC in an OSC */
  Buf osc;	/* the OSC being read */
  Mark *mark;	/* the commands, the oldest first */
  int nmark, capmark;
  long b_line;	/* 633;B: where the command being typed starts */
  int b_col, has_b;
  char *cmdline;	/* 633;E: the command line, as the shell said it */
  char cwd[1024];	/* the shell's folder (633;P;Cwd=, OSC 7); "" not known */
  int color;	/* Change Color: 0 none, else an ANSI color + 1 */
  uint32_t icon;	/* Change Icon: 0 the terminal codicon */
} Term;

#define MAX_TERM	16

static Term *g_term[MAX_TERM];
static int g_n, g_cur, g_next_grp;

/* the selection, made with the mouse; one, in one terminal */
static struct {
  Term *t;	/* the terminal it is in; NULL: none */
  long ay, by;	/* lines, as Term.base counts: where it began, where it is now */
  int ax, bx;	/* columns */
  int block;	/* Alt+drag: a column block */
  int unit;	/* 0 letters, 1 words (a double click), 2 lines (a triple click) */
  int drag, moved;
  long long at_us;	/* the last click, for double and triple ones: its terminal, place */
  Term *ct;
  int clicks, cx;
  long cy;
} SL;

#define P	(*g_term[g_cur])	/* the terminal in front */
#define NONE	(g_n == 0)


/* what the pty and the parser ask of a window */
void win_wake (void) {
}


void win_message (const char *title, const char *text) {
  (void)title;
  toast(1, "%s", text);
}


static void reply (void *ud, const char *s, size_t n) {
  Term *t = (Term *)ud;
  if (t->pty) pty_write(t->pty, s, n);
}


static void set_title (void *ud, const char *utf8) {
  Term *t = (Term *)ud;
  snprintf(t->title, sizeof(t->title), "%s", utf8);
}


/*
** {==================================================================
** Shell integration
** ===================================================================
*/

static Vec g_hist, g_dirs;	/* the commands run and the folders been in, the last first (in mme-data) */
static int g_hist_read;


static void hist_read (Vec *v, const char *name) {
  char *f = data_path(name), *s, *line;
  vec_init(v);
  s = read_file(f, NULL);
  free(f);
  if (s == NULL) return;
  for (line = strtok(s, "\n"); line; line = strtok(NULL, "\n"))
    if (*line) vec_push(v, xstrdup(line));
  free(s);
}


static void hist_load (void) {
  if (g_hist_read) return;
  g_hist_read = 1;
  hist_read(&g_hist, "terminal-history");
  hist_read(&g_dirs, "terminal-dirs");
}


/* s first in v (once), and v into mme-data */
static void hist_add (Vec *v, const char *s, const char *name) {
  size_t i;
  Buf b;
  char *f;
  int fd;
  hist_load();
  if (s == NULL || *s == '\0' || strchr(s, '\n')) return;
  for (i = 0; i < v->n; i++)
    if (strcmp(v->v[i], s) == 0) {
      free(v->v[i]);
      memmove(v->v + i, v->v + i + 1, (v->n - i - 1) * sizeof(char *));
      v->n--;
      break;
    }
  vec_insert(v, 0, xstrdup(s));
  while (v->n > 200) free(v->v[--v->n]);
  buf_init(&b);
  for (i = 0; i < v->n; i++) buf_printf(&b, "%s\n", v->v[i]);
  f = data_path(name);
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  free(f);
  buf_free(&b);
}


/* the text of line y (grid_line's y) from column c0 to c1; its trailing spaces cut when trim */
static void cell_text (const Term *t, int y, int c0, int c1, int trim, Buf *b) {
  const Line *l = grid_line(t->g, y);
  size_t start = b->len;
  int col;
  for (col = c0 < 0 ? 0 : c0; l && col < c1 && col < l->n && col < t->cols; col++) {
    char u[4];
    uint32_t ch = l->c[col].ch;
    if (l->c[col].attr & A_WCONT) continue;
    if (ch & CH_CLUSTER) ch = grid_base(t->g, &l->c[col]);
    if (ch == 0 || (ch & CH_IMAGE)) ch = ' ';
    buf_putn(b, u, (size_t)utf8_encode(ch, u));
  }
  while (trim && b->len > start && b->s[b->len - 1] == ' ') b->len--;
}


/* lines that went up into the scrollback since last time: base counts them */
static void track (Term *t) {
  Grid *g = t->g;
  if (g->sb_len >= t->sb_len0 || g->sb_head != 0)	/* not cleared */
    t->base += (g->sb_len - t->sb_len0) + (g->sb_cap > 0 ? (g->sb_head - t->sb_head0 + g->sb_cap) % g->sb_cap : 0);
  t->sb_len0 = g->sb_len;
  t->sb_head0 = g->sb_head;
}


/* 633's escapes undone: \\ and \xAB */
static char *si_unescape (const char *s, const char *end) {
  Buf b;
  buf_init(&b);
  for (; s < end && *s; s++) {
    if (s[0] == '\\' && s[1] == '\\') {
      buf_putc(&b, '\\');
      s++;
    }
    else if (s[0] == '\\' && s[1] == 'x' && s[2] && s[3]) {
      char h[3];
      h[0] = s[2];
      h[1] = s[3];
      h[2] = '\0';
      buf_putc(&b, (char)strtol(h, NULL, 16));
      s += 3;
    }
    else buf_putc(&b, *s);
  }
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/* the shell is in folder dir now ("/C:/x" and "/c/x" as Windows says them) */
static void set_cwd (Term *t, const char *dir) {
  char d[1024];
  size_t i;
  snprintf(d, sizeof(d), "%s", dir);
#ifdef _WIN32
  if (d[0] == '/' && d[1] && d[2] == ':') memmove(d, d + 1, strlen(d));	/* /C:/x */
  else if (d[0] == '/' && d[1] && (d[2] == '/' || d[2] == '\0')) {	/* /c/x, MSYS's */
    d[0] = (char)(d[1] >= 'a' && d[1] <= 'z' ? d[1] - 32 : d[1]);
    d[1] = ':';
    if (d[2] == '\0') {
      d[2] = '\\';
      d[3] = '\0';
    }
  }
  for (i = 0; d[i]; i++)
    if (d[i] == '/') d[i] = '\\';
#else
  (void)i;
#endif
  {	/* a folder there is (a shell's own names for them, like mmc's /tmp, are left) */
    OsStat st;
    if (d[0] == '\0' || os_stat(d, &st) != 0 || !st.exists || !st.is_dir) return;
  }
  snprintf(t->cwd, sizeof(t->cwd), "%s", d);
  hist_add(&g_dirs, d, "terminal-dirs");
}


/* OSC 7: file://host/path */
static void osc7 (Term *t, const char *s) {
  const char *p = s;
  char *path;
  if (strncmp(p, "file://", 7) == 0) {
    p += 7;
    while (*p && *p != '/') p++;	/* the host */
  }
  path = si_unescape(p, p + strlen(p));
  {	/* %20 and the like */
    char *r = path, *w = path;
    for (; *r; r++) {
      if (r[0] == '%' && r[1] && r[2]) {
        char h[3];
        h[0] = r[1];
        h[1] = r[2];
        h[2] = '\0';
        *w++ = (char)strtol(h, NULL, 16);
        r += 2;
      }
      else *w++ = *r;
    }
    *w = '\0';
  }
  set_cwd(t, path);
  free(path);
}


static Mark *mark_add (Term *t) {
  Mark *m;
  if (t->nmark == 1000) {	/* the oldest goes */
    free(t->mark[0].cmd);
    memmove(t->mark, t->mark + 1, (size_t)(t->nmark - 1) * sizeof(Mark));
    t->nmark--;
  }
  if (t->nmark == t->capmark) {
    t->capmark = t->capmark ? t->capmark * 2 : 32;
    t->mark = (Mark *)xrealloc(t->mark, (size_t)t->capmark * sizeof(Mark));
  }
  m = &t->mark[t->nmark++];
  memset(m, 0, sizeof(*m));
  m->line = t->base + t->g->cy;
  m->exit = -1;
  return m;
}


/* what was typed after the prompt (633;B) up to the cursor's line */
static char *typed_text (Term *t) {
  Buf b;
  long y = t->b_line - t->base;
  int c0 = t->b_col;
  char *s, *p;
  buf_init(&b);
  while (y >= -t->g->sb_len && y < t->g->rows) {
    const Line *l = grid_line(t->g, (int)y);
    cell_text(t, (int)y, c0, t->cols, !(l && l->wrapped), &b);
    if (l == NULL || !l->wrapped || y >= t->g->cy) break;
    y++;
    c0 = 0;
  }
  buf_putc(&b, '\0');
  s = buf_take(&b);
  for (p = s; *p == ' '; p++) ;
  memmove(s, p, strlen(p) + 1);
  return s;
}


/* OSC 633;s (or 133;s): A prompt, B its end, C the command runs, D;n it ended, E;cmd, P;Cwd=dir */
static void on_si (Term *t, const char *s) {
  Mark *m = t->nmark ? &t->mark[t->nmark - 1] : NULL;
  track(t);
  if (t->g->alt) return;	/* a full screen program */
  switch (s[0]) {
    case 'A':
      mark_add(t);
      t->has_b = 0;
      break;
    case 'B':
      t->b_line = t->base + t->g->cy;
      t->b_col = t->g->cx;
      t->has_b = 1;
      break;
    case 'C':
      if (m && !m->ran) {
        char *c = t->cmdline ? xstrdup(t->cmdline) : t->has_b ? typed_text(t) : NULL;
        if (c && *c) {
          m->ran = 1;
          m->cmd = c;
          hist_add(&g_hist, c, "terminal-history");
        }
        else free(c);
      }
      free(t->cmdline);
      t->cmdline = NULL;
      break;
    case 'D':
      if (m && m->ran && m->exit == -1) m->exit = s[1] == ';' && s[2] ? atoi(s + 2) : -2;
      break;
    case 'E':
      if (s[1] == ';') {
        const char *e = strchr(s + 2, ';');	/* ;nonce after it */
        free(t->cmdline);
        t->cmdline = si_unescape(s + 2, e ? e : s + strlen(s));
      }
      break;
    case 'P':
      if (strncmp(s + 1, ";Cwd=", 5) == 0) {
        char *d = si_unescape(s + 6, s + strlen(s));
        set_cwd(t, d);
        free(d);
      }
      break;
  }
}


/* an OSC read to its end (st: by ESC \, else BEL): shell integration's, else it goes on to tvt.c */
static void osc_done (Term *t, int st) {
  size_t n = t->osc.len;
  const char *s;
  buf_putc(&t->osc, '\0');
  s = t->osc.s;
  if (strncmp(s, "633;", 4) == 0 || strncmp(s, "133;", 4) == 0) {
    if (opt.term_shell_int) on_si(t, s + 4);
  }
  else {
    if (strncmp(s, "7;", 2) == 0) osc7(t, s + 2);
    vt_feed(&t->vt, "\033]", 2);
    vt_feed(&t->vt, s, n);
    vt_feed(&t->vt, st ? "\033\\" : "\a", st ? 2 : 1);
  }
  t->osc.len = 0;
}


/* the shell's output to its screen; the OSCs are looked at on the way */
static void term_feed (Term *t, const char *s, size_t n) {
  size_t i, from = 0;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (t->ost) {
      case 0:
        if (c == 0x1B) {
          if (i > from) vt_feed(&t->vt, s + from, i - from);
          t->ost = 1;
          from = i + 1;
        }
        break;
      case 1:	/* after ESC */
        if (c == ']') {
          t->ost = 2;
          t->osc.len = 0;
          from = i + 1;
        }
        else {
          vt_feed(&t->vt, "\033", 1);
          t->ost = 0;
          from = i;
          if (c == 0x1B) {
            t->ost = 1;
            from = i + 1;
          }
        }
        break;
      case 2:	/* in an OSC */
        if (c == 0x07) {
          osc_done(t, 0);
          t->ost = 0;
          from = i + 1;
        }
        else if (c == 0x1B) t->ost = 3;
        else if (t->osc.len < 8192) buf_putc(&t->osc, (char)c);
        break;
      default:	/* ESC in an OSC: ESC \ ends it, anything else starts over */
        osc_done(t, 1);
        if (c == '\\') {
          t->ost = 0;
          from = i + 1;
        }
        else {
          t->ost = 1;
          from = i;
          i--;
        }
        break;
    }
  }
  if (t->ost == 0 && n > from) vt_feed(&t->vt, s + from, n - from);
  track(t);
}


static const char ps_script[] =
  "# mme's shell integration for PowerShell, like VS Code's: OSC 633 around the prompt and the command\n"
  "if ($Global:__MmeIntegrated) { return }\n"
  "$Global:__MmeIntegrated = $true\n"
  "$Global:__MmeOrig = $function:prompt\n"
  "$Global:__MmeStarted = $false\n"
  "function Global:__MmeEsc([string]$s) { return $s.Replace('\\', '\\\\').Replace(';', '\\x3b').Replace(\"`n\", '\\x0a').Replace(\"`r\", '\\x0d') }\n"
  "function Global:prompt {\n"
  "  $ok = $global:?\n"
  "  $code = $global:LASTEXITCODE\n"
  "  $e = [char]27; $b = [char]7\n"
  "  $r = ''\n"
  "  if ($Global:__MmeStarted) {\n"
  "    if ($ok) { $c = 0 } elseif ($code) { $c = $code } else { $c = 1 }\n"
  "    $r += \"$e]633;D;$c$b\"\n"
  "  }\n"
  "  $r += \"$e]633;A$b$e]633;P;Cwd=$(__MmeEsc $executionContext.SessionState.Path.CurrentLocation.ProviderPath)$b\"\n"
  "  $r += (& $Global:__MmeOrig)\n"
  "  $r += \"$e]633;B$b\"\n"
  "  $Global:__MmeStarted = $true\n"
  "  return $r\n"
  "}\n"
  "if (Get-Module -Name PSReadLine) {\n"
  "  Set-PSReadLineKeyHandler -Chord Enter -ScriptBlock {\n"
  "    $line = $null; $cursor = $null\n"
  "    [Microsoft.PowerShell.PSConsoleReadLine]::GetBufferState([ref]$line, [ref]$cursor)\n"
  "    [Console]::Write(\"$([char]27)]633;E;$(__MmeEsc $line)$([char]7)\")\n"
  "    [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine()\n"
  "    [Console]::Write(\"$([char]27)]633;C$([char]7)\")\n"
  "  }\n"
  "}\n";

static const char bash_script[] =
  "# mme's shell integration for bash, like VS Code's: OSC 633 around the prompt and the command\n"
  "if [ -n \"$MME_LOGIN\" ]; then\n"
  "  unset MME_LOGIN\n"
  "  [ -r /etc/profile ] && . /etc/profile\n"
  "  if [ -r ~/.bash_profile ]; then . ~/.bash_profile; elif [ -r ~/.bash_login ]; then . ~/.bash_login; elif [ -r ~/.profile ]; then . ~/.profile; fi\n"
  "elif [ -r ~/.bashrc ]; then . ~/.bashrc; fi\n"
  "__mme_esc() { local s=\"${1//\\\\/\\\\\\\\}\"; printf '%s' \"${s//;/\\\\x3b}\"; }\n"
  "__mme_prompt() {\n"
  "  local ec=$?\n"
  "  if [ -n \"$__mme_started\" ]; then printf '\\033]633;D;%s\\007' \"$ec\"; fi\n"
  "  printf '\\033]633;A\\007\\033]633;P;Cwd=%s\\007' \"$(__mme_esc \"$PWD\")\"\n"
  "  __mme_started=1\n"
  "}\n"
  "PROMPT_COMMAND=\"__mme_prompt${PROMPT_COMMAND:+;$PROMPT_COMMAND}\"\n"
  "PS1=\"${PS1}\\[\\033]633;B\\007\\]\"\n"
  "PS0='\\[\\033]633;C\\007\\]'\n";


/* the shell's kind by its program's name */
static int shell_kind (const char *sh) {
  char n[64];
  size_t i;
  snprintf(n, sizeof(n), "%s", path_basename(sh));
  for (i = 0; n[i]; i++)
    if (n[i] >= 'A' && n[i] <= 'Z') n[i] = (char)(n[i] + 32);
  if (strncmp(n, "cmd", 3) == 0) return SH_CMD;
  if (strncmp(n, "powershell", 10) == 0 || strncmp(n, "pwsh", 4) == 0) return SH_PS;
  if (strncmp(n, "bash", 4) == 0) return SH_BASH;
  return SH_OTHER;
}


/* a script written into mme-data: its path */
static char *put_script (const char *name, const char *text) {
  char *f = data_path(name);
  int fd = os_open(f, OS_WRITE);
  if (fd >= 0) {
    os_write(fd, text, strlen(text));
    os_close(fd);
  }
  return f;
}

/* }================================================================== */


/*
** {==================================================================
** Profiles: the shells there are
** ===================================================================
*/

typedef struct Profile {
  char name[40];	/* VS Code's names: "Command Prompt", "Git Bash" ... */
  char *path;
  const char *arg;	/* one argument, or NULL */
} Profile;

static Profile g_prof[12];
static int g_nprof = -1;


static void add_profile (const char *name, char *path, const char *arg) {
  int i;
  if (path == NULL) return;
  for (i = 0; i < g_nprof; i++)	/* the same program once */
    if (m_fncmp(g_prof[i].path, path) == 0) {
      free(path);
      return;
    }
  if (g_nprof == (int)(sizeof(g_prof) / sizeof(g_prof[0]))) {
    free(path);
    return;
  }
  snprintf(g_prof[g_nprof].name, sizeof(g_prof[g_nprof].name), "%s", name);
  g_prof[g_nprof].path = path;
  g_prof[g_nprof].arg = arg;
  g_nprof++;
}


#ifdef _WIN32
static char *if_exec (char *p) {
  if (p && os_is_exec(p)) return p;
  free(p);
  return NULL;
}
#endif


/* the shells found, looked for once */
static void find_profiles (void) {
  if (g_nprof >= 0) return;
  g_nprof = 0;
  add_profile("mmc-shell", find_program("mmc-shell"), NULL);
#ifdef _WIN32
  {
    char *git = find_program("git"), *bash = NULL;
    add_profile("PowerShell", find_program("pwsh"), NULL);
    add_profile("Windows PowerShell", find_program("powershell"), NULL);
    add_profile("Command Prompt", os_getenv("ComSpec"), NULL);
    if (git) {	/* ...\Git\cmd\git.exe: ...\Git\bin\bash.exe */
      char *d = path_dirname(git), *up = path_dirname(d);
      bash = if_exec(path_join(up, "bin\\bash.exe"));
      free(d);
      free(up);
      free(git);
    }
    if (bash == NULL) bash = if_exec(xstrdup("C:\\Program Files\\Git\\bin\\bash.exe"));
    add_profile("Git Bash", bash, "--login");
    add_profile("WSL", find_program("wsl"), NULL);
  }
#else
  add_profile("mmc", find_program("mmc"), NULL);
  add_profile("bash", find_program("bash"), NULL);
  add_profile("zsh", find_program("zsh"), NULL);
  add_profile("fish", find_program("fish"), NULL);
  add_profile("sh", find_program("sh"), NULL);
#endif
}


int panel_profiles (void) {
  find_profiles();
  return g_nprof;
}


const char *panel_profile_name (int i) {
  find_profiles();
  return (i >= 0 && i < g_nprof) ? g_prof[i].name : "";
}


const char *panel_profile_path (int i) {
  find_profiles();
  return (i >= 0 && i < g_nprof) ? g_prof[i].path : "";
}


static int profile_by_name (const char *name) {
  int i;
  find_profiles();
  for (i = 0; name && *name && i < g_nprof; i++)
    if (m_strnicmp(g_prof[i].name, name, strlen(name) + 1) == 0) return i;
  return -1;
}


/* the shell: terminal.integrated.shell, the default profile, $MME_SHELL, else mmc, else the system's */
static char *shell_path (const char **arg) {
  char *s;
  int p;
  *arg = NULL;
  if (opt.shell[0]) return xstrdup(opt.shell);
  if ((p = profile_by_name(opt.term_profile)) >= 0) {
    *arg = g_prof[p].arg;
    return xstrdup(g_prof[p].path);
  }
  s = os_getenv("MME_SHELL");
  if (s && *s) return s;
  free(s);
  if ((s = find_program("mmc-shell")) != NULL) return s;
#ifdef _WIN32
  if ((s = os_getenv("ComSpec")) != NULL) return s;
  return xstrdup("C:\\Windows\\System32\\cmd.exe");
#else
  if ((s = find_program("mmc")) != NULL) return s;
  if ((s = os_getenv("SHELL")) != NULL && *s) return s;
  free(s);
  return xstrdup("/bin/sh");
#endif
}

/* }================================================================== */


int panel_alive (void) {
  return g_n > 0;
}


int panel_count (void) {
  return g_n;
}


int panel_current (void) {
  return g_cur;
}


void panel_select (int i) {
  if (i >= 0 && i < g_n) g_cur = i;
}


/* the name on terminal i's tab: its shell, or what it was renamed to */
const char *panel_name (int i) {
  return (i >= 0 && i < g_n) ? g_term[i]->name : "";
}


/* the title the shell in front set */
const char *panel_title (void) {
  if (NONE) return "";
  return P.title;
}


/* Terminal: Rename... */
void panel_rename (const char *name) {
  if (!NONE && name && *name) snprintf(P.name, sizeof(P.name), "%s", name);
}


/* how many terminals are in the group of the one in front */
static int group_size (void) {
  int i, n = 0;
  for (i = 0; i < g_n; i++)
    if (g_term[i]->grp == P.grp) n++;
  return n;
}


int panel_group_size (void) {
  return NONE ? 0 : group_size();
}


static void term_free (int i) {
  Term *t = g_term[i];
  int grp = t->grp, j, was = i == g_cur;
  if (t->pty) pty_close(t->pty);
  vt_free(&t->vt);
  grid_free(t->g);
  buf_free(&t->osc);
  for (j = 0; j < t->nmark; j++) free(t->mark[j].cmd);
  free(t->mark);
  free(t->cmdline);
  if (SL.t == t) SL.t = NULL;
  if (SL.ct == t) SL.ct = NULL;
  free(t);
  memmove(g_term + i, g_term + i + 1, (size_t)(g_n - i - 1) * sizeof(Term *));
  g_n--;
  if (g_cur > i || g_cur >= g_n) g_cur = g_cur > 0 ? g_cur - 1 : 0;
  for (j = 0; was && j < g_n; j++)	/* one of its group stays in front */
    if (g_term[j]->grp == grp && (g_cur >= g_n || g_term[g_cur]->grp != grp)) g_cur = j;
}


/* the terminal in front ends (the trash icon) */
void panel_kill (void) {
  if (!NONE) term_free(g_cur);
}


/* terminal.integrated.confirmOnKill: 1 when the terminal in front may go */
int panel_confirm_kill (void) {
  static const char *const bt[] = {"Terminate", "Cancel"};
  if (NONE || opt.term_confirm_kill < 2) return 1;	/* never, editor: the panel's go without asking */
  return dialog("Do you want to terminate the active terminal session?", NULL, bt, 2) == 0;
}


/* is something running in t: a task, or a command shell integration saw start */
static int running (const Term *t) {
  if (t->task) return !t->done;
  return t->nmark > 0 && t->mark[t->nmark - 1].ran && t->mark[t->nmark - 1].exit == -1;
}


/* terminal.integrated.confirmOnExit: 1 when mme may quit */
int panel_confirm_exit (void) {
  static const char *const bt[] = {"Terminate", "Cancel"};
  char msg[160];
  int i, busy = 0;
  if (NONE || opt.term_confirm_exit == 0) return 1;
  for (i = 0; i < g_n; i++) busy |= running(g_term[i]);
  if (opt.term_confirm_exit == 2 && !busy) return 1;	/* hasChildProcesses */
  if (g_n == 1) snprintf(msg, sizeof(msg), "Do you want to terminate the active terminal session?");
  else snprintf(msg, sizeof(msg), "Do you want to terminate the %d active terminal sessions?", g_n);
  return dialog(msg, NULL, bt, 2) == 0;
}


/*
** A task: its command line run by the system's shell in a terminal named
** after it, like VS Code's; when it ends the terminal stays, and a key
** closes it.
*/
int panel_run (int cols, int rows, const char *name, const char *cmd, const char *cwd, int task) {
  char *sh, *argv[5], *here, head[1024];
  Term *t;
  int i;
  for (i = 0; i < g_n; i++)	/* a finished task's terminal is used again */
    if (g_term[i]->done) {
      term_free(i);
      break;
    }
  if (g_n == MAX_TERM) return -1;
  if (cols < 2) cols = 2;
  if (rows < 1) rows = 1;
#ifdef _WIN32
  sh = os_getenv("ComSpec");
  if (sh == NULL) sh = xstrdup("C:\\Windows\\System32\\cmd.exe");
  argv[0] = sh;
  argv[1] = "/d";
  argv[2] = "/c";
  argv[3] = (char *)cmd;
  argv[4] = NULL;
#else
  sh = xstrdup("/bin/sh");
  argv[0] = sh;
  argv[1] = "-c";
  argv[2] = (char *)cmd;
  argv[3] = NULL;
#endif
  os_setenv("TERM", "xterm-256color");
  os_setenv("COLORTERM", "truecolor");
  os_setenv("TERM_PROGRAM", MME_NAME);
  t = (Term *)xmalloc(sizeof(Term));
  memset(t, 0, sizeof(*t));
  here = os_getcwd();
  os_chdir(cwd ? cwd : side_root());
  t->pty = pty_spawn(sh, argv, cols, rows);
  if (here) os_chdir(here);
  free(here);
  free(sh);
  if (t->pty == NULL) {
    toast(1, "The task could not start");
    free(t);
    return -1;
  }
  snprintf(t->name, sizeof(t->name), "%s", name);
  t->task = task;
  t->grp = ++g_next_grp;
  t->g = grid_new(cols, rows, opt.term_scrollback);
  vt_init(&t->vt, t->g);
  t->vt.ud = t;
  t->vt.reply = reply;
  t->vt.title = set_title;
  t->cols = cols;
  t->rows = rows;
  snprintf(head, sizeof(head), "\033[0m *  Executing task: %s \r\n\r\n", cmd);
  buf_init(&t->osc);
  term_feed(t, head, strlen(head));
  g_term[g_n] = t;
  g_cur = g_n++;
  return 0;
}


void panel_kill_all (void) {
  while (g_n > 0) term_free(0);
}


static char *g_cwd;	/* where the next one starts, NULL: the folder open */


void panel_cwd (const char *dir) {
  free(g_cwd);
  g_cwd = dir ? xstrdup(dir) : NULL;
}


/* a new terminal at 'at' in g_term: shell sh (NULL: the default) in the folder that is open */
static int term_new (int at, int grp, int cols, int rows, const char *sh0, const char *arg, const char *name) {
  char *sh, *cwd, *argv[8], *dot, *script = NULL, *cmdarg = NULL, *oldprompt = NULL, *start;
  const char *arg0 = NULL;
  int na = 0, kind, login = 0, i;
  Term *t;
  const Json *env;
  char *old[64];
  const char *envname[64];
  int nenv = 0;
  if (g_n == MAX_TERM) {
    toast(1, "There are %d terminals already", MAX_TERM);
    return -1;
  }
  if (cols < 2) cols = 2;
  if (rows < 1) rows = 1;
  sh = sh0 ? xstrdup(sh0) : shell_path(&arg0);
  if (sh0 == NULL) arg = arg0;
  kind = shell_kind(sh);
  argv[na++] = sh;
  if (arg && kind == SH_BASH && strcmp(arg, "--login") == 0 && opt.term_shell_int) login = 1;	/* the script reads the profile */
  else if (arg) argv[na++] = (char *)arg;
  if (opt.term_shell_int) {	/* terminal.integrated.shellIntegration.enabled: the shell told to mark its prompts */
    if (kind == SH_PS) {
      size_t k;
      Buf b;
      script = put_script("shellIntegration.ps1", ps_script);
      buf_init(&b);
      buf_puts(&b, ". '");
      for (k = 0; script[k]; k++) {
        if (script[k] == '\'') buf_putc(&b, '\'');
        buf_putc(&b, script[k]);
      }
      buf_puts(&b, "'");
      buf_putc(&b, '\0');
      cmdarg = buf_take(&b);
      argv[na++] = "-NoLogo";
      argv[na++] = "-NoExit";
      argv[na++] = "-Command";
      argv[na++] = cmdarg;
    }
    else if (kind == SH_BASH) {
      char *c;
      script = put_script("shellIntegration.bash", bash_script);
      for (c = script; *c; c++)
        if (*c == '\\') *c = '/';	/* bash on Windows reads C:/x */
      argv[na++] = "--init-file";
      argv[na++] = script;
      if (login) os_setenv("MME_LOGIN", "1");
    }
    else if (kind == SH_CMD) {	/* cmd's PROMPT: $E is ESC, $P the folder, $G > */
      char *p = os_getenv("PROMPT"), pr[512];
      oldprompt = p ? p : xstrdup("");
      snprintf(pr, sizeof(pr), "$E]633;A$E\\$E]633;P;Cwd=$P$E\\%s$E]633;B$E\\", p && *p ? p : "$P$G");
      os_setenv("PROMPT", pr);
    }
  }
  argv[na] = NULL;
  os_setenv("TERM", "xterm-256color");
  os_setenv("COLORTERM", "truecolor");
  os_setenv("TERM_PROGRAM", MME_NAME);
#ifdef _WIN32	/* terminal.integrated.env.windows (linux, osx): more for the shell, only */
  env = settings_value("terminal.integrated.env.windows");
#elif defined(__APPLE__)
  env = settings_value("terminal.integrated.env.osx");
#else
  env = settings_value("terminal.integrated.env.linux");
#endif
  for (i = 0; env && env->type == J_OBJ && i < (int)env->n && nenv < 64; i++) {
    envname[nenv] = env->kid[i]->key;
    old[nenv] = os_getenv(env->kid[i]->key);
    os_setenv(env->kid[i]->key, env->kid[i]->type == J_STR ? env->kid[i]->str : NULL);
    nenv++;
  }
  t = (Term *)xmalloc(sizeof(Term));
  memset(t, 0, sizeof(*t));
  buf_init(&t->osc);
  t->kind = kind;
  cwd = os_getcwd();	/* the shell starts in the folder that is open, or terminal.integrated.cwd */
  if (g_cwd) start = xstrdup(g_cwd);
  else if (opt.term_cwd[0]) {
#ifdef _WIN32
    int abs = opt.term_cwd[0] == '\\' || opt.term_cwd[0] == '/' || (opt.term_cwd[0] && opt.term_cwd[1] == ':');
#else
    int abs = opt.term_cwd[0] == '/';
#endif
    start = abs ? xstrdup(opt.term_cwd) : path_join(side_root(), opt.term_cwd);
  }
  else start = xstrdup(side_root());
  if (os_chdir(start) != 0) {
    free(start);
    start = xstrdup(side_root());
    os_chdir(start);
  }
  snprintf(t->cwd, sizeof(t->cwd), "%s", start);
  free(start);
  panel_cwd(NULL);
  t->pty = pty_spawn(sh, argv, cols, rows);
  if (cwd) os_chdir(cwd);
  free(cwd);
  for (i = 0; i < nenv; i++) {	/* mme's own again */
    os_setenv(envname[i], old[i]);
    free(old[i]);
  }
  if (oldprompt) {
    os_setenv("PROMPT", *oldprompt ? oldprompt : NULL);
    free(oldprompt);
  }
  if (login) os_setenv("MME_LOGIN", NULL);
  free(script);
  free(cmdarg);
  if (t->pty == NULL) {
    toast(1, "The terminal could not start %s", sh);
    free(sh);
    buf_free(&t->osc);
    free(t);
    return -1;
  }
  if (name) snprintf(t->name, sizeof(t->name), "%s", name);
  else {
    snprintf(t->name, sizeof(t->name), "%s", path_basename(sh));	/* "mmc-shell.exe": "mmc-shell" */
    if ((dot = strrchr(t->name, '.')) != NULL) *dot = '\0';
  }
  free(sh);
  t->g = grid_new(cols, rows, opt.term_scrollback);
  vt_init(&t->vt, t->g);
  t->vt.ud = t;
  t->vt.reply = reply;
  t->vt.title = set_title;
  t->cols = cols;
  t->rows = rows;
  t->grp = grp;
  memmove(g_term + at + 1, g_term + at, (size_t)(g_n - at) * sizeof(Term *));
  g_term[at] = t;
  g_n++;
  g_cur = at;
  return 0;
}


/* a new terminal in front, in a group of its own */
int panel_new (int cols, int rows) {
  return term_new(g_n, ++g_next_grp, cols, rows, NULL, NULL, NULL);
}


/* the same with profile p (panel_profile_name) */
int panel_new_profile (int cols, int rows, int p) {
  find_profiles();
  if (p < 0 || p >= g_nprof) return panel_new(cols, rows);
  return term_new(g_n, ++g_next_grp, cols, rows, g_prof[p].path, g_prof[p].arg, g_prof[p].name);
}


/* Terminal: Split Terminal: one more next to the one in front, in its group */
int panel_split (void) {
  if (NONE) return panel_new(40, 10);
  return term_new(g_cur + 1, P.grp, P.cols / 2 > 2 ? P.cols / 2 : 2, P.rows, NULL, NULL, NULL);
}


/* Alt+Left / Alt+Right: the terminal before / after the one in front, in its group */
void panel_focus_pane (int d) {
  int i;
  if (NONE) return;
  for (i = g_cur + d; i >= 0 && i < g_n; i += d)
    if (g_term[i]->grp == P.grp) {
      g_cur = i;
      return;
    }
}


/* the first terminal, when there is none yet */
int panel_start (int cols, int rows) {
  return NONE ? panel_new(cols, rows) : 0;
}


/* every shell's output into its grid; 1 when something changed, 2 the last one ended */
int panel_poll (void) {
  char buf[65536];
  int i, got = 0, code;
  for (i = 0; i < g_n; i++) {
    Term *t = g_term[i];
    long n;
    if (t->done) continue;
    while ((n = pty_read(t->pty, buf, sizeof(buf))) > 0) {
      term_feed(t, buf, (size_t)n);
      if (t->task) task_output(t->task, buf, (size_t)n);
      got = 1;
    }
    if (t->task && (n < 0 || pty_exited(t->pty, &code))) {	/* a task ended: its terminal stays */
      char msg[256];
      if (n < 0) code = -1;
      while ((n = pty_read(t->pty, buf, sizeof(buf))) > 0) {	/* its last words */
        term_feed(t, buf, (size_t)n);
        task_output(t->task, buf, (size_t)n);
      }
      snprintf(msg, sizeof(msg), "\r\n\033[0m *  The terminal process terminated with exit code: %d. \r\n"
                                 " *  Terminal will be reused by tasks, press any key to close it. \r\n", code);
      term_feed(t, msg, strlen(msg));
      task_done(t->task, code);
      pty_close(t->pty);
      t->pty = NULL;
      t->done = 1;
      got = 1;
      continue;
    }
    if (n < 0 || pty_exited(t->pty, &code)) {	/* the shell ended: its terminal goes */
      term_free(i--);
      if (g_n == 0) return 2;
      got = 1;
    }
  }
  return got;
}


static void resize (Term *t, int cols, int rows) {
  if (cols < 2) cols = 2;
  if (rows < 1) rows = 1;
  if (cols == t->cols && rows == t->rows) return;
  grid_resize(t->g, cols, rows);
  if (t->pty) pty_resize(t->pty, cols, rows);
  t->cols = cols;
  t->rows = rows;
}


/* Terminal: Clear: the scrollback and the screen go, the line being typed stays on top */
void panel_clear (void) {
  const Line *l;
  Buf b;
  int col, cx;
  if (NONE) return;
  if (P.g->alt) return;	/* a full screen program owns the screen */
  l = grid_line(P.g, P.g->cy);
  cx = P.g->cx;
  buf_init(&b);
  buf_puts(&b, "\033[H\033[2J\033[3J");
  for (col = 0; l && col < l->n && col < P.cols; col++) {	/* the prompt again */
    char u[4];
    uint32_t ch = l->c[col].ch;
    if (l->c[col].attr & A_WCONT) continue;
    if (ch & CH_CLUSTER) ch = grid_base(P.g, &l->c[col]);
    if (ch == 0 || (ch & CH_IMAGE)) ch = ' ';
    buf_putn(&b, u, (size_t)utf8_encode(ch, u));
  }
  while (b.len > 0 && b.s[b.len - 1] == ' ') b.len--;
  buf_printf(&b, "\r\033[%dC", cx);
  if (cx == 0) buf_puts(&b, "\r");
  vt_feed(&P.vt, b.s, b.len);
  track(&P);
  grid_set_view(P.g, 0);
  buf_free(&b);
}


/* Shift+PgUp and the like: the scrollback, d lines (a page: 0, with page -1 / 1) */
void panel_scroll (int d, int page) {
  if (NONE || P.g->alt) return;
  if (page) d = page * (P.rows - 1 > 1 ? P.rows - 1 : 1);
  grid_set_view(P.g, P.g->view - d);
}


/*
** {==================================================================
** Links: path:line:col and URLs in the output
** ===================================================================
*/

/* a line's text, and the column each byte came from */
static int line_text (const Term *t, int y, char *s, int *col_of, int cap) {
  const Line *l = grid_line(t->g, y);
  int col, n = 0;
  for (col = 0; l && col < l->n && col < t->cols; col++) {
    char u[4];
    int k, len;
    uint32_t ch = l->c[col].ch;
    if (l->c[col].attr & A_WCONT) continue;
    if (ch & CH_CLUSTER) ch = grid_base(t->g, &l->c[col]);
    if (ch == 0 || (ch & CH_IMAGE)) ch = ' ';
    len = utf8_encode(ch, u);
    if (n + len >= cap) break;
    for (k = 0; k < len; k++) {
      s[n] = u[k];
      col_of[n++] = col;
    }
  }
  s[n] = '\0';
  col_of[n] = col;
  return n;
}


static int is_path_char (int c) {
  return c > 127 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         (c != '\0' && strchr("_-./\\:~@+%#=?&,$!*", c) != NULL);
}


static int is_digit (int c) {
  return c >= '0' && c <= '9';
}


/*
** The link at byte i of s (a line's text): *a..*b its bytes; lk gets what it
** opens. 0: none. "mme.c:120:5", "E:\w\x.c(12,5)", "./a/b.go:7", URLs.
*/
static int link_at (const char *s, int n, int i, int *a, int *b, PanelLink *lk, const char *root) {
  int p, q, e;
  char tok[1024];
  size_t tl;
  if (i < 0 || i >= n || !is_path_char((unsigned char)s[i])) return 0;
  for (p = i; p > 0 && is_path_char((unsigned char)s[p - 1]); p--) ;
  for (q = i + 1; q < n && is_path_char((unsigned char)s[q]); q++) ;
  while (q > p && strchr(".,;:!?'\"", s[q - 1])) q--;	/* "see a.c." */
  if (q <= p || q - p >= (int)sizeof(tok)) return 0;
  memset(lk, 0, sizeof(*lk));
  {	/* a URL */
    const char *u = NULL;
    int k;
    for (k = p; k < q - 3 && u == NULL; k++)
      if (strncmp(s + k, "https://", 8) == 0 || strncmp(s + k, "http://", 7) == 0 || strncmp(s + k, "file://", 7) == 0)
        u = s + k;
    if (u) {
      int start = (int)(u - s);
      if (i < start) return 0;
      *a = start;
      *b = q;
      lk->url = 1;
      lk->target = (char *)xmalloc((size_t)(q - start) + 1);
      memcpy(lk->target, u, (size_t)(q - start));
      lk->target[q - start] = '\0';
      return 1;
    }
  }
  e = q;	/* path:line:col, the numbers go */
  {
    int k, nums[2], nn = 0;
    while (nn < 2) {
      for (k = e; k > p && is_digit((unsigned char)s[k - 1]); k--) ;
      if (k == e || k - 1 <= p || s[k - 1] != ':') break;
      nums[nn++] = atoi(s + k);
      e = k - 1;
    }
    if (nn == 2) {
      lk->line = nums[1];
      lk->col = nums[0];
    }
    else if (nn == 1) lk->line = nums[0];
  }
  if (e - p >= 2 && s[e - 1] == ':' && s[e - 2] != ':' && !(e - p == 2)) e--;	/* "a.c:" */
  if (q < n && s[q] == '(' && lk->line == 0) {	/* x.c(12,5): the numbers after it */
    int k = q + 1;
    if (k < n && is_digit((unsigned char)s[k])) {
      lk->line = atoi(s + k);
      while (k < n && is_digit((unsigned char)s[k])) k++;
      if (k < n && s[k] == ',') lk->col = atoi(s + k + 1);
      while (k < n && s[k] != ')') k++;
      if (k < n) q = k + 1;
    }
  }
  tl = (size_t)(e - p);
  memcpy(tok, s + p, tl);
  tok[tl] = '\0';
  if (strchr(tok, '.') == NULL && strchr(tok, '/') == NULL && strchr(tok, '\\') == NULL) return 0;
  if (strncmp(tok, "a/", 2) == 0 || strncmp(tok, "b/", 2) == 0) {	/* git diff's a/x.c */
    char *f = path_join(root, tok + 2);
    OsStat st;
    if (os_stat(f, &st) == 0 && st.exists && !st.is_dir) memmove(tok, tok + 2, tl - 1);
    free(f);
  }
  {
    char *f;
    OsStat st;
#ifdef _WIN32
    int abs = tok[0] == '\\' || tok[0] == '/' || (tok[0] && tok[1] == ':');
#else
    int abs = tok[0] == '/';
#endif
    if (tok[0] == '~' && (tok[1] == '/' || tok[1] == '\\')) {
      char *home = os_getenv("HOME");
      if (home == NULL) home = os_getenv("USERPROFILE");
      f = home ? path_join(home, tok + 2) : xstrdup(tok);
      free(home);
    }
    else f = abs ? xstrdup(tok) : path_join(root, tok);
    if (os_stat(f, &st) != 0 || !st.exists || st.is_dir) {	/* not from the shell's folder: from the one open */
      free(f);
      if (abs || strcmp(root, side_root()) == 0) return 0;
      f = path_join(side_root(), tok);
      if (os_stat(f, &st) != 0 || !st.exists || st.is_dir) {
        free(f);
        return 0;
      }
    }
    lk->target = f;
  }
  *a = p;
  *b = q;
  return 1;
}


/* the link at a terminal's cell: its columns c0..c1 too */
static int link_cell (const Term *t, int y, int col, int *c0, int *c1, PanelLink *lk) {
  static char s[4096];
  static int cof[4097];
  const Line *l = grid_line(t->g, y);
  int n, i, a, b;
  if (l && col < l->n && l->c[col].link) {	/* OSC 8: the program said where it goes */
    int id = l->c[col].link;
    const char *uri = grid_link(t->g, id);
    if (uri && *uri) {
      for (*c0 = col; *c0 > 0 && l->c[*c0 - 1].link == id; (*c0)--) ;
      for (*c1 = col + 1; *c1 < l->n && l->c[*c1].link == id; (*c1)++) ;
      memset(lk, 0, sizeof(*lk));
      lk->url = 1;
      lk->target = xstrdup(uri);
      return 1;
    }
  }
  n = line_text(t, y, s, cof, (int)sizeof(s) - 1);
  for (i = 0; i < n && cof[i] < col; i++) ;
  if (i >= n || cof[i] != col) return 0;
  if (!link_at(s, n, i, &a, &b, lk, t->cwd[0] ? t->cwd : side_root())) return 0;
  *c0 = cof[a];
  *c1 = b < n ? cof[b] : cof[n - 1] + 1;
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Find (Ctrl+F in the terminal)
** ===================================================================
*/

static struct {
  int open;	/* the widget shows */
  char text[256];
  int y, c0, c1;	/* the match found last; c1 0: none */
  int total;	/* how many there are */
  int x, w, wy;	/* where the widget was drawn */
  int in_x, up_x, down_x, close_x;
} FD;


/* the matches of FD.text in line y of t: their columns into c0/c1; how many */
static int line_matches (const Term *t, int y, int *c0, int *c1, int max) {
  static char s[4096];
  static int cof[4097];
  int n = line_text(t, y, s, cof, (int)sizeof(s) - 1), tl = (int)strlen(FD.text), i, k = 0;
  if (tl == 0) return 0;
  for (i = 0; i + tl <= n && k < max; i++)
    if (m_strnicmp(s + i, FD.text, (size_t)tl) == 0) {
      c0[k] = cof[i];
      c1[k] = i + tl < n ? cof[i + tl] : cof[n - 1] + 1;
      k++;
      i += tl - 1;
    }
  return k;
}


static void find_count (void) {
  int y, c0[64], c1[64];
  FD.total = 0;
  if (NONE) return;
  for (y = -P.g->sb_len; y < P.rows; y++) FD.total += line_matches(&P, y, c0, c1, 64);
}


/* the next match up (d -1, older) or down (d 1) from the last one, around the ends; the view goes there */
static void find_step (int d) {
  int lo, hi, span, y0, i, c0[64], c1[64];
  if (NONE || FD.text[0] == '\0') return;
  lo = -P.g->sb_len;
  hi = P.rows - 1;
  span = hi - lo + 1;
  y0 = FD.c1 ? FD.y : (d < 0 ? hi : lo);
  for (i = 0; i <= span; i++) {
    int y = lo + ((y0 - lo + i * d) % span + span) % span, n = line_matches(&P, y, c0, c1, 64), k;
    if (d < 0) {
      for (k = n - 1; k >= 0; k--)
        if (!(i == 0 && FD.c1 && c0[k] >= FD.c0)) break;
    }
    else {
      for (k = 0; k < n; k++)
        if (!(i == 0 && FD.c1 && c0[k] <= FD.c0)) break;
      if (k == n) k = -1;
    }
    if (k >= 0) {
      int view = P.rows / 2 - y;	/* its line in the middle of the view */
      FD.y = y;
      FD.c0 = c0[k];
      FD.c1 = c1[k];
      if (view < 0 || y >= 0) view = 0;
      if (view > P.g->sb_len) view = P.g->sb_len;
      grid_set_view(P.g, view);
      return;
    }
  }
  FD.c1 = 0;
}


void panel_find_open (void) {
  if (NONE) return;
  FD.open = 1;
  FD.c1 = 0;
  find_count();
}


int panel_finding (void) {
  return FD.open && !NONE;
}


/* a key while the find widget is open; 0: not its */
int panel_find_key (int k) {
  int code = KEY_CODE(k);
  size_t len = strlen(FD.text);
  if (code == K_ESC) {
    FD.open = 0;
    FD.c1 = 0;
    return 1;
  }
  if (code == K_ENTER || code == K_F3) find_step((k & KM_SHIFT) ? 1 : -1);	/* Enter: up, like VS Code */
  else if (code == K_UP) find_step(-1);
  else if (code == K_DOWN) find_step(1);
  else if (code == K_BS) {
    while (len > 0 && ((unsigned char)FD.text[len - 1] & 0xC0) == 0x80) len--;
    if (len > 0) FD.text[len - 1] = '\0';
    FD.c1 = 0;
    find_count();
  }
  else if (IS_TEXT(k) && len + 4 < sizeof(FD.text)) {
    len += (size_t)utf8_encode((uint32_t)k, FD.text + len);
    FD.text[len] = '\0';
    FD.c1 = 0;
    find_count();
    find_step(-1);
  }
  else if (code == K_PASTE) {
    Buf b;
    size_t i;
    buf_init(&b);
    term_paste(&b);
    for (i = 0; i < b.len && b.s[i] != '\n' && len + 1 < sizeof(FD.text); i++) FD.text[len++] = b.s[i];
    FD.text[len] = '\0';
    buf_free(&b);
    FD.c1 = 0;
    find_count();
    find_step(-1);
  }
  else return 0;
  return 1;
}


static void draw_find (int x, int y, int w) {
  char t[64];
  int cx;
  FD.w = w < 48 ? w : 48;
  FD.x = x + w - FD.w - 1;
  FD.wy = y;
  if (FD.x < x) FD.x = x;
  scr_fill(FD.x, y, FD.w, S_BOX);
  FD.in_x = FD.x + 1;
  scr_fill(FD.in_x, y, FD.w - 20, S_INPUT);
  if (FD.text[0]) cx = FD.in_x + 1 + scr_putsw(FD.in_x + 1, y, FD.w - 22, FD.text, S_INPUT_ON);
  else {
    scr_putsw(FD.in_x + 1, y, FD.w - 22, "Find", S_INPUT_HINT);
    cx = FD.in_x + 1;
  }
  if (FD.text[0] == '\0') t[0] = '\0';
  else if (FD.total == 0) snprintf(t, sizeof(t), "No results");
  else snprintf(t, sizeof(t), "%d results", FD.total);
  scr_putsw(FD.in_x + FD.w - 19, y, 11, t, FD.total == 0 && FD.text[0] ? S_TOAST_WARN : S_BOX);
  FD.up_x = FD.x + FD.w - 6;
  FD.down_x = FD.x + FD.w - 4;
  FD.close_x = FD.x + FD.w - 2;
  scr_put(FD.up_x, y, 0xEAA1, S_BOX);	/* arrow-up */
  scr_put(FD.down_x, y, 0xEA9A, S_BOX);	/* arrow-down */
  scr_put(FD.close_x, y, 0xEA76, S_BOX);
  scr_cursor(cx, y);
}

/* }================================================================== */


/*
** {==================================================================
** Selection
** ===================================================================
*/

/* VS Code's terminal.integrated.wordSeparators */
static int is_sep (uint32_t ch) {
  static const uint32_t sep[] = {' ', '(', ')', '[', ']', '{', '}', '\'', ',', '"', '`', 0x2500, 0x2018, 0x2019,
                                 0x201C, 0x201D, '|', 0};
  int i;
  for (i = 0; sep[i]; i++)
    if (ch == sep[i]) return 1;
  return 0;
}


/* the character at line y (grid_line's y), column col; ' ' where there is none */
static uint32_t cell_ch (const Term *t, int y, int col) {
  const Line *l = grid_line(t->g, y);
  uint32_t ch;
  if (l == NULL || col < 0 || col >= l->n) return ' ';
  ch = l->c[col].ch;
  if (ch & CH_CLUSTER) ch = grid_base(t->g, &l->c[col]);
  return (ch == 0 || (ch & CH_IMAGE)) ? ' ' : ch;
}


/* the selection's ends in order: lines y0..y1, columns x0 (in) .. x1 (out) */
static void sel_range (long *y0, int *x0, long *y1, int *x1) {
  Term *t = SL.t;
  if (SL.block) {
    *y0 = SL.ay < SL.by ? SL.ay : SL.by;
    *y1 = SL.ay < SL.by ? SL.by : SL.ay;
    *x0 = SL.ax < SL.bx ? SL.ax : SL.bx;
    *x1 = (SL.ax < SL.bx ? SL.bx : SL.ax) + 1;
    return;
  }
  if (SL.ay < SL.by || (SL.ay == SL.by && SL.ax <= SL.bx)) {
    *y0 = SL.ay;
    *x0 = SL.ax;
    *y1 = SL.by;
    *x1 = SL.bx + 1;
  }
  else {
    *y0 = SL.by;
    *x0 = SL.bx;
    *y1 = SL.ay;
    *x1 = SL.ax + 1;
  }
  if (SL.unit == 2) {	/* whole lines */
    *x0 = 0;
    *x1 = t->cols;
  }
  else if (SL.unit == 1) {	/* whole words */
    int a = (int)(*y0 - t->base), b = (int)(*y1 - t->base);
    if (!is_sep(cell_ch(t, a, *x0)))
      while (*x0 > 0 && !is_sep(cell_ch(t, a, *x0 - 1))) (*x0)--;
    if (!is_sep(cell_ch(t, b, *x1 - 1)))
      while (*x1 < t->cols && !is_sep(cell_ch(t, b, *x1))) (*x1)++;
  }
}


static int shown_sel (const Term *t) {
  return SL.t == t && (SL.moved || SL.unit > 0);
}


/* is line y (base's count), column col of t selected */
static int selected (const Term *t, long y, int col) {
  long y0, y1;
  int x0, x1;
  if (!shown_sel(t)) return 0;
  sel_range(&y0, &x0, &y1, &x1);
  if (y < y0 || y > y1) return 0;
  if (SL.block) return col >= x0 && col < x1;
  return (y > y0 || col >= x0) && (y < y1 || col < x1);
}


/* the selected text: its lines joined by \n, a wrapped line's parts not */
static char *sel_text (size_t *len) {
  Term *t = SL.t;
  long y0, y1, y;
  int x0, x1;
  Buf b;
  buf_init(&b);
  if (t) {
    sel_range(&y0, &x0, &y1, &x1);
    for (y = y0; y <= y1; y++) {
      int gy = (int)(y - t->base), c0 = SL.block || y == y0 ? x0 : 0, c1 = SL.block || y == y1 ? x1 : t->cols;
      const Line *l;
      if (gy < -t->g->sb_len || gy >= t->g->rows) continue;
      l = grid_line(t->g, gy);
      cell_text(t, gy, c0, c1, SL.block || !(l && l->wrapped) || y == y1, &b);
      if (y < y1 && (SL.block || l == NULL || !l->wrapped)) buf_putc(&b, '\n');
    }
  }
  *len = b.len;
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/* the selection to the clipboard (and the system's); clear: it goes too */
static void sel_copy (int clear) {
  size_t n;
  char *s;
  if (SL.t == NULL) return;
  s = sel_text(&n);
  if (n > 0) clip_set(s, n);
  free(s);
  if (clear) SL.t = NULL;
}


/* Terminal: Copy Selection */
void panel_copy (void) {
  sel_copy(0);
}


/* Terminal: Select All: the scrollback and the screen */
void panel_select_all (void) {
  if (NONE) return;
  SL.t = &P;
  SL.ay = P.base - P.g->sb_len;
  SL.ax = 0;
  SL.by = P.base + P.rows - 1;
  SL.bx = P.cols - 1;
  SL.block = SL.unit = SL.drag = 0;
  SL.moved = 1;
}

/* }================================================================== */


/*
** {==================================================================
** Drawing
** ===================================================================
*/

#define TERM_FG	ui_color(C_TERM_FG)	/* VS Code's terminal.foreground */
#define TERM_BG	ui_color(C_TERM_BG)


static uint32_t color (uint32_t c, uint32_t def) {
  uint32_t i;
  switch (COL_TAG(c)) {
    case 1:
      i = c & 0xFF;
      if (i < 16) return ui_color(C_ANSI + (int)i);	/* terminal.ansi* */
      if (i < 232) {	/* the 6x6x6 cube */
        static const uint32_t v[6] = {0, 95, 135, 175, 215, 255};
        i -= 16;
        return (v[i / 36] << 16) | (v[(i / 6) % 6] << 8) | v[i % 6];
      }
      i = 8 + (i - 232) * 10;	/* grays */
      return (i << 16) | (i << 8) | i;
    case 2: return c & 0xFFFFFF;
  }
  return def;
}


static int g_hx = -1, g_hy = -1;	/* the mouse, over the panel */
static struct {
  int x, y, w, h;	/* the tabs list; w 0: none */
  int kill_x;	/* the trash on the row in front */
  int row0;	/* the terminal on its first row */
} TL;


void panel_hover (int x, int y) {
  g_hx = x;
  g_hy = y;
}


/* terminal t in x, y, w, h: its screen, the link under the mouse underlined, find's matches lit */
static void draw_term (Term *t, int x, int y, int w, int h, int focus) {
  int row, lrow = -1, lc0 = 0, lc1 = 0, sel = shown_sel(t), mk = 0;
  resize(t, w - 2, h);
  t->x = x;
  t->y = y;
  t->w = w;
  t->h = h;
  scr_box(x, y, w, h, S_PANEL);
  if (g_hy >= y && g_hy < y + h && g_hx > x && g_hx < x + w - 1) {	/* a link under the mouse */
    PanelLink lk;
    int yy = g_hy - y - t->g->view;
    if (link_cell(t, yy, g_hx - x - 1, &lc0, &lc1, &lk)) {
      lrow = g_hy - y;
      free(lk.target);
    }
  }
  if (t->nmark && !t->g->alt) {	/* the first mark that could show */
    long top = t->base - t->g->view;
    while (mk < t->nmark && t->mark[mk].line < top) mk++;
  }
  for (row = 0; row < h && row < t->g->rows; row++) {
    const Line *l = grid_view_line(t->g, row);
    int col, mc0[64], mc1[64], nm = 0, k;
    long ln = t->base + row - t->g->view;
    while (mk < t->nmark && t->mark[mk].line < ln) mk++;
    if (mk < t->nmark && t->mark[mk].line == ln && opt.term_decor && !t->g->alt && t->mark[mk].ran) {
      const Mark *m = &t->mark[mk];	/* the command's circle: blue went well, red failed, grey running */
      uint32_t c = m->exit == 0 ? 0x1B81A8 : m->exit > 0 ? 0xF14C4C : ui_color(C_WS);
      scr_put_rgb(x, y + row, m->exit > 0 ? 0xEA87 : 0xEA71, c, TERM_BG, 0);	/* codicon error, circle-filled */
    }
    if (FD.open && t == &P && FD.text[0]) nm = line_matches(t, row - t->g->view, mc0, mc1, 64);
    for (col = 0; col < t->cols && col < l->n; col++) {
      const Cell *c = &l->c[col];
      uint32_t fg, bg, ch;
      int at = 0;
      if (c->attr & A_WCONT) continue;
      fg = color(c->fg, TERM_FG);
      bg = color(c->bg, TERM_BG);
      if (c->attr & A_REVERSE) {
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
      }
      if (c->attr & A_HIDDEN) fg = bg;
      if (c->attr & A_BOLD) at |= RGB_BOLD;
      if (c->attr & A_ITALIC) at |= RGB_ITALIC;
      if (c->attr & A_UNDER) at |= RGB_UNDER;
      if (row == lrow && col >= lc0 && col < lc1) at |= RGB_UNDER;
      if (sel && selected(t, t->base + row - t->g->view, col)) bg = ui_color(C_SEL_BG);	/* terminal.selectionBackground */
      for (k = 0; k < nm; k++)
        if (col >= mc0[k] && col < mc1[k]) {
          int cur = FD.c1 && row - t->g->view == FD.y && mc0[k] == FD.c0;
          bg = ui_color(cur ? C_ACCENT : C_MATCH_BG);
          if (cur) fg = 0xFFFFFF;
        }
      ch = c->ch;
      if (ch & CH_CLUSTER) ch = grid_base(t->g, c);
      if (ch == 0 || (ch & CH_IMAGE)) ch = ' ';
      scr_put_rgb(x + 1 + col, y + row, ch, fg, bg, at);
    }
  }
  if (opt.term_sticky && t->nmark && !t->g->alt && t->g->view > 0 && h > 2) {	/* the prompt of the command scrolled into, pinned */
    long top = t->base - t->g->view;
    int i;
    for (i = t->nmark - 1; i >= 0 && t->mark[i].line >= top; i--) ;
    if (i >= 0 && t->mark[i].line - t->base >= -t->g->sb_len) {
      const Line *l = grid_line(t->g, (int)(t->mark[i].line - t->base));
      int col;
      uint32_t sbg = ui_color(C_LINE_BG);
      for (col = 0; col < t->cols; col++) scr_put_rgb(x + 1 + col, y, ' ', TERM_FG, sbg, 0);
      for (col = 0; l && col < t->cols && col < l->n; col++) {
        const Cell *c = &l->c[col];
        uint32_t ch = c->ch, bg = color(c->bg, sbg);
        if (c->attr & A_WCONT) continue;
        if (ch & CH_CLUSTER) ch = grid_base(t->g, c);
        if (ch == 0 || (ch & CH_IMAGE)) ch = ' ';
        scr_put_rgb(x + 1 + col, y, ch, color(c->fg, TERM_FG), bg == TERM_BG ? sbg : bg, 0);
      }
    }
  }
  if (focus && t == &P && !FD.open && t->g->view == 0 && t->g->cursor_on && t->g->cy < h)
    scr_cursor(x + 1 + t->g->cx, y + t->g->cy);
}


/* the tabs list on the right: a row a terminal, a group's joined by a line */
static void draw_tabs (int x, int y, int w, int h, int focus) {
  int i, row;
  TL.x = x;
  TL.y = y;
  TL.w = w;
  TL.h = h;
  TL.kill_x = -1;
  for (row = 0; row < h; row++) scr_put(x, y + row, 0x2502, S_BORDER);
  TL.row0 = 0;
  if (g_cur >= h) TL.row0 = g_cur - h + 1;
  for (i = TL.row0, row = 0; i < g_n && row < h; i++, row++) {
    Term *t = g_term[i];
    int st = i == g_cur ? (focus ? S_SIDE_SEL : S_SIDE_ACTIVE) : S_PANEL, cx = x + 1;
    int prev = i > 0 && g_term[i - 1]->grp == t->grp, next = i + 1 < g_n && g_term[i + 1]->grp == t->grp;
    scr_fill(x + 1, y + row, w - 1, st);
    if (prev || next)	/* a split group: ┌ ├ └ */
      scr_put(cx, y + row, !prev ? 0x250C : next ? 0x251C : 0x2514, st);
    cx += 2;
    scr_put(cx, y + row, t->icon ? t->icon : 0xEA85, st);	/* codicon terminal, or Change Icon's */
    if (t->color) scr_set_fg(cx, y + row, ui_color(C_ANSI + t->color - 1));	/* Change Color */
    cx += 2;
    scr_putsw(cx, y + row, x + w - cx - 3, t->name, st);
    if (i == g_cur) {
      TL.kill_x = x + w - 2;
      scr_put(TL.kill_x, y + row, 0xEA81, st);	/* trash */
    }
  }
}


void panel_draw (int x, int y, int w, int h, int focus) {
  int i, m = 0, pw, px, k, tw = 0;
  TL.w = 0;
  for (i = 0; i < g_n; i++) g_term[i]->w = 0;
  if (NONE) {
    scr_box(x, y, w, h, S_PANEL);
    return;
  }
  if (g_n > 1 && w >= 40) {	/* the tabs list */
    tw = w / 5;
    if (tw < 16) tw = 16;
    if (tw > 28) tw = 28;
    draw_tabs(x + w - tw, y, tw, h, focus);
  }
  m = group_size();
  pw = (w - tw - (m - 1)) / m;
  px = x;
  for (i = 0, k = 0; i < g_n; i++) {
    int ww;
    if (g_term[i]->grp != P.grp) continue;
    ww = k == m - 1 ? x + w - tw - px : pw;
    draw_term(g_term[i], px, y, ww, h, focus && i == g_cur);
    px += ww;
    if (k < m - 1) {	/* the line between two */
      int r;
      for (r = 0; r < h; r++) scr_put(px, y + r, 0x2502, S_BORDER);
      px++;
    }
    k++;
  }
  if (FD.open && focus) draw_find(P.x, P.y, P.w);
}


/* a mouse event as a program that asked for them wants it (1000, 1002, 1003; SGR or bytes) */
static void mouse_report (Term *t, const Mouse *m) {
  int x = m->x - t->x, y = m->y - t->y + 1, b;
  char buf[48];
  if (t->pty == NULL || x < 1 || y < 1 || x > t->cols || y > t->rows) return;
  if (m->wheel) b = m->wheel < 0 ? 64 : 65;
  else if (m->button == 3) {	/* it moved, no button down */
    if (t->g->mouse != 1003) return;
    b = 3 + 32;
  }
  else {
    b = m->button;
    if (m->drag) {
      if (t->g->mouse < 1002) return;
      b += 32;
    }
  }
  if (m->mods & KM_SHIFT) b += 4;
  if (m->mods & KM_ALT) b += 8;
  if (m->mods & KM_CTRL) b += 16;
  if (t->g->mouse_sgr) snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c", b, x, y, m->press || m->wheel || m->drag ? 'M' : 'm');
  else {
    if (!m->press && !m->wheel && !m->drag) b = (b & ~3) | 3;	/* up */
    if (x > 223 || y > 223 || t->g->mouse == 9) return;
    snprintf(buf, sizeof(buf), "\033[M%c%c%c", 32 + b, 32 + x, 32 + y);
  }
  pty_write(t->pty, buf, strlen(buf));
}


int panel_dragging (void) {
  return SL.drag;
}


/* the column of t under screen column x, kept inside it */
static int col_at (const Term *t, int x) {
  int c = x - t->x - 1;
  return c < 0 ? 0 : c >= t->cols ? t->cols - 1 : c;
}


static void paste_clip (void);

/*
** The mouse in the panel's body (and, while a selection is dragged,
** anywhere): 1 used, 2 a link to open (in *lk), 3 the last terminal went,
** 100 + a command from the right-click menu.
*/
int panel_mouse (const Mouse *m, int focused, PanelLink *lk) {
  int i, press = m->press && !m->drag, x = m->x, y = m->y;
  Term *t = NULL;
  long ln;
  int col;
  (void)focused;	/* a link opens with Ctrl+click, focused or not, like VS Code */
  if (NONE) return 0;
  if (SL.drag) {	/* a selection is being made */
    t = SL.t;
    if (t == NULL) {
      SL.drag = 0;
      return 1;
    }
    if (m->drag && m->button == 0) {
      long row = y - t->y;
      if (row < 0) {	/* above it: the scrollback comes down */
        grid_set_view(t->g, t->g->view + 1);
        row = 0;
      }
      else if (row >= t->h) {
        grid_set_view(t->g, t->g->view - 1);
        row = t->h - 1;
      }
      SL.by = t->base + row - t->g->view;
      SL.bx = col_at(t, x);
      SL.moved = 1;
    }
    else if (!m->press) {	/* the button came up */
      SL.drag = 0;
      if (!SL.moved && SL.unit == 0) SL.t = NULL;	/* a click, no selection */
      else if (opt.term_copy_sel) sel_copy(0);	/* terminal.integrated.copyOnSelection */
    }
    return 1;
  }
  if (press && FD.open && y == FD.wy && x >= FD.x && x < FD.x + FD.w) {	/* the find widget */
    if (x == FD.close_x) FD.open = FD.c1 = 0;
    else if (x == FD.up_x) find_step(-1);
    else if (x == FD.down_x) find_step(1);
    return 1;
  }
  if (TL.w > 0 && x >= TL.x && x < TL.x + TL.w && y >= TL.y && y < TL.y + TL.h) {	/* the tabs list */
    int k = TL.row0 + (y - TL.y);
    if (!press || m->button != 0 || k >= g_n) return 1;
    if (k == g_cur && x == TL.kill_x) {
      if (panel_confirm_kill()) term_free(g_cur);
      return NONE ? 3 : 1;
    }
    g_cur = k;
    return 1;
  }
  for (i = 0; i < g_n && t == NULL; i++)
    if (g_term[i]->w && x >= g_term[i]->x && x < g_term[i]->x + g_term[i]->w && y >= g_term[i]->y &&
        y < g_term[i]->y + g_term[i]->h)
      t = g_term[i];
  if (t == NULL) return 0;
  i--;
  if (t->g->mouse && t->pty && !(m->mods & KM_SHIFT)) {	/* the program wants the mouse (Shift: select anyway) */
    if (press) g_cur = i;
    mouse_report(t, m);
    return 1;
  }
  if (!press) return 1;
  ln = t->base + (y - t->y) - t->g->view;
  col = col_at(t, x);
  if (m->button == 2) {	/* terminal.integrated.rightClickBehavior */
    g_cur = i;
    switch (opt.term_right_click) {
      case RC_COPY_PASTE:
        if (shown_sel(t)) sel_copy(1);
        else paste_clip();
        break;
      case RC_PASTE: paste_clip(); break;
      case RC_SELECT_WORD:
        SL.t = t;
        SL.ay = SL.by = ln;
        SL.ax = SL.bx = col;
        SL.unit = 1;
        SL.block = SL.drag = 0;
        SL.moved = 1;
        break;
      case RC_DEFAULT: {	/* the menu, like VS Code's */
        static const int cmd[] = {CMD_TERM_COPY, CMD_TERM_PASTE, CMD_TERM_SELECT_ALL, 0, CMD_TERMINAL_CLEAR, 0,
                                  CMD_TERMINAL_SPLIT, CMD_TERMINAL_KILL, -1};
        static const char *const label[] = {"Copy", "Paste", "Select All", NULL, "Clear Terminal", NULL,
                                            "Split Terminal", "Kill Terminal"};
        int c = menu_popup(x, y, cmd, label);
        if (c > 0) return 100 + c;
        break;
      }
    }
    return 1;
  }
  if (m->button != 0) return 1;
  if (m->mods & KM_CTRL) {	/* Ctrl+click: a link */
    int c0, c1;
    if (link_cell(t, (int)(ln - t->base), col, &c0, &c1, lk)) {
      g_cur = i;
      SL.t = NULL;
      return 2;
    }
  }
  {	/* a selection starts: a click, a double one (words), a triple one (lines) */
    long long now = os_now_us();
    if (SL.ct == t && now - SL.at_us < 500000 && SL.cy == ln && abs(SL.cx - col) <= 1) SL.clicks = SL.clicks % 3 + 1;
    else SL.clicks = 1;
    SL.ct = t;
    SL.at_us = now;
    SL.cx = col;
    SL.cy = ln;
  }
  g_cur = i;
  SL.t = t;
  SL.ay = SL.by = ln;
  SL.ax = SL.bx = col;
  SL.block = (m->mods & KM_ALT) != 0;
  SL.unit = SL.clicks - 1;
  SL.drag = 1;
  SL.moved = 0;
  return 1;
}


/* a click in the panel's body: 1 used, 2 a link to open (in *lk), 3 the last terminal went */
int panel_click (int x, int y, int mods, int focused, PanelLink *lk) {
  int i;
  if (NONE) return 0;
  if (FD.open && y == FD.wy && x >= FD.x && x < FD.x + FD.w) {	/* the find widget */
    if (x == FD.close_x) FD.open = FD.c1 = 0;
    else if (x == FD.up_x) find_step(-1);
    else if (x == FD.down_x) find_step(1);
    return 1;
  }
  if (TL.w > 0 && x >= TL.x && x < TL.x + TL.w && y >= TL.y && y < TL.y + TL.h) {	/* the tabs list */
    int t = TL.row0 + (y - TL.y);
    if (t >= g_n) return 1;
    if (t == g_cur && x == TL.kill_x) {
      if (panel_confirm_kill()) term_free(g_cur);
      return NONE ? 3 : 1;
    }
    g_cur = t;
    return 1;
  }
  for (i = 0; i < g_n; i++) {
    Term *t = g_term[i];
    int c0, c1;
    if (t->w == 0 || x < t->x || x >= t->x + t->w || y < t->y || y >= t->y + t->h) continue;
    if (((mods & KM_CTRL) || (focused && i == g_cur)) &&
        link_cell(t, y - t->y - t->g->view, x - t->x - 1, &c0, &c1, lk)) {
      g_cur = i;
      return 2;
    }
    g_cur = i;
    return 1;
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Keys
** ===================================================================
*/

static void send (const char *s) {
  if (!NONE && P.pty) pty_write(P.pty, s, strlen(s));
}


/* bytes to the shell as they are (workbench.action.terminal.sendSequence) */
void panel_send (const char *s, size_t n) {
  if (!NONE && P.pty) pty_write(P.pty, s, n);
}


/*
** Text pasted: \n as Enter (\r), inside bracketed paste when the shell asked
** for it; more than one line asks first (terminal.integrated.enableMultiLinePasteWarning).
*/
void panel_paste (const char *s, size_t n) {
  size_t i, lines = 0;
  int one = 0;
  Buf b;
  if (NONE || P.pty == NULL || n == 0) return;
  for (i = 0; i < n; i++)
    if (s[i] == '\n' && i + 1 < n) lines++;
  if (lines && (opt.term_paste_warn == 2 || (opt.term_paste_warn == 1 && !P.g->bracketed))) {
    static const char *const bt[] = {"Paste", "Paste as one line", "Cancel"};
    char msg[128];
    snprintf(msg, sizeof(msg), "Are you sure you want to paste %lu lines of text into the terminal?", (unsigned long)lines + 1);
    switch (dialog(msg, NULL, bt, 3)) {
      case 0: break;
      case 1: one = 1; break;
      default: return;
    }
  }
  buf_init(&b);
  for (i = 0; i < n; i++) {
    if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') continue;
    if (s[i] == '\n' || s[i] == '\r') {
      if (!one) buf_putc(&b, '\r');
      else if (i + 1 < n && b.len && b.s[b.len - 1] != ' ') buf_putc(&b, ' ');
    }
    else buf_putc(&b, s[i]);
  }
  if (P.g->bracketed) send("\033[200~");
  if (b.len) pty_write(P.pty, b.s, b.len);
  if (P.g->bracketed) send("\033[201~");
  buf_free(&b);
  SL.t = NULL;
  grid_set_view(P.g, 0);
}


/* the clipboard's text pasted (Ctrl+V, Ctrl+Shift+V, the right button) */
static void paste_clip (void) {
  size_t n;
  const char *s = clip_get(&n);
  if (s && n) panel_paste(s, n);
}


void panel_paste_clip (void) {
  paste_clip();
}


/* Terminal: Scroll to Previous / Next Command (Ctrl+Up, Ctrl+Down): its prompt on the top row */
void panel_scroll_cmd (int d) {
  long top;
  int i;
  if (NONE || P.g->alt || P.nmark == 0) return;
  top = P.base - P.g->view;
  if (d < 0)
    for (i = P.nmark - 1; i >= 0 && P.mark[i].line >= top; i--) ;
  else
    for (i = 0; i < P.nmark && P.mark[i].line <= top; i++) ;
  if (i < 0) return;
  if (i >= P.nmark || P.mark[i].line >= P.base) grid_set_view(P.g, 0);	/* on the screen: the bottom */
  else grid_set_view(P.g, (int)(P.base - P.mark[i].line));
}


/* text typed into the terminal in front and Enter: Run Selected Text, Run Active File, Run Recent Command */
void panel_run_text (const char *s, size_t n) {
  Buf b;
  size_t i;
  int multi = 0;
  if (NONE || P.pty == NULL) return;
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
  buf_init(&b);
  for (i = 0; i < n; i++) {
    if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') continue;
    if (s[i] == '\n') multi = 1;
    buf_putc(&b, s[i] == '\n' ? '\r' : s[i]);
  }
  if (multi && P.g->bracketed) send("\033[200~");
  if (b.len) pty_write(P.pty, b.s, b.len);
  if (multi && P.g->bracketed) send("\033[201~");
  send("\r");
  buf_free(&b);
  SL.t = NULL;
  grid_set_view(P.g, 0);
}


/* path, quoted as the shell in front wants it */
static void put_path (Buf *b, const char *path, int kind) {
  const char *c;
  if (kind == SH_PS) {	/* & 'x' */
    buf_puts(b, "& '");
    for (c = path; *c; c++) {
      if (*c == '\'') buf_putc(b, '\'');
      buf_putc(b, *c);
    }
    buf_putc(b, '\'');
  }
  else if (kind == SH_BASH) {
    buf_putc(b, '\'');
    for (c = path; *c; c++) {
      if (*c == '\'') buf_puts(b, "'\\''");
      else buf_putc(b, *c == '\\' ? '/' : *c);
    }
    buf_putc(b, '\'');
  }
  else if (strpbrk(path, " &()^;,") != NULL) {
    buf_putc(b, '"');
    buf_puts(b, path);
    buf_putc(b, '"');
  }
  else buf_puts(b, path);
}


/* Run Active File In Active Terminal */
void panel_run_file (const char *path) {
  Buf b;
  if (NONE) return;
  buf_init(&b);
  put_path(&b, path, P.kind);
  panel_run_text(b.s, b.len);
  buf_free(&b);
}


/* Run Recent Command (Ctrl+Alt+R): this terminal's, then the history's */
void panel_recent_command (void) {
  Pick p;
  Vec all;
  size_t k;
  int i, r;
  if (NONE) return;
  hist_load();
  vec_init(&all);
  pick_init(&p, "Select a command to run");
  for (i = P.nmark - 1; i >= 0; i--) {
    const char *c = P.mark[i].cmd;
    int dup = 0;
    if (c == NULL) continue;
    for (k = 0; k < all.n && !dup; k++) dup = strcmp(all.v[k], c) == 0;
    if (dup) continue;
    vec_push(&all, xstrdup(c));
    pick_add(&p, c, "current session", P.mark[i].exit > 0 ? 0xEA87 : 0xEA71);
  }
  for (k = 0; k < g_hist.n; k++) {
    size_t j;
    int dup = 0;
    for (j = 0; j < all.n && !dup; j++) dup = strcmp(all.v[j], g_hist.v[k]) == 0;
    if (dup) continue;
    vec_push(&all, xstrdup(g_hist.v[k]));
    pick_add(&p, g_hist.v[k], "history", 0xEA82);	/* codicon history */
  }
  p.keep_order = 0;
  p.hint = "No commands yet: shell integration sees them as they run.";
  r = pick_run(&p);
  if (r >= 0 && (size_t)r < all.n) panel_run_text(all.v[r], strlen(all.v[r]));
  pick_free(&p);
  vec_free(&all);
}


/* Go to Recent Directory (Ctrl+G): cd there */
void panel_recent_dir (void) {
  Pick p;
  size_t k;
  int r;
  if (NONE) return;
  hist_load();
  pick_init(&p, "Select a directory to go to");
  for (k = 0; k < g_dirs.n; k++) pick_add(&p, g_dirs.v[k], NULL, 0xEA83);	/* codicon folder */
  p.hint = "No directories yet: shell integration sees them as the shell goes there.";
  r = pick_run(&p);
  if (r >= 0 && (size_t)r < g_dirs.n) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, P.kind == SH_CMD ? "cd /d " : "cd ");
    if (P.kind == SH_PS) {	/* cd 'x' */
      const char *c;
      buf_putc(&b, '\'');
      for (c = g_dirs.v[r]; *c; c++) {
        if (*c == '\'') buf_putc(&b, '\'');
        buf_putc(&b, *c);
      }
      buf_putc(&b, '\'');
    }
    else put_path(&b, g_dirs.v[r], P.kind);
    panel_run_text(b.s, b.len);
    buf_free(&b);
  }
  pick_free(&p);
}


/* Terminal: Change Color... */
void panel_change_color (void) {
  static const char *const name[] = {"Default", "Black", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"};
  Pick p;
  int i, r;
  if (NONE) return;
  pick_init(&p, "Select a color for the terminal");
  p.keep_order = 1;
  for (i = 0; i < 9; i++) pick_add(&p, name[i], i == P.color ? "current" : NULL, 0xEA71);
  p.start = P.color;
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) P.color = r;	/* 1 black .. 8 white: ANSI 0 .. 7 */
}


/* Terminal: Change Icon... */
void panel_change_icon (void) {
  static const struct {
    const char *name;
    uint32_t cp;
  } ic[] = {
    {"terminal", 0xEA85}, {"terminal-bash", 0xEBCA}, {"terminal-cmd", 0xEBC4}, {"terminal-powershell", 0xEBC7},
    {"terminal-linux", 0xEBC6}, {"terminal-ubuntu", 0xEBC9}, {"terminal-debian", 0xEBC5}, {"code", 0xEAC4},
    {"bug", 0xEAAF}, {"beaker", 0xEA79}, {"flame", 0xEAF2}, {"rocket", 0xEB44}, {"server", 0xEB50},
    {"globe", 0xEB01}, {"gear", 0xEAF8}, {"tools", 0xEB6D}, {"zap", 0xEA86}, {"star-full", 0xEB59}
  };
  Pick p;
  int i, r, n = (int)(sizeof(ic) / sizeof(ic[0]));
  if (NONE) return;
  pick_init(&p, "Select an icon for the terminal");
  for (i = 0; i < n; i++) pick_add(&p, ic[i].name, NULL, (int)ic[i].cp);
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0 && r < n) P.icon = r == 0 ? 0 : ic[r].cp;
}


/* the cursor's shape in the terminal: the program's (DECSCUSR), else terminal.integrated.cursorStyle */
int panel_cursor_shape (void) {
  static const int shape[] = {2, 6, 4};	/* block, line, underline, steady; one less blinks */
  if (NONE) return 2;
  if (P.g->cursor_shape > 0) return P.g->cursor_shape;
  return shape[opt.term_cursor_style] - (opt.term_cursor_blink ? 1 : 0);
}


/* the terminal's own keys, before the shell gets them: copy, paste, commands; 1 used */
int panel_key_cmd (int k) {
  int code = KEY_CODE(k);
  if (NONE) return 0;
  if (shown_sel(&P) && (k == CTRL('c') || k == ('c' | KM_CTRL | KM_SHIFT) || k == ('C' | KM_CTRL | KM_SHIFT))) {
    sel_copy(1);	/* Ctrl+C with a selection copies, like VS Code on Windows */
    return 1;
  }
  if (k == ('c' | KM_CTRL | KM_SHIFT) || k == ('C' | KM_CTRL | KM_SHIFT)) return 1;
#ifdef _WIN32
  if (k == CTRL('v')) {
    paste_clip();
    return 1;
  }
#endif
  if (k == ('v' | KM_CTRL | KM_SHIFT) || k == ('V' | KM_CTRL | KM_SHIFT)) {
    paste_clip();
    return 1;
  }
  if ((code == K_UP || code == K_DOWN) && (k & (KM_CTRL | KM_SHIFT | KM_ALT)) == KM_CTRL && P.nmark > 0 &&
      opt.term_shell_int && !P.g->alt) {
    panel_scroll_cmd(code == K_UP ? -1 : 1);
    return 1;
  }
  if (k == ('r' | KM_CTRL | KM_ALT) || k == (CTRL('r') | KM_ALT)) {
    panel_recent_command();
    return 1;
  }
  if (k == CTRL('g') && opt.term_shell_int && !P.g->alt) {
    hist_load();
    if (g_dirs.n > 0) {
      panel_recent_dir();
      return 1;
    }
  }
  if (code == K_ESC && shown_sel(&P)) {
    SL.t = NULL;
    return 1;
  }
  if (SL.t && !SL.drag) SL.t = NULL;	/* typing: the selection goes */
  return 0;
}


/* a key as a terminal sends it: ESC [ 1 ; m A for Ctrl+Up and so on */
void panel_key (int k) {
  int code = KEY_CODE(k), m = 1, i;
  char buf[32];
  static const struct {
    int key;
    const char *seq;	/* %s: the modifiers, "1;m" or ... */
    char final;
  } fn[] = {
    {K_UP, "", 'A'}, {K_DOWN, "", 'B'}, {K_RIGHT, "", 'C'}, {K_LEFT, "", 'D'},
    {K_HOME, "", 'H'}, {K_END, "", 'F'}, {K_F1, "", 'P'}, {K_F2, "", 'Q'},
    {K_F3, "", 'R'}, {K_F4, "", 'S'}, {K_INS, "2", '~'}, {K_DEL, "3", '~'},
    {K_PGUP, "5", '~'}, {K_PGDN, "6", '~'}, {K_F5, "15", '~'}, {K_F6, "17", '~'},
    {K_F7, "18", '~'}, {K_F8, "19", '~'}, {K_F9, "20", '~'}, {K_F10, "21", '~'},
    {K_F11, "23", '~'}, {K_F12, "24", '~'}
  };
  if (NONE) return;
  if (P.done) {	/* a finished task's terminal: any key closes it */
    term_free(g_cur);
    return;
  }
  if ((code == K_PGUP || code == K_PGDN) && (k & (KM_SHIFT | KM_CTRL | KM_ALT)) == KM_SHIFT && !P.g->alt) {
    panel_scroll(0, code == K_PGUP ? -1 : 1);	/* Shift+PgUp: the scrollback, like VS Code */
    return;
  }
  if ((code == K_UP || code == K_DOWN) && (k & (KM_SHIFT | KM_CTRL)) == (KM_SHIFT | KM_CTRL) && !P.g->alt) {
    panel_scroll(code == K_UP ? -1 : 1, 0);	/* Ctrl+Shift+Up: a line */
    return;
  }
  if (P.g->view != 0) grid_set_view(P.g, 0);	/* typing shows the bottom again */
  if (k & KM_SHIFT) m += 1;
  if (k & KM_ALT) m += 2;
  if (k & KM_CTRL) m += 4;
  for (i = 0; i < (int)(sizeof(fn) / sizeof(fn[0])); i++) {
    if (code != fn[i].key) continue;
    if (fn[i].final == '~') {
      if (m > 1) snprintf(buf, sizeof(buf), "\033[%s;%d~", fn[i].seq, m);
      else snprintf(buf, sizeof(buf), "\033[%s~", fn[i].seq);
    }
    else if (m > 1) snprintf(buf, sizeof(buf), "\033[1;%d%c", m, fn[i].final);
    else if (code >= K_F1 || P.g->app_cursor) snprintf(buf, sizeof(buf), "\033O%c", fn[i].final);
    else snprintf(buf, sizeof(buf), "\033[%c", fn[i].final);
    send(buf);
    return;
  }
  switch (code) {
    case K_ENTER: send((k & KM_ALT) ? "\033\r" : "\r"); return;
    case K_TAB: send((k & KM_SHIFT) ? "\033[Z" : "\t"); return;
    case K_ESC: send("\033"); return;
    case K_BS:
      if (k & KM_ALT) send("\033\177");
      else send((k & KM_CTRL) ? "\b" : "\177");
      return;
  }
  if (code >= K_UP) return;
  if ((k & KM_CTRL) && code >= 'a' && code <= 'z') code = CTRL(code);	/* Ctrl+Shift+x */
  if (k & KM_ALT) send("\033");
  buf[utf8_encode((uint32_t)code, buf)] = '\0';
  if (code == 0) pty_write(P.pty, "", 1);	/* Ctrl+Space, Ctrl+@: NUL */
  else send(buf);
}


void panel_wheel (int d) {
  if (NONE) return;
  if (g_hx >= 0) {	/* the terminal under the mouse */
    int i;
    for (i = 0; i < g_n; i++) {
      Term *t = g_term[i];
      if (t->w && g_hx >= t->x && g_hx < t->x + t->w && g_hy >= t->y && g_hy < t->y + t->h) {
        if (t->g->mouse && t->pty) {	/* the program wants the wheel */
          Mouse m;
          memset(&m, 0, sizeof(m));
          m.x = g_hx;
          m.y = g_hy;
          m.wheel = d;
          mouse_report(t, &m);
          return;
        }
        if (t->g->alt) break;
        grid_set_view(t->g, t->g->view - d * 3);
        return;
      }
    }
  }
  if (P.g->alt) {	/* a full screen program: it gets arrows, like VS Code does */
    int i;
    for (i = 0; i < 3; i++) send(d < 0 ? (P.g->app_cursor ? "\033OA" : "\033[A")
                                       : (P.g->app_cursor ? "\033OB" : "\033[B"));
    return;
  }
  grid_set_view(P.g, P.g->view - d * wheel_step(0));	/* the scrollback */
}

/* }================================================================== */
