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
**
** WSL and Dev Containers are the same window with another way in:
**
**   --remote=wsl+Ubuntu::/mnt/c/x          wsl.exe -d Ubuntu runs a script
**                                          (wsl-connect.sh) in the distro: the
**                                          Linux build copied (again when it
**                                          changed) to ~/.mme-server, run there
**   --remote=dev-container+<config>         devcontainer.json read here: docker
**   --remote=dev-container-rebuild+<config> build / run (the folder mounted),
**                                          docker cp of the build, docker exec -it
**   --remote=attached-container+<name>::/x  a running container: cp, exec -it
**
** The remote mme knows where it is by MME_REMOTE (ssh-remote+host,
** wsl+Ubuntu, dev-container+name, attached-container+name): VS Code's
** remote indicator at the left of its status bar ("WSL: Ubuntu"), and the
** commands that only mean something there. Those that need this computer
** (Reopen Folder in Windows / Locally, Rebuild Container, Close Remote
** Connection) ask the window with an OSC 7717 (eports.c) at their exit.
** Remote Tunnels need Microsoft's tunnel service: they are not here.
*/

#include "mme.h"

#include <stdarg.h>
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
static void remote_run (Buf *b, const char *host, const char *folder, const char *q) {
  const char *p;
  buf_puts(b, "export MME_REMOTE=ssh-remote+");
  for (p = host; *p; p++)	/* the indicator's "SSH: host" (nothing the shells would read) */
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("@.-_:", *p)) buf_putc(b, *p);
  buf_printf(b, "; M=$HOME/.mme-server/mme; [ -x %s$M%s ] || M=mme; exec %s$M%s %s", q, q, q, q, q);
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
  remote_run(&run, host, folder, "\\\"");
