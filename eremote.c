/*
** eremote.c - Remote-SSH, as VS Code's: a window whose folder is on another
** machine. mme runs there (all of it: its files, the search, git, the
** language servers, the terminal), over ssh; this window is its terminal.
**
** "Remote-SSH: Connect to Host..." asks the host (~/.ssh/config's, or
** user@host) and a folder there, then opens a window running
** `mme --remote=host::folder`: it shows a terminal that runs a script
** (mme-data/remote/connect.cmd or .sh) - ssh looks whether mme is on the
** host (~/.mme-server/mme, or mme in its PATH), copies the build for it
** there when it is not (mme-<arch>-linux, from mme-server next to this
** program or a dist folder; `build cross` makes them), then runs it on the
** folder with ssh -t. ssh asks for passwords in that terminal. When the
** remote mme ends, the window closes.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int remote_mode;	/* this window is a remote one: the SDL window keeps its system frame */
int remote_close;	/* the window's x: once asks the remote mme to close (Ctrl+Shift+W), twice ends the window */


/* the hosts of ~/.ssh/config (Host lines, not their patterns) */
static void config_hosts (Vec *out) {
  char *home = os_getenv("HOME"), *d, *cfg, *s;
  size_t i, n = 0;
  if (home == NULL) home = os_getenv("USERPROFILE");
  if (home == NULL) return;
  d = path_join(home, ".ssh");
  cfg = path_join(d, "config");
  free(d);
  free(home);
  s = read_file(cfg, &n);
  free(cfg);
  if (s == NULL) return;
  for (i = 0; i < n;) {
    size_t e = i, k;
    while (e < n && s[e] != '\n') e++;
    k = i;
    while (k < e && (s[k] == ' ' || s[k] == '\t')) k++;
    if (e - k > 5 && m_strnicmp(s + k, "Host", 4) == 0 && (s[k + 4] == ' ' || s[k + 4] == '\t')) {
      k += 4;
      while (k < e) {
        size_t w;
        while (k < e && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r')) k++;
        w = k;
        while (k < e && s[k] != ' ' && s[k] != '\t' && s[k] != '\r') k++;
        if (k > w && !memchr(s + w, '*', k - w) && !memchr(s + w, '?', k - w) && !memchr(s + w, '!', k - w))
          vec_push(out, xstrndup(s + w, k - w));
      }
    }
    i = e + 1;
  }
  free(s);
}


/* Remote-SSH: Connect to Host...: the host, a folder there, then a window of its own */
void remote_connect (void) {
  Vec hosts;
  Pick p;
  size_t i;
  int r;
  char *host = NULL, *folder, *arg;
  if (find_program("ssh") == NULL) {
    toast(1, "Remote-SSH: ssh was not found (Windows: Settings > Optional features > OpenSSH Client).");
    return;
  }
  vec_init(&hosts);
  config_hosts(&hosts);
  pick_init(&p, "Select configured SSH host or enter user@host");
  for (i = 0; i < hosts.n; i++) pick_add(&p, hosts.v[i], "~/.ssh/config", 0xEB50);	/* remote */
  p.hint = "Type user@host and press Enter";
  r = pick_run(&p);
  if (r >= 0 && (size_t)r < hosts.n) host = xstrdup(hosts.v[r]);
  else if (r == PICK_TEXT && p.text[0]) host = xstrdup(p.text);
  pick_free(&p);
  vec_free(&hosts);
  if (host == NULL) return;
  folder = ask_text("Remote-SSH: the folder to open on the host", "~");
  if (folder == NULL) {
    free(host);
    return;
  }
  arg = (char *)xmalloc(strlen(host) + strlen(folder) + 16);
  sprintf(arg, "--remote=%s::%s", host, *folder ? folder : "~");
  if (term_new_window(arg) != 0) toast(1, "Remote-SSH: a window could not be opened here (run mme-sdl, or mme in mmc-term).");
  else toast(0, "Remote-SSH: connecting to %s in a new window...", host);
  free(arg);
  free(folder);
  free(host);
}


/* the build of mme for a host's `uname -sm`, where this program finds it; NULL: none */
static char *server_build (const char *name) {
  char *exe = os_exe_path(NULL), *dir, *up, *up2, *c[4], *found = NULL;
  int i;
  OsStat st;
  if (exe == NULL) return NULL;
  dir = path_dirname(exe);
  up = path_dirname(dir);
  up2 = path_dirname(up);
  c[0] = path_join(dir, "mme-server");	/* next to the program (the shell's usr/bin) */
  c[1] = path_join(dir, "dist");
  c[2] = path_join(up, "dist");	/* E:\w\editor\dist, beside mme.exe's folder */
  c[3] = path_join(up2, "dist");	/* and beside sdl2_port\bin */
  for (i = 0; i < 4 && found == NULL; i++) {
    char *f = path_join(c[i], name);
    if (os_stat(f, &st) == 0 && st.exists && !st.is_dir) found = f;
    else free(f);
  }
  for (i = 0; i < 4; i++) free(c[i]);
  free(up2);
  free(up);
  free(dir);
  free(exe);
  return found;
}


/*
** The remote shell's command that runs mme on the folder (~ and ~/x as
** $HOME's): M=$HOME/.mme-server/mme; [ -x "$M" ] || M=mme; exec "$M" "folder".
** q: how a double quote is written for the local side (\" in a Windows
** argument, " inside the single quotes of sh)
*/
static void remote_run (Buf *b, const char *folder, const char *q) {
  const char *p;
  buf_printf(b, "export MME_REMOTE=1; M=$HOME/.mme-server/mme; [ -x %s$M%s ] || M=mme; exec %s$M%s %s", q, q, q, q, q);
  if (folder[0] == '~') {
    buf_puts(b, "$HOME");
    folder++;
  }
  for (p = folder; *p; p++) {
    if (*p == '"' || *p == '`' || *p == '\\') buf_puts(b, strcmp(q, "\"") == 0 ? "\\" : "\\\\");
    if (*p == '\'' && strcmp(q, "\"") == 0) buf_puts(b, "'\\''");	/* sh: out of the quotes and back */
    else buf_putc(b, *p);
  }
  buf_puts(b, q);
}


/* the script the window's terminal runs: look, install when needed, run */
static char *connect_script (const char *host, const char *folder) {
  static const char *const arch[][2] = {
    {"Linux x86_64", "mme-x86_64-linux"}, {"Linux aarch64", "mme-aarch64-linux"}, {"Linux arm64", "mme-aarch64-linux"},
    {"Linux armv7l", "mme-arm-linux"}, {"Linux armv6l", "mme-arm-linux"}, {"Darwin x86_64", "mme-x86_64-macos"},
    {"Darwin arm64", "mme-aarch64-macos"}
  };
  char *dir = data_path("remote"), *f;
  Buf s, run;
  size_t i;
  int fd;
  os_mkdir(dir);
  buf_init(&run);	/* the last step: the remote mme, on its folder */
#ifdef _WIN32
  remote_run(&run, folder, "\\\"");
#else
  remote_run(&run, folder, "\"");
#endif
  buf_putc(&run, '\0');
  buf_init(&s);
#ifdef _WIN32
  f = path_join(dir, "connect.cmd");
  buf_printf(&s, "@echo off\r\ncls\r\necho Connecting to %s...\r\n", host);
  buf_printf(&s, "call ssh %s \"test -x ~/.mme-server/mme || command -v mme >/dev/null 2>&1\"\r\n", host);
  buf_puts(&s, "if errorlevel 255 goto fail\r\nif not errorlevel 1 goto run\r\n");
  buf_printf(&s, "for /f \"delims=\" %%%%a in ('ssh %s uname -sm') do set U=%%%%a\r\nset BIN=\r\n", host);
  for (i = 0; i < sizeof(arch) / sizeof(arch[0]); i++) {
    char *b = server_build(arch[i][1]);
    if (b) buf_printf(&s, "if \"%%U%%\"==\"%s\" set BIN=%s\r\n", arch[i][0], b);
    free(b);
  }
  buf_printf(&s, "if \"%%BIN%%\"==\"\" goto nobuild\r\necho Installing mme on %s (%%U%%)...\r\n", host);
  buf_printf(&s, "call ssh %s \"mkdir -p ~/.mme-server && cat > ~/.mme-server/mme.new && chmod +x ~/.mme-server/mme.new && "
                 "mv -f ~/.mme-server/mme.new ~/.mme-server/mme\" < \"%%BIN%%\"\r\nif errorlevel 1 goto fail\r\n", host);
  buf_printf(&s, ":run\r\ncall ssh -t %s \"%s\"\r\nexit /b 0\r\n", host, run.s);
  buf_printf(&s, ":nobuild\r\necho mme has no build here for %%U%% (run build cross), and %s has no mme: install it there.\r\n"
                 "pause\r\nexit /b 1\r\n", host);
  buf_printf(&s, ":fail\r\necho Could not connect to %s.\r\npause\r\nexit /b 1\r\n", host);
#else
  f = path_join(dir, "connect.sh");
  buf_printf(&s, "#!/bin/sh\nprintf '\\033[2J\\033[H'\necho 'Connecting to %s...'\n", host);
  buf_printf(&s, "ssh '%s' 'test -x ~/.mme-server/mme || command -v mme >/dev/null 2>&1'\nrc=$?\n", host);
  buf_puts(&s, "if [ $rc = 255 ]; then echo 'Could not connect.'; read x; exit 1; fi\nif [ $rc != 0 ]; then\n");
  buf_printf(&s, "  U=$(ssh '%s' uname -sm)\n  BIN=\n  case \"$U\" in\n", host);
  for (i = 0; i < sizeof(arch) / sizeof(arch[0]); i++) {
    char *b = server_build(arch[i][1]);
    if (b) buf_printf(&s, "    '%s') BIN='%s';;\n", arch[i][0], b);
    free(b);
  }
  buf_printf(&s, "  esac\n  if [ -z \"$BIN\" ]; then echo \"mme has no build here for $U: install mme on %s.\"; read x; exit 1; fi\n", host);
  buf_printf(&s, "  echo \"Installing mme on %s ($U)...\"\n", host);
  buf_printf(&s, "  ssh '%s' 'mkdir -p ~/.mme-server && cat > ~/.mme-server/mme.new && chmod +x ~/.mme-server/mme.new && "
                 "mv -f ~/.mme-server/mme.new ~/.mme-server/mme' < \"$BIN\" || { echo 'Could not install.'; read x; exit 1; }\nfi\n", host);
  /* its connection is the master of the ports' ssh (eports.c): they need no password */
  buf_printf(&s, "exec ssh -t -o ControlMaster=auto -o 'ControlPath=~/.ssh/mme-%%r@%%h-%%p' -o ControlPersist=10 '%s' '%s'\n",
             host, run.s);
#endif
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, s.s, s.len);
    os_close(fd);
  }
  buf_free(&s);
  buf_free(&run);
  free(dir);
  return f;
}