#else
  remote_run(&run, host, folder, "\"");
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
** {==================================================================
** The scripts: an argument as their shell reads it
** ===================================================================
*/

#ifdef _WIN32
#define NL	"\r\n"
#else
#define NL	"\n"
#endif


/* one word for sh, in single quotes when it needs them */
static void sq (Buf *b, const char *a) {
  const char *p;
  int plain = *a != '\0';
  for (p = a; *p && plain; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_./=:,+@", *p))) plain = 0;
  if (plain) {
    buf_puts(b, a);
    return;
  }
  buf_putc(b, '\'');
  for (p = a; *p; p++)
    if (*p == '\'') buf_puts(b, "'\\''");
    else buf_putc(b, *p);
  buf_putc(b, '\'');
}


/*
** One argument for this computer's script: sh's quotes, or on Windows a
** program's argument in a .cmd file (in double quotes when it has more than
** a plain word, \" for a quote and the backslashes before one doubled, as
** CommandLineToArgvW reads them; % doubled for cmd).
*/
static void arg (Buf *b, const char *a) {
#ifdef _WIN32
  const char *p;
  int plain = *a != '\0';
  for (p = a; *p && plain; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_./=:,+@\\", *p))) plain = 0;
  if (!plain) buf_putc(b, '"');
  for (p = a; *p; p++) {
    size_t n = 0, k;
    while (p[n] == '\\') n++;
    if (n > 0) {	/* a run of backslashes: doubled before a quote (or the closing one) */
      int twice = !plain && (p[n] == '"' || p[n] == '\0');
      for (k = 0; k < (twice ? 2 * n : n); k++) buf_putc(b, '\\');
      p += n - 1;
      continue;
    }
    if (*p == '%') buf_puts(b, "%%");
    else if (*p == '"') buf_puts(b, "\\\"");
    else buf_putc(b, *p);
  }
  if (!plain) buf_putc(b, '"');
#else
  sq(b, a);
#endif
}


/* words, each an arg(), separated by spaces; the list ends with NULL */
static void words (Buf *b, ...) {
  va_list ap;
  const char *w;
  int first = 1;
  va_start(ap, b);
  while ((w = va_arg(ap, const char *)) != NULL) {
    if (!first) buf_putc(b, ' ');
    arg(b, w);
    first = 0;
  }
  va_end(ap);
}


/* a line the script says (cmd's echo: its specials escaped with ^) */
static void say (Buf *b, const char *text) {
#ifdef _WIN32
  const char *p;
  buf_puts(b, "echo ");
  for (p = text; *p; p++) {
    if (*p == '\r' || *p == '\n') continue;
    if (*p == '%') buf_puts(b, "%%");
    else {
      if (strchr("&|<>^()", *p)) buf_putc(b, '^');
      buf_putc(b, *p);
    }
  }
#else
  buf_puts(b, "echo ");
  sq(b, text);
#endif
  buf_puts(b, NL);
}


/* the end of a step: the script goes to its failure when it failed */
static void or_fail (Buf *b) {
#ifdef _WIN32
  buf_puts(b, " || goto fail" NL);
#else
  buf_puts(b, " || fail" NL);
#endif
}


/* a script in mme-data/remote; its path */
static char *script_put (const char *name, const Buf *s) {
  char *dir = data_path("remote"), *f;
  int fd;
  os_mkdir(dir);
  f = path_join(dir, name);
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, s->s ? s->s : "", s->len);
    os_close(fd);
  }
  free(dir);
  return f;
}


/* a program's output and its errors, both; its exit code (-1: it did not start) */
static int capture (char **argv, Buf *out) {
  int fds[2], io[3];
  OsProc proc;
  long pid, n;
  char chunk[4096];
  if (os_pipe(fds) != 0) return -1;
  io[0] = -1;
  io[1] = fds[1];
  io[2] = fds[1];
  if (os_spawn(argv[0], argv, NULL, io, 3, &proc, &pid) != 0) {
    os_close(fds[0]);
    os_close(fds[1]);
    return -1;
  }
  os_close(fds[1]);
  while ((n = os_read(fds[0], chunk, sizeof(chunk))) > 0) buf_putn(out, chunk, (size_t)n);
  os_close(fds[0]);
  buf_putc(out, '\0');
  out->len--;
  return os_wait(proc);
}

/* }================================================================== */


/*
** {==================================================================
** WSL
** ===================================================================
*/

/* "C:\x\y" as WSL sees it, /mnt/c/x/y; \\wsl$\Distro\x (and \\wsl.localhost\) is /x of that distro */
static char *wsl_path (const char *win, char *distro, size_t dn) {
  Buf b;
  const char *p = win;
  buf_init(&b);
  if (distro) distro[0] = '\0';
  if ((m_strnicmp(p, "\\\\wsl$\\", 7) == 0 && (p += 7)) || (m_strnicmp(p, "\\\\wsl.localhost\\", 16) == 0 && (p += 16))) {
    const char *e = p;
    while (*e && *e != '\\' && *e != '/') e++;
    if (distro) snprintf(distro, dn, "%.*s", (int)(e - p), p);
    p = e;
    if (*p == '\0') buf_putc(&b, '/');
  }
  else if (((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':') {
    buf_printf(&b, "/mnt/%c", p[0] >= 'A' && p[0] <= 'Z' ? p[0] - 'A' + 'a' : p[0]);
    p += 2;
    if (*p == '\0') buf_putc(&b, '/');
  }
  for (; *p; p++) buf_putc(&b, *p == '\\' ? '/' : *p);
  if (b.len > 1 && b.s[b.len - 1] == '/') b.len--;
  buf_putc(&b, '\0');
  return b.s;
}


/* /mnt/c/x as Windows sees it (C:\x); NULL: it is not on a Windows drive */
static char *win_path (const char *lx) {
  char *r;
  size_t i;
  if (strncmp(lx, "/mnt/", 5) != 0 || !((lx[5] >= 'a' && lx[5] <= 'z') || (lx[5] >= 'A' && lx[5] <= 'Z')) ||
      (lx[6] != '\0' && lx[6] != '/')) return NULL;
  r = (char *)xmalloc(strlen(lx) + 4);
  sprintf(r, "%c:%s", lx[5] >= 'a' ? lx[5] - 'a' + 'A' : lx[5], lx[6] ? lx + 6 : "\\");
  for (i = 0; r[i]; i++)
    if (r[i] == '/') r[i] = '\\';
  return r;
}


/*
** The distros (wsl.exe -l -q: UTF-16, the default first); 0, or -1 with
** why (what wsl.exe said: WSL is not installed, no distro) in err
*/
static int wsl_distros (Vec *out, char *err, size_t en) {
  char *wsl = find_program("wsl"), *argv[4], *s;
  Buf o;
  size_t i, n, wide;
  int rc;
  err[0] = '\0';
  if (wsl == NULL) {
#ifdef _WIN32
    snprintf(err, en, "wsl.exe was not found. Install WSL: wsl.exe --install (https://aka.ms/wslinstall).");
#else
    snprintf(err, en, "WSL is on Windows only.");
#endif
    return -1;
  }
  argv[0] = wsl;
  argv[1] = (char *)"-l";
  argv[2] = (char *)"-q";
  argv[3] = NULL;
  buf_init(&o);
  rc = capture(argv, &o);
  free(wsl);
  n = 0;
  for (i = 1, wide = 0; i < o.len; i += 2) wide += o.s[i] == '\0';	/* UTF-16 (WSL_UTF8=1 makes it UTF-8) */
  s = (char *)xmalloc(o.len * 2 + 1);
  if (wide > 0 && wide * 4 >= o.len / 2) {
    for (i = 0; i + 1 < o.len; i += 2) {
      unsigned c = (unsigned char)o.s[i] | (unsigned)(unsigned char)o.s[i + 1] << 8;
      if (c == 0xFEFF || c == 0) continue;
      if (c < 0x80) s[n++] = (char)c;
      else n += (size_t)utf8_encode(c, s + n);
    }
  }
  else {
    memcpy(s, o.s ? o.s : "", o.len);
    n = o.len;
  }
  s[n] = '\0';
  buf_free(&o);
  if (rc != 0) {	/* its message, on one line */
    char *p, *q;
    for (p = s, q = s; *p; p++) {
      if (*p == '\r' || *p == '\n') {
        if (q > s && q[-1] != ' ') *q++ = ' ';
      }
      else *q++ = *p;
    }
    while (q > s && q[-1] == ' ') q--;
    *q = '\0';
    snprintf(err, en, "%s", *s ? s : rc < 0 ? "wsl.exe could not be started." : "wsl.exe -l failed.");
    free(s);
    return -1;
  }
  for (i = 0; s[i];) {
    size_t e = i;
    while (s[e] && s[e] != '\n' && s[e] != '\r') e++;
    if (e > i) vec_push(out, xstrndup(s + i, e - i));
    i = s[e] ? e + 1 : e;
  }
  free(s);
  if (out->n == 0) {
    snprintf(err, en, "WSL has no distribution installed (wsl.exe --install -d Ubuntu).");
    return -1;
  }
  return 0;
}


#ifdef _WIN32
/* the window's scripts: wsl-connect.cmd here, wsl-connect.sh in the distro (the folder and MME_REMOTE its arguments) */
static char *wsl_script (const char *distro, const char *folder) {
  static const char *const arch[][2] = {
    {"x86_64", "mme-x86_64-linux"}, {"aarch64", "mme-aarch64-linux"}, {"arm64", "mme-aarch64-linux"}
  };
  char *dir = data_path("remote"), *in, *f, rem[300];
  Buf s;
  size_t i;
  buf_init(&s);
  buf_puts(&s, "#!/bin/sh\n# mme: the WSL side of a WSL window (mme writes it at each connect)\n"
               "F=$1\ncase \"$F\" in \"~\") F=$HOME;; \"~/\"*) F=\"$HOME/${F#??}\";; esac\n"
               "U=$(uname -m)\nW=\ncase \"$U\" in\n");
  for (i = 0; i < sizeof(arch) / sizeof(arch[0]); i++) {
    char *b = server_build(arch[i][1]);
    if (b) {
      buf_printf(&s, "  %s) W=", arch[i][0]);
      sq(&s, b);
      buf_puts(&s, ";;\n");
    }
    free(b);
  }
  buf_puts(&s, "esac\nM=\"$HOME/.mme-server/mme\"\n"
               "if [ -n \"$W\" ]; then	# the build of this mme, copied again when it changed\n"
               "  B=$(wslpath -u \"$W\" 2>/dev/null)\n"
               "  if [ -n \"$B\" ] && ! cmp -s \"$B\" \"$M\" 2>/dev/null; then\n"
               "    echo \"Installing mme in WSL ($U)...\"\n"
               "    mkdir -p \"$HOME/.mme-server\" && cp \"$B\" \"$M.new\" && chmod +x \"$M.new\" && mv -f \"$M.new\" \"$M\" ||"
               " { echo 'Could not install mme in ~/.mme-server.'; exit 1; }\n"
               "  fi\nfi\n"
               "[ -x \"$M\" ] || M=$(command -v mme) ||"
               " { echo \"mme has no build here for $U (build cross makes them): install mme in this distro.\"; exit 1; }\n"
               "export MME_REMOTE=\"$2\" COLORTERM=truecolor\n"
               "cd \"$F\" 2>/dev/null\nexec \"$M\" \"$F\"\n");
  in = script_put("wsl-connect.sh", &s);
  free(in);
  s.len = 0;
  snprintf(rem, sizeof(rem), "wsl+%s", distro);
  buf_puts(&s, "@echo off" NL "cls" NL);
  buf_printf(&s, "echo Connecting to WSL: %s..." NL, distro);
  words(&s, "wsl.exe", "-d", distro, "--cd", dir, "--", "sh", "wsl-connect.sh", folder, rem, (char *)NULL);
  buf_puts(&s, NL "if errorlevel 1 goto fail" NL "exit /b 0" NL ":fail" NL "echo." NL);
  buf_printf(&s, "echo Could not connect to WSL: %s." NL "pause" NL "exit /b 1" NL, distro);
  f = script_put("wsl-connect.cmd", &s);
  buf_free(&s);
  free(dir);
  return f;
}
#endif


/* a window whose mme runs in the distro, on folder (Linux's) */
static int wsl_window (const char *distro, const char *folder) {
  char *a = (char *)xmalloc(strlen(distro) + strlen(folder) + 24);
  int r;
  sprintf(a, "--remote=wsl+%s::%s", distro, folder);
  r = term_new_window(a);
  if (r != 0) toast(1, "WSL: a window could not be opened here (run mme-sdl, or mme in mmc-term).");
  else toast(0, "WSL: connecting to %s in a new window...", distro);
  free(a);
  return r;
}


/* the distro: the default one (ask 0), or the one picked; NULL: none (said why) */
static char *wsl_pick (int ask) {
  Vec d;
  char err[400], *r = NULL;
  vec_init(&d);
  if (wsl_distros(&d, err, sizeof(err)) != 0) toast(1, "WSL: %s", err);
  else if (!ask) r = xstrdup(d.v[0]);
  else {
    Pick p;
    size_t i;
    int k;
    pick_init(&p, "Select a WSL distro");
    p.keep_order = 1;
    for (i = 0; i < d.n; i++) pick_add(&p, d.v[i], i == 0 ? "default distro" : "", 0xEBC6);	/* terminal-linux */
    k = pick_run(&p);
    if (k >= 0) r = xstrdup(d.v[k]);
    pick_free(&p);
  }
  vec_free(&d);
  return r;
}


/* WSL: Open Folder in WSL... (win: the folder, NULL asks) and Reopen Folder in WSL */
static int wsl_folder (const char *win) {
  char distro[128], *lx, *pick = NULL, *dlg = NULL;
  int r = -1;
  if (win == NULL && (win = dlg = file_dialog("Open Folder in WSL", 1)) == NULL) return -1;
  lx = wsl_path(win, distro, sizeof(distro));
  if (lx[0] != '/') toast(1, "WSL: %s is not a folder WSL can reach (a drive's, or \\\\wsl$\\).", win);
  else if (distro[0] || (pick = wsl_pick(0)) != NULL) r = wsl_window(distro[0] ? distro : pick, lx);
  free(pick);
  free(lx);
  free(dlg);
  return r;
}

/* }================================================================== */


/*
** {==================================================================
** Dev Containers: devcontainer.json, and docker
** ===================================================================
*/

typedef struct Devc {
  char *config, *cdir, *folder;	/* devcontainer.json, its folder, the folder it is of (here) */
  char *name, *cname, *ws, *user;	/* its name, the container's (and image's), workspaceFolder, remoteUser */
  char id[12];	/* ${devcontainerId}: the folder's hash */
  Json *j;
  char err[400];	/* not usable: why */
} Devc;


static unsigned fnv (const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
#ifdef _WIN32
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');	/* C:\X is c:\x */
    if (c == '/') c = '\\';
#endif
    h = (h ^ c) * 16777619u;
  }
  return h;
}


/* ${localWorkspaceFolder} ... in a devcontainer.json string; malloc'd */
static char *subst (const Devc *d, const char *s) {
  Buf b;
  buf_init(&b);
  while (*s) {
    const char *e;
    if (s[0] == '$' && s[1] == '{' && (e = strchr(s, '}')) != NULL) {
      char v[256];
      snprintf(v, sizeof(v), "%.*s", (int)(e - s - 2), s + 2);
      if (strcmp(v, "localWorkspaceFolder") == 0) buf_puts(&b, d->folder);
      else if (strcmp(v, "localWorkspaceFolderBasename") == 0) buf_puts(&b, path_basename(d->folder));
      else if (strcmp(v, "containerWorkspaceFolder") == 0) buf_puts(&b, d->ws ? d->ws : "");
      else if (strcmp(v, "containerWorkspaceFolderBasename") == 0) {
        const char *w = d->ws ? strrchr(d->ws, '/') : NULL;
        buf_puts(&b, w ? w + 1 : "");
      }
      else if (strcmp(v, "devcontainerId") == 0) buf_puts(&b, d->id);
      else if (strncmp(v, "localEnv:", 9) == 0 || strncmp(v, "env:", 4) == 0) {	/* ${localEnv:NAME:default} */
        char *name = v + (v[0] == 'l' ? 9 : 4), *def = strchr(name, ':'), *val;
        if (def) *def++ = '\0';
        val = os_getenv(name);
        buf_puts(&b, val ? val : def ? def : "");
        free(val);
      }
      s = e + 1;	/* containerEnv: and the rest: nothing here */
      continue;
    }
    buf_putc(&b, *s++);
  }
  buf_putc(&b, '\0');
  return b.s;
}


static const char *jstr (const Devc *d, const char *key) {
  return json_str(json_get(d->j, key), NULL);
}


/* the devcontainer.json of a folder: .devcontainer/devcontainer.json, .devcontainer.json, .devcontainer/<x>/ (picked) */
static char *devc_find (const char *folder) {
  char *dc = path_join(folder, ".devcontainer"), *f = path_join(dc, "devcontainer.json"), *r = NULL;
  OsStat st;
  Vec sub, found;
  size_t i;
  if (os_stat(f, &st) == 0 && st.exists && !st.is_dir) r = f;
  else {
    free(f);
    f = path_join(folder, ".devcontainer.json");
    if (os_stat(f, &st) == 0 && st.exists && !st.is_dir) r = f;
    else free(f);
  }
  if (r) {
    free(dc);
    return r;
  }
  vec_init(&sub);
  vec_init(&found);
  os_listdir(dc, &sub);
  vec_sort(&sub);
  for (i = 0; i < sub.n; i++) {	/* several configurations: VS Code asks which */
    char *d = path_join(dc, sub.v[i]);
    f = path_join(d, "devcontainer.json");
    if (os_stat(f, &st) == 0 && st.exists && !st.is_dir) vec_push(&found, f);
    else free(f);
    free(d);
  }
  if (found.n == 1) r = xstrdup(found.v[0]);
  else if (found.n > 1) {
    Pick p;
    int k;
    pick_init(&p, "Select a dev container configuration");
    for (i = 0; i < found.n; i++) {	/* .devcontainer/<x>: x */
      char *up = path_dirname(found.v[i]);
      pick_add(&p, path_basename(up), found.v[i], 0);
      free(up);
    }
    k = pick_run(&p);
    if (k >= 0) r = xstrdup(found.v[k]);
    pick_free(&p);
  }
  vec_free(&found);
  vec_free(&sub);
  free(dc);
  return r;
}


/* devcontainer.json read (config: its path); d->err when it cannot be used */
static void devc_load (Devc *d, const char *config) {
  char *text, *base, *c;
  const char *s;
  size_t n = 0, i;
  memset(d, 0, sizeof(*d));
  d->config = xstrdup(config);
  d->cdir = path_dirname(config);
  if (strcmp(path_basename(d->cdir), ".devcontainer") == 0) d->folder = path_dirname(d->cdir);
  else if (strcmp(path_basename(config), ".devcontainer.json") == 0) d->folder = xstrdup(d->cdir);
  else {	/* .devcontainer/<x>/devcontainer.json */
    char *up = path_dirname(d->cdir);
    d->folder = path_dirname(up);
    free(up);
  }
  snprintf(d->id, sizeof(d->id), "%08x", fnv(d->folder));
  base = xstrdup(path_basename(d->folder));
  for (c = base; *c; c++) {	/* docker's names: lower case letters, digits, . _ - */
    if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    else if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-')) *c = '-';
  }
  d->cname = (char *)xmalloc(strlen(base) + 24);
  sprintf(d->cname, "mme-%s-%s", *base ? base : "folder", d->id);
  free(base);
  text = read_file(config, &n);
  if (text == NULL || (d->j = json_parse(text, n)) == NULL || d->j->type != J_OBJ) {
    snprintf(d->err, sizeof(d->err), "%s is not readable JSON.", config);
    free(text);
    d->name = xstrdup(path_basename(d->folder));
    return;
  }
  free(text);
  d->name = xstrdup((s = jstr(d, "name")) != NULL ? s : path_basename(d->folder));
  for (i = 0; d->name[i]; i++)	/* it goes into the scripts' lines and MME_REMOTE */
    if (strchr("\"%'`$\\\r\n", d->name[i])) d->name[i] = ' ';
  if ((s = jstr(d, "workspaceFolder")) != NULL) d->ws = subst(d, s);
  else {
    d->ws = (char *)xmalloc(strlen(path_basename(d->folder)) + 16);
    sprintf(d->ws, "/workspaces/%s", path_basename(d->folder));
  }
  s = jstr(d, "remoteUser");
  if (s == NULL) s = jstr(d, "containerUser");
  if (s) d->user = subst(d, s);
  if (json_get(d->j, "dockerComposeFile"))
    snprintf(d->err, sizeof(d->err), "Docker Compose configurations (dockerComposeFile) are not supported by mme: use image or build.dockerfile.");
  else if (!jstr(d, "image") && !jstr(d, "build.dockerfile") && !jstr(d, "dockerFile"))
    snprintf(d->err, sizeof(d->err), "%s has neither \"image\" nor \"build\": { \"dockerfile\" }.", config);
}


static void devc_free (Devc *d) {
  free(d->config);
  free(d->cdir);
  free(d->folder);
  free(d->name);
  free(d->cname);
  free(d->ws);
  free(d->user);
  json_free(d->j);
}


/* a path of devcontainer.json (relative to its folder) as a full one here */
static char *devc_rel (const Devc *d, const char *p) {
  char *s = subst(d, p), *r;
  if (s[0] == '/' || s[0] == '\\' || (s[0] && s[1] == ':')) return s;
  r = path_join(d->cdir, s);
  free(s);
  return r;
}


/* the forwardPorts (numbers, "3000"; not "db:5432"), as 3000,8080 */
static void forward_ports (const Json *fp, Buf *b) {
  size_t i;
  for (i = 0; fp && fp->type == J_ARR && i < fp->n; i++) {
    const Json *e = fp->kid[i];
    int port = e->type == J_NUM ? (int)e->num : e->type == J_STR && strspn(e->str, "0123456789") == strlen(e->str) ? atoi(e->str) : 0;
    if (port > 0 && port < 65536) buf_printf(b, "%s%d", b->len ? "," : "", port);
  }
  buf_putc(b, '\0');
  b->len--;
}


/* a lifecycle command (postCreateCommand ...: a string, an array, an object of them) as an sh script; NULL: none */
static char *lifecycle (const Devc *d, const char *key) {
  const Json *c = json_get(d->j, key);
  Buf s;
  size_t i, k, n = c ? (c->type == J_OBJ ? c->n : 1) : 0;
  char name[200], *f;
  if (n == 0) return NULL;
  buf_init(&s);
  buf_printf(&s, "set -e\necho '[Dev Containers] Running the %s...'\n", key);
  for (i = 0; i < n; i++) {
    const Json *e = c->type == J_OBJ ? c->kid[i] : c;
    if (e->type == J_STR) buf_printf(&s, "%s\n", e->str);
    else if (e->type == J_ARR) {
      for (k = 0; k < e->n; k++) {
        if (k) buf_putc(&s, ' ');
        sq(&s, json_str(e->kid[k], ""));
      }
      buf_putc(&s, '\n');
    }
  }
  snprintf(name, sizeof(name), "%s-%s.sh", d->cname, key);
  f = script_put(name, &s);
  buf_free(&s);
  return f;
}


/* a lifecycle script run in the container (as its user, in its folder) */
static void run_lifecycle (Buf *s, const Devc *d, const char *key) {
  char *f = lifecycle(d, key);
  if (f == NULL) return;
  words(s, "docker", "exec", "-i", (char *)NULL);
  if (d->user) {
    buf_putc(s, ' ');
    words(s, "-u", d->user, (char *)NULL);
  }
  buf_putc(s, ' ');
  words(s, "-w", d->ws, d->cname, "/bin/sh", (char *)NULL);
  buf_puts(s, " < ");
  arg(s, f);
  or_fail(s);
  free(f);
}


/* -e NAME=value for each of an object's (containerEnv, remoteEnv) */
static void env_args (Buf *s, const Devc *d, const char *key) {
  const Json *o = json_get(d->j, key);
  size_t i;
  for (i = 0; o && o->type == J_OBJ && i < o->n; i++) {
    char *v = subst(d, json_str(o->kid[i], "")), *kv = (char *)xmalloc(strlen(o->kid[i]->key) + strlen(v) + 2);
    sprintf(kv, "%s=%s", o->kid[i]->key, v);
    buf_putc(s, ' ');
    words(s, "-e", kv, (char *)NULL);
    free(kv);
    free(v);
  }
}


/*
** The last steps, a dev container's and an attached one's: the Linux build
** for the container's machine copied into its user's ~/.mme-server, then
** run there with docker exec -it. rem: MME_REMOTE; ws: the folder, NULL the
** container's own working directory.
*/
static void install_and_run (Buf *s, const char *cname, const char *user, const char *ws, const char *rem,
                             const char *ports, const Devc *d) {
  char *x86 = server_build("mme-x86_64-linux"), *arm = server_build("mme-aarch64-linux"), e1[1200];
  say(s, "[Dev Containers] Installing mme in the container...");
#ifdef _WIN32
  buf_puts(s, "set BIN=" NL);
  if (x86) {
    snprintf(e1, sizeof(e1), "BIN=%s", x86);
    buf_puts(s, "set ");
    arg(s, e1);
    buf_puts(s, NL);
  }
  words(s, "docker", "exec", cname, "uname", "-m", (char *)NULL);
  buf_puts(s, " | findstr /b /c:aarch64 /c:arm64 >nul" NL "if not errorlevel 1 set BIN=" NL);
  if (arm) {
    snprintf(e1, sizeof(e1), "BIN=%s", arm);
    buf_puts(s, "if not errorlevel 1 set ");
    arg(s, e1);
    buf_puts(s, NL);
  }
  buf_puts(s, "if \"%BIN%\"==\"\" goto nobuild" NL "docker cp \"%BIN%\" ");
#else
  buf_puts(s, "BIN=");
  sq(s, x86 ? x86 : "");
  buf_puts(s, NL "case \"$(docker exec ");
  sq(s, cname);
  buf_puts(s, " uname -m)\" in aarch64|arm64) BIN=");
  sq(s, arm ? arm : "");
  buf_puts(s, ";; esac" NL "[ -n \"$BIN\" ] || nobuild" NL "docker cp \"$BIN\" ");
#endif
  snprintf(e1, sizeof(e1), "%s:/tmp/mme-server", cname);
  words(s, e1, (char *)NULL);
  or_fail(s);
  words(s, "docker", "exec", (char *)NULL);
  if (user) {
    buf_putc(s, ' ');
    words(s, "-u", user, (char *)NULL);
  }
  buf_putc(s, ' ');
  words(s, cname, "/bin/sh", "-c", "mkdir -p $HOME/.mme-server && cp /tmp/mme-server $HOME/.mme-server/mme.new && "
        "chmod +x $HOME/.mme-server/mme.new && mv -f $HOME/.mme-server/mme.new $HOME/.mme-server/mme", (char *)NULL);
  or_fail(s);
#ifdef _WIN32
  buf_puts(s, "cls" NL);
#else
  buf_puts(s, "printf '\\033[2J\\033[H'" NL);
#endif
  snprintf(e1, sizeof(e1), "MME_REMOTE=%s", rem);
  words(s, "docker", "exec", "-it", "-e", e1, "-e", "TERM=xterm-256color", "-e", "COLORTERM=truecolor", (char *)NULL);
  if (ports && *ports) {
    snprintf(e1, sizeof(e1), "MME_FORWARD_PORTS=%s", ports);
    buf_putc(s, ' ');
    words(s, "-e", e1, (char *)NULL);
  }
  if (d) env_args(s, d, "remoteEnv");
  if (user) {
    buf_putc(s, ' ');
    words(s, "-u", user, (char *)NULL);
  }
  if (ws && *ws) {
    buf_putc(s, ' ');
    words(s, "-w", ws, (char *)NULL);
  }
  buf_putc(s, ' ');
  words(s, cname, "/bin/sh", "-c", "exec $HOME/.mme-server/mme .", (char *)NULL);
  or_fail(s);
#ifdef _WIN32
  buf_puts(s, "exit /b 0" NL);
#else
  buf_puts(s, "exit 0" NL);
#endif
  free(x86);
  free(arm);
}


/* the head of a container's script: its failure (reopen: the folder here, opened again then) */
static void script_head (Buf *s, const char *title, const char *reopen) {
  char *exe = os_exe_path(NULL);
#ifdef _WIN32
  buf_puts(s, "@echo off" NL "cls" NL);
  say(s, title);
  buf_puts(s, "goto start" NL ":nobuild" NL
              "echo mme has no Linux build for the container's machine here (build cross makes them, into dist\\)." NL
              ":fail" NL "echo." NL "echo [Dev Containers] The container could not be started (see above)." NL);
  if (reopen && exe) {
    buf_puts(s, "echo Press any key to reopen the folder locally." NL "pause >nul" NL);
    words(s, exe, reopen, (char *)NULL);
    buf_puts(s, NL);
  }
  else buf_puts(s, "pause" NL);
  buf_puts(s, "exit /b 1" NL ":start" NL);
  buf_puts(s, "docker version >nul" NL "if errorlevel 1 echo [Dev Containers] Docker is not running (see above): start it (Docker Desktop), then try again. & goto fail" NL);
#else
  buf_puts(s, "#!/bin/sh\nprintf '\\033[2J\\033[H'\n");
  say(s, title);
  buf_puts(s, "nobuild () { echo \"mme has no Linux build for the container's machine here (make cross makes them, into dist/).\"; fail; }\n"
              "fail () {\n  echo\n  echo '[Dev Containers] The container could not be started (see above).'\n");
  if (reopen && exe) {
    buf_puts(s, "  printf 'Press Enter to reopen the folder locally.'\n  read x\n  exec ");
    words(s, exe, reopen, (char *)NULL);
    buf_puts(s, "\n}\n");
  }
  else buf_puts(s, "  read x\n  exit 1\n}\n");
  buf_puts(s, "docker version >/dev/null || { echo '[Dev Containers] Docker is not running (see above): start it, then try again.'; fail; }\n");
#endif
  free(exe);
}


/* the dev container's window script: create (build, run) or start the container, then install_and_run */
static char *devc_script (const char *config, int rebuild, char *title, size_t tn, char *cname, size_t cn) {
  Devc d;
  Buf s, ports, rem;
  const Json *a;
  size_t i;
  char *f, t[600];
  devc_load(&d, config);
  snprintf(title, tn, "%s [Dev Container: %s]", d.ws ? d.ws : d.folder, d.name);
  snprintf(cname, cn, "%s", d.cname);
  buf_init(&s);
  snprintf(t, sizeof(t), "[Dev Containers] %s: %s", rebuild ? "Rebuilding" : "Starting", d.name);
  script_head(&s, t, d.folder);
  if (d.err[0]) {
    say(&s, d.err);
#ifdef _WIN32
    buf_puts(&s, "goto fail" NL);
    f = script_put("dev-container.cmd", &s);
#else
    buf_puts(&s, "fail" NL);
    f = script_put("dev-container.sh", &s);
#endif
    buf_free(&s);
    devc_free(&d);
    return f;
  }
  if (rebuild) {
    say(&s, "[Dev Containers] Removing the container...");
    words(&s, "docker", "rm", "-f", d.cname, (char *)NULL);
#ifdef _WIN32
    buf_puts(&s, " >nul 2>&1" NL);
#else
    buf_puts(&s, " >/dev/null 2>&1" NL);
#endif
  }
  buf_init(&ports);
  forward_ports(json_get(d.j, "forwardPorts"), &ports);
  /* there: running, stopped (started, postStartCommand), or not yet (created) */
#ifdef _WIN32
  words(&s, "docker", "container", "inspect", d.cname, (char *)NULL);
  buf_puts(&s, " >nul 2>&1" NL "if errorlevel 1 goto create" NL);
  words(&s, "docker", "container", "inspect", "-f", "{{.State.Running}}", d.cname, (char *)NULL);
  buf_puts(&s, " | findstr true >nul" NL "if not errorlevel 1 goto install" NL);
  say(&s, "[Dev Containers] Starting the container...");
  words(&s, "docker", "start", d.cname, (char *)NULL);
  buf_puts(&s, " >nul");
  or_fail(&s);
  run_lifecycle(&s, &d, "postStartCommand");
  buf_puts(&s, "goto install" NL ":create" NL);
#else
  buf_puts(&s, "if docker container inspect ");
  sq(&s, d.cname);
  buf_puts(&s, " >/dev/null 2>&1; then\n  if [ \"$(docker container inspect -f '{{.State.Running}}' ");
  sq(&s, d.cname);
  buf_puts(&s, ")\" != true ]; then\n    echo '[Dev Containers] Starting the container...'\n    ");
  words(&s, "docker", "start", d.cname, (char *)NULL);
  buf_puts(&s, " >/dev/null");
  or_fail(&s);
  run_lifecycle(&s, &d, "postStartCommand");
  buf_puts(&s, "  fi\nelse\n");
#endif
  {	/* the image: its own (docker build), or the one named */
    const char *img = jstr(&d, "image");
    char *image = img ? subst(&d, img) : xstrdup(d.cname);
    const char *df = jstr(&d, "build.dockerfile");
    if (df == NULL) df = jstr(&d, "dockerFile");
    if (img == NULL && df) {
      const char *ctx = jstr(&d, "build.context");
      char *dfp, *cx;
      const Json *ba = json_get(d.j, "build.args");
      if (ctx == NULL) ctx = jstr(&d, "context");
      dfp = devc_rel(&d, df);
      cx = devc_rel(&d, ctx ? ctx : ".");
      say(&s, "[Dev Containers] Building the image...");
      words(&s, "docker", "build", "-f", dfp, "-t", image, (char *)NULL);
      for (i = 0; ba && ba->type == J_OBJ && i < ba->n; i++) {
        char *v = subst(&d, json_str(ba->kid[i], "")), *kv = (char *)xmalloc(strlen(ba->kid[i]->key) + strlen(v) + 2);
        sprintf(kv, "%s=%s", ba->kid[i]->key, v);
        buf_putc(&s, ' ');
        words(&s, "--build-arg", kv, (char *)NULL);
        free(kv);
        free(v);
      }
      if (jstr(&d, "build.target")) {
        buf_putc(&s, ' ');
        words(&s, "--target", jstr(&d, "build.target"), (char *)NULL);
      }
      buf_putc(&s, ' ');
      words(&s, cx, (char *)NULL);
      or_fail(&s);
      free(dfp);
      free(cx);
    }
    say(&s, "[Dev Containers] Creating the container...");
    {
      char *mount, lab1[1200], lab2[1200];
      const char *wm = jstr(&d, "workspaceMount");
      if (wm) mount = subst(&d, wm);
      else {
        mount = (char *)xmalloc(strlen(d.folder) + strlen(path_basename(d.folder)) + 64);
        sprintf(mount, "type=bind,source=%s,target=/workspaces/%s", d.folder, path_basename(d.folder));
      }
      snprintf(lab1, sizeof(lab1), "devcontainer.local_folder=%s", d.folder);
      snprintf(lab2, sizeof(lab2), "devcontainer.config_file=%s", d.config);
      words(&s, "docker", "run", "-d", "--name", d.cname, "--label", lab1, "--label", lab2, (char *)NULL);
      if (*mount) {
        buf_putc(&s, ' ');
        words(&s, "--mount", mount, (char *)NULL);
      }
      free(mount);
    }
    a = json_get(d.j, "mounts");
    for (i = 0; a && a->type == J_ARR && i < a->n; i++) {	/* "type=bind,source=..,target=..", or { source, target, type } */
      const Json *m = a->kid[i];
      char *v = NULL;
      if (m->type == J_STR) v = subst(&d, m->str);
      else if (m->type == J_OBJ && json_str(json_get(m, "target"), NULL)) {
        Buf mb;
        char *src = subst(&d, json_str(json_get(m, "source"), ""));
        buf_init(&mb);
        buf_printf(&mb, "type=%s,%s%s%starget=%s", json_str(json_get(m, "type"), "bind"), *src ? "source=" : "", src,
                   *src ? "," : "", json_str(json_get(m, "target"), ""));
        buf_putc(&mb, '\0');
        v = mb.s;
        free(src);
      }
      if (v && *v) {
        buf_putc(&s, ' ');
        words(&s, "--mount", v, (char *)NULL);
      }
      free(v);
    }
    env_args(&s, &d, "containerEnv");
    a = json_get(d.j, "appPort");	/* published: a number, "3000:3000", or a list of them */
    for (i = 0; a && i < (a->type == J_ARR ? a->n : 1); i++) {
      const Json *p = a->type == J_ARR ? a->kid[i] : a;
      char pv[64];
      if (p->type == J_NUM) snprintf(pv, sizeof(pv), "127.0.0.1:%d:%d", (int)p->num, (int)p->num);
      else if (p->type == J_STR) snprintf(pv, sizeof(pv), "%s", p->str);
      else continue;
      buf_putc(&s, ' ');
      words(&s, "-p", pv, (char *)NULL);
    }
    if (jstr(&d, "containerUser")) {
      buf_putc(&s, ' ');
      words(&s, "-u", jstr(&d, "containerUser"), (char *)NULL);
    }
    a = json_get(d.j, "runArgs");
    for (i = 0; a && a->type == J_ARR && i < a->n; i++) {
      char *v = subst(&d, json_str(a->kid[i], ""));
      buf_putc(&s, ' ');
      words(&s, v, (char *)NULL);
      free(v);
    }
    if (json_bool(json_get(d.j, "overrideCommand"), 1)) {	/* VS Code's: the container stays up whatever its image runs */
      buf_putc(&s, ' ');
      words(&s, "--entrypoint", "/bin/sh", image, "-c",
            "echo Container started; trap \"exit 0\" 15; while sleep 1 & wait $!; do :; done", (char *)NULL);
    }
    else {
      buf_putc(&s, ' ');
      words(&s, image, (char *)NULL);
    }
    or_fail(&s);
    free(image);
  }
  run_lifecycle(&s, &d, "onCreateCommand");
  run_lifecycle(&s, &d, "postCreateCommand");
  run_lifecycle(&s, &d, "postStartCommand");
#ifdef _WIN32
  buf_puts(&s, ":install" NL);
#else
  buf_puts(&s, "fi\n");
#endif
  buf_init(&rem);
  buf_printf(&rem, "dev-container+%s", d.name);
  buf_putc(&rem, '\0');
  install_and_run(&s, d.cname, d.user, d.ws, rem.s, ports.s, &d);
#ifdef _WIN32
  f = script_put("dev-container.cmd", &s);
#else
  f = script_put("dev-container.sh", &s);
#endif
  buf_free(&rem);
  buf_free(&ports);
  buf_free(&s);
  devc_free(&d);
  return f;
}


/* an attached container's window script: started when it is not, then install_and_run */
static char *attach_script (const char *cname, const char *folder) {
  Buf s;
  char *f, t[300], rem[300];
  buf_init(&s);
  snprintf(t, sizeof(t), "[Dev Containers] Attaching to %s", cname);
  script_head(&s, t, NULL);
  words(&s, "docker", "start", cname, (char *)NULL);
#ifdef _WIN32
  buf_puts(&s, " >nul");
#else
  buf_puts(&s, " >/dev/null");
#endif
  or_fail(&s);
  snprintf(rem, sizeof(rem), "attached-container+%s", cname);
  install_and_run(&s, cname, NULL, folder, rem, NULL, NULL);
#ifdef _WIN32
  f = script_put("attach-container.cmd", &s);
#else
  f = script_put("attach-container.sh", &s);
#endif
  buf_free(&s);
  return f;
}


static int have_docker (void) {
  char *d = find_program("docker");
  int ok = d != NULL;
  free(d);
  if (!ok) toast(1, "Dev Containers: docker was not found. Install Docker (Docker Desktop, or the docker CLI and engine) and make sure docker is in PATH.");
  return ok;
}


/* a window whose mme runs in the dev container of config (rebuilt first) */
static int devc_window (const char *config, int rebuild) {
  char *a = (char *)xmalloc(strlen(config) + 40);
  int r;
  sprintf(a, "--remote=dev-container%s+%s", rebuild ? "-rebuild" : "", config);
  r = term_new_window(a);
  if (r != 0) toast(1, "Dev Containers: a window could not be opened here (run mme-sdl, or mme in mmc-term).");
  else toast(0, "Dev Containers: %s the container in a new window...", rebuild ? "rebuilding" : "starting");
  free(a);
  return r;
}


static void add_config_done (void *ud, int choice) {	/* Add Dev Container Configuration Files: a plain one */
  char *folder = (char *)ud;
  if (choice == 0) {
    char *dc = path_join(folder, ".devcontainer"), *f = path_join(dc, "devcontainer.json");
    int fd;
    static const char text[] =
      "// For format details, see https://aka.ms/devcontainer.json.\n"
      "{\n"
      "\t\"name\": \"Ubuntu\",\n"
      "\t\"image\": \"mcr.microsoft.com/devcontainers/base:ubuntu\"\n"
      "\n"
      "\t// Use 'forwardPorts' to make a list of ports inside the container available locally.\n"
      "\t// \"forwardPorts\": [],\n"
      "\n"
      "\t// Use 'postCreateCommand' to run commands after the container is created.\n"
      "\t// \"postCreateCommand\": \"uname -a\",\n"
      "\n"
      "\t// Uncomment to connect as root instead. More info: https://aka.ms/dev-containers-non-root.\n"
      "\t// \"remoteUser\": \"root\"\n"
      "}\n";
    mkdir_p(dc);
    if ((fd = os_open(f, OS_WRITE)) >= 0) {
      os_write(fd, text, sizeof(text) - 1);
      os_close(fd);
      open_path(f);
    }
    free(f);
    free(dc);
  }
  free(folder);
}


/* the folder's devcontainer.json; none: said so, with Add Dev Container Configuration Files */
static char *devc_config (const char *folder) {
  char *c = devc_find(folder);
  if (c == NULL) {
    static const char *const act[] = {"Add Dev Container Configuration Files..."};
    char msg[1200];
    snprintf(msg, sizeof(msg), "No dev container configuration (.devcontainer/devcontainer.json) was found in %s.", folder);
    toast_ask(1, "Dev Containers", msg, act, 1, add_config_done, xstrdup(folder));
  }
  return c;
}


/* docker ps (args) as lines; 0, or -1 with the first line docker said in err */
static int docker_lines (const char *const *args, Vec *out, char *err, size_t en) {
  char *docker = find_program("docker"), *argv[8];
  Buf o;
  size_t i;
  int rc, n = 0;
  if (docker == NULL) return -1;
  argv[n++] = docker;
  while (*args && n < 7) argv[n++] = (char *)*args++;
  argv[n] = NULL;
  buf_init(&o);
  rc = capture(argv, &o);
  free(docker);
  for (i = 0; o.s && o.s[i];) {
    size_t e = i;
    while (o.s[e] && o.s[e] != '\n' && o.s[e] != '\r') e++;
    if (rc != 0) {
      snprintf(err, en, "%.*s", (int)(e - i), o.s + i);
      break;
    }
    if (e > i) vec_push(out, xstrndup(o.s + i, e - i));
    i = o.s[e] ? e + 1 : e;
  }
  buf_free(&o);
  return rc == 0 ? 0 : -1;
}


/* Dev Containers: Attach to Running Container...: docker ps, one picked, a folder in it */
static void devc_attach (void) {
  static const char *const args[] = {"ps", "--format", "{{.Names}}\t{{.Image}}\t{{.Status}}", NULL};
  char err[400] = "", *folder;
  Vec lines, names;
  Pick p;
  size_t i;
  int k;
  vec_init(&lines);
  if (docker_lines(args, &lines, err, sizeof(err)) != 0) {
    toast(1, "Dev Containers: docker ps failed%s%s", err[0] ? ": " : " (is Docker running?).", err);
    vec_free(&lines);
    return;
  }
  vec_init(&names);
  pick_init(&p, "Select the container to attach to");
  for (i = 0; i < lines.n; i++) {	/* name \t image \t status */
    char *t1 = strchr(lines.v[i], '\t'), *t2, d[400];
    if (t1 == NULL) continue;
    *t1++ = '\0';
    if ((t2 = strchr(t1, '\t')) != NULL) *t2++ = '\0';
    snprintf(d, sizeof(d), "%s  %s", t1, t2 ? t2 : "");
    vec_push(&names, xstrdup(lines.v[i]));
    pick_add(&p, lines.v[i], d, 0xEB50);	/* server */
  }
  vec_free(&lines);
  if (names.n == 0) {
    pick_free(&p);
    vec_free(&names);
    toast(1, "Dev Containers: no container is running (docker ps lists none).");
    return;
  }
  k = pick_run(&p);
  pick_free(&p);
  if (k >= 0 && (folder = ask_text("Dev Containers: the folder to open in the container (empty: its working directory)", "")) != NULL) {
    char *a = (char *)xmalloc(strlen(names.v[k]) + strlen(folder) + 40);
    sprintf(a, "--remote=attached-container+%s::%s", names.v[k], folder);
    if (term_new_window(a) != 0) toast(1, "Dev Containers: a window could not be opened here (run mme-sdl, or mme in mmc-term).");
    else toast(0, "Dev Containers: attaching to %s in a new window...", names.v[k]);
    free(a);
    free(folder);
  }
  vec_free(&names);
}

/* }================================================================== */


/*
** {==================================================================
** The remote indicator, its menu, the commands
** ===================================================================
*/

/* MME_REMOTE: "wsl+Ubuntu" ... in a remote mme, "" here */
static const char *where (void) {
  static char *w;
  if (w == NULL) {
    w = os_getenv("MME_REMOTE");
    if (w == NULL) w = xstrdup("");
  }
  return w;
}


static int is_at (const char *kind) {
  size_t n = strlen(kind);
  return strncmp(where(), kind, n) == 0 && where()[n] == '+';
}


/* "WSL: Ubuntu", as VS Code's indicator says it */
static void where_label (char *out, size_t n) {
  const char *plus = strchr(where(), '+');
  const char *name = plus ? plus + 1 : "";
  if (is_at("ssh-remote")) snprintf(out, n, "SSH: %s", name);
  else if (is_at("wsl")) snprintf(out, n, "WSL: %s", name);
  else if (is_at("dev-container")) snprintf(out, n, "Dev Container: %s", name);
  else if (is_at("attached-container")) snprintf(out, n, "Container: %s", name);
  else snprintf(out, n, "Remote");
}


void remote_status (void) {
  char t[200], tip[240], lab[160];
  int k = utf8_encode(0xEB3A, t);	/* codicon remote: >< */
  t[k] = '\0';
  if (*where()) {
    where_label(lab, sizeof(lab));
    snprintf(t + k, sizeof(t) - (size_t)k, " %s", lab);
    snprintf(tip, sizeof(tip), "Editing on %s", lab);
  }
  else snprintf(tip, sizeof(tip), "Open a Remote Window");
  status_add("status.host", "Remote Host", 0, 110, t, tip, CMD_REMOTE_MENU);
}


int remote_hidden (int cmd) {
  int remote = *where() != '\0';
  switch (cmd) {
    case CMD_REMOTE_CLOSE: return !remote;
    case CMD_WSL_WINDOWS: return !is_at("wsl");
    case CMD_DC_LOCAL: return !is_at("dev-container") && !is_at("attached-container");
    case CMD_DC_REBUILD: return remote && !is_at("dev-container");
    case CMD_WSL_CONNECT: case CMD_WSL_DISTRO: case CMD_WSL_OPEN: case CMD_WSL_REOPEN:
#ifdef _WIN32
      return remote;
#else
      return 1;	/* WSL is Windows' */
#endif
    case CMD_DC_REOPEN: case CMD_DC_OPEN: case CMD_DC_ATTACH: case CMD_DC_REBUILD_REOPEN: return remote;	/* windows here only */
    default: return 0;
  }
}


/* the window is asked at the remote mme's exit (Close Window not cancelled): reopen-local, close-remote, rebuild */
static const char *g_exit_ask;

static void exit_ask (void) {
  if (g_exit_ask) {
    char s[64];
    snprintf(s, sizeof(s), "\033]7717;%s\a", g_exit_ask);
    term_write(s, strlen(s));
  }
}


static void leave (const char *ask) {
  static int once;
  if (!once) {
    atexit(exit_ask);
    once = 1;
  }
  g_exit_ask = ask;
  mme_command(CMD_CLOSE_WINDOW);
  if (!mme_quitting()) g_exit_ask = NULL;	/* cancelled (an unsaved file) */
}


/* Remote: Show Remote Menu (the indicator's click): VS Code's "Open a Remote Window" */
static void remote_menu (void) {
  static const struct {
    int cmd;
    const char *label, *group;
  } item[] = {
    {CMD_WSL_WINDOWS, "Reopen Folder in Windows", "WSL"}, {CMD_DC_LOCAL, "Reopen Folder Locally", "Dev Containers"},
    {CMD_DC_REBUILD, "Rebuild Container", "Dev Containers"}, {CMD_REMOTE_CLOSE, "Close Remote Connection", "Remote"},
    {CMD_REMOTE_CONNECT, "Connect to Host...", "Remote-SSH"},
    {CMD_WSL_CONNECT, "Connect to WSL", "WSL"}, {CMD_WSL_DISTRO, "Connect to WSL using Distro...", "WSL"},
    {CMD_WSL_OPEN, "Open Folder in WSL...", "WSL"}, {CMD_WSL_REOPEN, "Reopen Folder in WSL", "WSL"},
    {CMD_DC_REOPEN, "Reopen in Container", "Dev Containers"}, {CMD_DC_OPEN, "Open Folder in Container...", "Dev Containers"},
    {CMD_DC_ATTACH, "Attach to Running Container...", "Dev Containers"},
    {CMD_DC_REBUILD_REOPEN, "Rebuild and Reopen in Container", "Dev Containers"}
  };
  int cmds[16], n = 0, r;
  size_t i;
  Pick p;
  pick_init(&p, "Select an option to open a Remote Window");
  p.keep_order = 1;
  for (i = 0; i < sizeof(item) / sizeof(item[0]); i++) {
    if (remote_hidden(item[i].cmd) || (item[i].cmd == CMD_REMOTE_CONNECT && *where())) continue;
    if (item[i].cmd == CMD_DC_REBUILD && !*where()) continue;	/* here it is Rebuild and Reopen */
    cmds[n++] = item[i].cmd;
    pick_add(&p, item[i].label, item[i].group, 0);
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) mme_command(cmds[r]);
}


static void open_remote (const char *what, const char *arg) {
  char *a = (char *)xmalloc(strlen(arg) + 16);
  sprintf(a, "--remote=%s", arg);
  if (term_new_window(a) != 0) toast(1, "%s: a window could not be opened here (run mme-sdl, or mme in mmc-term).", what);
  free(a);
}


/*
** View: Show Remote Explorer: VS Code's Remote Explorer as a list, the
** targets of each: the SSH hosts (~/.ssh/config), the WSL distros, the
** containers (docker ps -a; a dev container's reopens its folder).
*/
static void remote_explorer (void) {
  static const char *const args[] = {"ps", "-a", "--format", "{{.Names}}\t{{.State}}\t{{.Label \"devcontainer.config_file\"}}", NULL};
  Vec hosts, distros, cont, what;
  Pick p;
  size_t i;
  int r;
  char err[400], *docker = find_program("docker");
  vec_init(&hosts);
  vec_init(&distros);
  vec_init(&cont);
  vec_init(&what);
  config_hosts(&hosts);
#ifdef _WIN32
  wsl_distros(&distros, err, sizeof(err));
#endif
  if (docker) docker_lines(args, &cont, err, sizeof(err));
  pick_init(&p, "Remote Explorer");
  p.keep_order = 1;
  for (i = 0; i < hosts.n; i++) {
    pick_add(&p, hosts.v[i], "SSH", 0xEB3A);
    vec_push(&what, xstrdup(""));
  }
  for (i = 0; i < distros.n; i++) {
    pick_add(&p, distros.v[i], i == 0 ? "WSL Targets  (default distro)" : "WSL Targets", 0xEBC6);
    vec_push(&what, xstrdup(""));
  }
  for (i = 0; i < cont.n; i++) {	/* name \t state \t devcontainer.json */
    char *t1 = strchr(cont.v[i], '\t'), *t2 = NULL, d[300];
    if (t1) {
      *t1++ = '\0';
      if ((t2 = strchr(t1, '\t')) != NULL) *t2++ = '\0';
    }
    snprintf(d, sizeof(d), "Dev Containers  %s%s", t1 ? t1 : "", t2 && *t2 ? "  (dev container)" : "");
    pick_add(&p, cont.v[i], d, 0xEB50);
    vec_push(&what, xstrdup(t2 ? t2 : ""));	/* its devcontainer.json, or "": attach */
  }
  p.hint = docker ? "No remote targets: ~/.ssh/config has no hosts, WSL no distros, docker no containers."
                  : "No remote targets: ~/.ssh/config has no hosts and WSL no distros (docker was not found).";
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0 && (size_t)r < hosts.n) {
    char *folder = ask_text("Remote-SSH: the folder to open on the host", "~");
    if (folder) {
      char *a = (char *)xmalloc(strlen(hosts.v[r]) + strlen(folder) + 8);
      sprintf(a, "%s::%s", hosts.v[r], *folder ? folder : "~");
      open_remote("Remote-SSH", a);
      free(a);
      free(folder);
    }
  }
  else if (r >= 0 && (size_t)r < hosts.n + distros.n) wsl_window(distros.v[r - (int)hosts.n], "~");
  else if (r >= 0) {
    const char *name = cont.v[r - (int)hosts.n - (int)distros.n], *config = what.v[r];
    if (*config) devc_window(config, 0);
    else {
      char *a = (char *)xmalloc(strlen(name) + 32);
      sprintf(a, "attached-container+%s::", name);
      open_remote("Dev Containers", a);
      free(a);
    }
  }
  free(docker);
  vec_free(&hosts);
  vec_free(&distros);
  vec_free(&cont);
  vec_free(&what);
}


void remote_command (int cmd) {
  char *c, *d;
  if (remote_hidden(cmd) && cmd != CMD_REMOTE_CLOSE && cmd != CMD_WSL_WINDOWS && cmd != CMD_DC_LOCAL) {
#ifndef _WIN32
    if (cmd >= CMD_WSL_CONNECT && cmd <= CMD_WSL_REOPEN) {
      toast(1, "WSL: WSL is on Windows only.");
      return;
    }
#endif
    toast(1, "%s: it opens a window on this computer; run it in a local window.", cmd_name(cmd));
    return;
  }
  switch (cmd) {
    case CMD_REMOTE_MENU: remote_menu(); break;
    case CMD_REMOTE_EXPLORER: remote_explorer(); break;
    case CMD_REMOTE_CLOSE:
      if (*where()) leave("close-remote");
      else toast(0, "This window is not connected to a remote.");
      break;
    case CMD_WSL_WINDOWS:
      if (is_at("wsl")) leave("reopen-local");
      else toast(1, "WSL: Reopen Folder in Windows is for a WSL window.");
      break;
    case CMD_DC_LOCAL:
      if (is_at("dev-container") || is_at("attached-container")) leave("reopen-local");
      else toast(1, "Dev Containers: Reopen Folder Locally is for a container's window.");
      break;
    case CMD_WSL_CONNECT: case CMD_WSL_DISTRO:
      if ((d = wsl_pick(cmd == CMD_WSL_DISTRO)) != NULL) wsl_window(d, "~");
      free(d);
      break;
    case CMD_WSL_OPEN: wsl_folder(NULL); break;
    case CMD_WSL_REOPEN:
      if (wsl_folder(side_root()) == 0) mme_command(CMD_CLOSE_WINDOW);	/* the window is WSL's now, as VS Code's */
      break;
    case CMD_DC_REBUILD:
      if (is_at("dev-container")) {
        leave("rebuild");
        break;
      }
      /* fall through: here it rebuilds and reopens */
    case CMD_DC_REOPEN: case CMD_DC_REBUILD_REOPEN:
      if (have_docker() && (c = devc_config(side_root())) != NULL) {
        if (devc_window(c, cmd != CMD_DC_REOPEN) == 0) mme_command(CMD_CLOSE_WINDOW);
        free(c);
      }
      break;
    case CMD_DC_OPEN:
      if (have_docker() && (d = file_dialog("Open Folder in Container", 1)) != NULL) {
        if ((c = devc_config(d)) != NULL) devc_window(c, 0);
        free(c);
        free(d);
      }
      break;
    case CMD_DC_ATTACH:
      if (have_docker()) devc_attach();
      break;
  }
}

/* }================================================================== */


/*
** {==================================================================
** The remote window
** ===================================================================
*/

static struct {
  int kind;	/* PT_SSH, PT_WSL, PT_DOCKER */
  int again;	/* Rebuild Container: the script again, rebuilding */
  char folder[1024];	/* the remote mme's */
  char config[1024];	/* a dev container's devcontainer.json ("": not one) */
} RW;


/* the remote mme asks at its exit (eports.c: its OSC 7717): 1 taken */
int remote_osc (const char *text) {
  if (!remote_mode) return 0;
  if (strcmp(text, "rebuild") == 0) {
    if (RW.config[0]) RW.again = 1;	/* a dev container's (not an attached one) */
    return 1;
  }
  if (strcmp(text, "close-remote") == 0) {
    term_new_window("--new-window");
    return 1;
  }
  if (strcmp(text, "reopen-local") == 0) {
    char *here = NULL;
    if (RW.kind == PT_WSL) here = win_path(RW.folder);
    else if (RW.config[0]) {
      Devc d;
      devc_load(&d, RW.config);
      here = xstrdup(d.folder);
      devc_free(&d);
    }
    term_new_window(here ? here : "--new-window");
    free(here);
    return 1;
  }
  return 0;
}


/* the window's script (and its title), by the spec's kind; NULL: none here */
static char *window_script (const char *spec, int rebuild, char *name, size_t nn) {
  const char *sep = strstr(spec, "::");
  RW.kind = PT_SSH;
  RW.config[0] = '\0';
  if (strncmp(spec, "wsl+", 4) == 0) {
    RW.kind = PT_WSL;
    snprintf(name, nn, "%.*s", sep ? (int)(sep - spec - 4) : (int)strlen(spec + 4), spec + 4);
    snprintf(RW.folder, sizeof(RW.folder), "%s", sep ? sep + 2 : "~");
    snprintf(ui_title, sizeof(ui_title), "%s [WSL: %s]", RW.folder, name);
    ports_target(PT_WSL, name);
#ifdef _WIN32
    return wsl_script(name, RW.folder);
#else
    return NULL;
#endif
  }
  if (strncmp(spec, "dev-container+", 14) == 0 || strncmp(spec, "dev-container-rebuild+", 22) == 0) {
    char title[1200], *f;
    RW.kind = PT_DOCKER;
    snprintf(RW.config, sizeof(RW.config), "%s", strchr(spec, '+') + 1);
    f = devc_script(RW.config, rebuild || spec[13] == '-', title, sizeof(title), name, nn);
    snprintf(ui_title, sizeof(ui_title), "%s", title);
    ports_target(PT_DOCKER, name);
    return f;
  }
  if (strncmp(spec, "attached-container+", 19) == 0) {
    RW.kind = PT_DOCKER;
    snprintf(name, nn, "%.*s", sep ? (int)(sep - spec - 19) : (int)strlen(spec + 19), spec + 19);
    snprintf(RW.folder, sizeof(RW.folder), "%s", sep ? sep + 2 : "");
    snprintf(ui_title, sizeof(ui_title), "%s [Container: %s]", RW.folder[0] ? RW.folder : "/", name);
    ports_target(PT_DOCKER, name);
    return attach_script(name, RW.folder);
  }
  if (sep) {
    snprintf(name, nn, "%.*s", (int)(sep - spec), spec);
    snprintf(RW.folder, sizeof(RW.folder), "%s", sep + 2);
  }
  else {
    snprintf(name, nn, "%s", spec);
    snprintf(RW.folder, sizeof(RW.folder), "~");
  }
  snprintf(ui_title, sizeof(ui_title), "%s [SSH: %s]", RW.folder, name);
  ports_target(PT_SSH, name);
  return connect_script(name, RW.folder);
}


/* the script run in the window's terminal; -1: none here */
static int window_start (const char *spec, int rebuild, int cols, int rows) {
  char name[256], *script, *cmd, *home;
  script = window_script(spec, rebuild, name, sizeof(name));
  if (script == NULL) return -1;
  cmd = (char *)xmalloc(strlen(script) + 8);
#ifdef _WIN32
  sprintf(cmd, strchr(script, ' ') ? "\"%s\"" : "%s", script);
#else
  sprintf(cmd, "sh '%s'", script);
#endif
  home = os_getenv("HOME");
  if (home == NULL) home = os_getenv("USERPROFILE");
  panel_run(cols, rows, name, cmd, home, 0);
  free(home);
  free(cmd);
  free(script);
  return 0;
}


/*
** The remote window: a terminal as big as it, running the script; the
** keys, the mouse, pastes go to it. It ends with the remote mme (a dev
** container's Rebuild Container: the script again, rebuilding).
*/
int remote_main (const char *spec) {
  int cols = 80, rows = 24, done = 0;
  remote_mode = 1;
  if (term_open() != 0) return 1;
  term_size(&cols, &rows);
  scr_resize(cols, rows);
  if (window_start(spec, 0, cols, rows) != 0) {
    term_close();
    fd_puts(2, MME_NAME ": WSL is on Windows only\n");
    return 1;
  }
  while (!done) {
    int k, c2, r2;
    if (panel_poll() == 2 || !panel_alive() || remote_close >= 2) {	/* the remote mme ended; the x twice */
      if (!RW.again || remote_close >= 2) break;
      RW.again = 0;	/* Rebuild Container */
      ports_stop_all();
      panel_kill_all();
      window_start(spec, 1, cols, rows);
      continue;
    }
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

/* }================================================================== */