/*
** The remote window: a terminal as big as it, running the script; the
** keys, the mouse, pastes go to it. It ends with the remote mme.
*/
int remote_main (const char *spec) {
  const char *sep = strstr(spec, "::");
  char host[256], folder[1024], *script, *cmd, *home;
  int cols = 80, rows = 24, done = 0;
  remote_mode = 1;
  if (sep) {
    snprintf(host, sizeof(host), "%.*s", (int)(sep - spec), spec);
    snprintf(folder, sizeof(folder), "%s", sep + 2);
  }
  else {
    snprintf(host, sizeof(host), "%s", spec);
    snprintf(folder, sizeof(folder), "~");
  }
  if (term_open() != 0) return 1;
  snprintf(ui_title, sizeof(ui_title), "%s [SSH: %s]", folder, host);
  term_size(&cols, &rows);
  scr_resize(cols, rows);
  script = connect_script(host, folder);
#ifdef _WIN32
  cmd = (char *)xmalloc(strlen(script) + 8);
  sprintf(cmd, strchr(script, ' ') ? "\"%s\"" : "%s", script);
#else
  cmd = (char *)xmalloc(strlen(script) + 8);
  sprintf(cmd, "sh '%s'", script);
#endif
  home = os_getenv("HOME");
  if (home == NULL) home = os_getenv("USERPROFILE");
  panel_run(cols, rows, host, cmd, home, 0);
  ports_host(host);
  free(home);
  free(cmd);
  free(script);
  while (!done) {
    int k, c2, r2;
    if (panel_poll() == 2 || !panel_alive() || remote_close >= 2) break;	/* the remote mme (ssh) ended; the x twice */
    ports_poll();
    term_size(&c2, &r2);
    if (c2 != cols || r2 != rows) {
      cols = c2;
      rows = r2;
      scr_resize(cols, rows);
    }
    scr_clear(S_PANEL);
    panel_draw(0, 0, cols, rows, 1);
    scr_cursor_shape(panel_cursor_shape());
    scr_flush();
    k = term_key(20);
    if (k == K_NONE) continue;
    if (KEY_CODE(k) == K_MOUSE) {	/* to the remote mme (it asks for the mouse) */
      PanelLink lk;
      memset(&lk, 0, sizeof(lk));
      panel_mouse(&term_mouse, 1, &lk);
      continue;
    }
    if (IS_PASTE(k)) {	/* this computer's clipboard, pasted there (a bracketed paste: the remote mme's K_PASTE) */
      Buf b;
      buf_init(&b);
      paste_take(k, &b);
      panel_paste(b.s ? b.s : "", b.len);
      buf_free(&b);
      continue;
    }
    panel_key(k);
  }
  ports_stop_all();
  panel_kill_all();
  term_close();
  return 0;
}
