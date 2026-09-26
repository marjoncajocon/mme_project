/*
** eports.c - Remote-SSH's forwarded ports, as VS Code's Ports view: a port
** of the remote machine made a port of this one (localhost), to open what
** runs there (a web server) in this computer's browser.
**
** A Remote-SSH window is two mme: the one here (eremote.c) runs ssh in a
** full-window terminal, and the one on the host (MME_REMOTE set by the
** connect script) is what is seen in it. The host's mme asks the one here
** with an OSC of its own, written to its terminal:
**
**   ESC ] 7717 ; forward=3000 BEL     forward the host's port 3000
**   ESC ] 7717 ; unforward=3000 BEL   stop it
**   ESC ] 7717 ; open=3000 BEL        open http://localhost:<its local port>
**
** The one here runs "ssh -N -L 127.0.0.1:L:localhost:3000 host" for it
** (L: 3000, or the next port free here) and answers on the terminal's
** input, as a bracketed paste the host's mme takes for itself (eterm.c):
**
**   ESC [ 200 ~ mme-ports:forwarded=3000:3000 ESC [ 201 ~   (remote:local)
**   ESC [ 200 ~ mme-ports:failed=3000:why ESC [ 201 ~
**
** A paste, because it is what reaches the host whatever is in between:
** Windows' console (ConPTY, under ssh.exe) drops an OSC on the input, but
** hands a bracketed paste over as it came.
**
** Only a Remote-SSH window listens, only a port is ever asked for, and only
** a forwarded port's localhost URL is opened. A URL with a localhost port
** printed in the host's terminal is forwarded by itself
** (remote.autoForwardPorts, on by default), as VS Code does.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Sock;
#define NO_SOCK		INVALID_SOCKET
#define sock_close	closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int Sock;
#define NO_SOCK		(-1)
#define sock_close	close
#endif


#define MAX_PORTS	32


/* something listens on 127.0.0.1:port here */
static int listening (int port) {
  struct sockaddr_in a;
  Sock s;
  int ok;
#ifdef _WIN32
  static int wsa = 0;
  if (!wsa) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
    wsa = 1;
  }
#endif
  s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == NO_SOCK) return 0;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(0x7F000001);
  ok = connect(s, (struct sockaddr *)&a, sizeof(a)) == 0;
  sock_close(s);
  return ok;
}


static int port_of (const char *s) {	/* 1 .. 65535, else 0 */
  long n = 0;
  if (*s < '0' || *s > '9') return 0;
  while (*s >= '0' && *s <= '9' && n < 100000) n = n * 10 + (*s++ - '0');
  return n >= 1 && n <= 65535 ? (int)n : 0;
}


/*
** {==================================================================
** The host's mme: what is asked, and the Ports view
** ===================================================================
*/

enum { P_ASKED, P_ON, P_FAILED };

static struct {
  int port, local, state, autof;
  char why[120];
} R[MAX_PORTS];
static int nR;
static int g_remote = -1;


int ports_remote (void) {	/* mme runs on a Remote-SSH host */
  if (g_remote < 0) {
    char *e = os_getenv("MME_REMOTE");
    g_remote = e && *e;
    free(e);
  }
  return g_remote;
}


static void ask_local (const char *what, int port) {
  char s[64];
  snprintf(s, sizeof(s), "\033]7717;%s=%d\a", what, port);
  term_write(s, strlen(s));
}


static int find_r (int port) {
  int i;
  for (i = 0; i < nR; i++)
    if (R[i].port == port) return i;
  return -1;
}


static void forward (int port, int autof) {
  int i = find_r(port);
  if (i >= 0 && R[i].state != P_FAILED) {
    if (!autof) toast(0, "Port %d is forwarded already.", port);
    return;
  }
  if (i < 0) {
    if (nR == MAX_PORTS) {
      toast(1, "Ports: %d are forwarded; stop one first.", MAX_PORTS);
      return;
    }
    i = nR++;
  }
  memset(&R[i], 0, sizeof(R[i]));
  R[i].port = port;
  R[i].autof = autof;
  R[i].state = P_ASKED;
  ask_local("forward", port);
}


static void unforward (int i) {
  ask_local("unforward", R[i].port);
  toast(0, "Port %d is no longer forwarded.", R[i].port);
  memmove(&R[i], &R[i + 1], (size_t)(nR - i - 1) * sizeof(R[0]));
  nR--;
}


static void open_ask (void *ud, int choice) {
  if (choice == 0) ask_local("open", (int)(intptr_t)ud);
}


/* the answer of the mme here (eterm.c: a paste of "mme-ports:" and it): "forwarded=3000:3001", "failed=3000:why" */
void ports_reply (const char *text) {
  const char *eq = strchr(text, '='), *colon;
  int port, i;
  if (eq == NULL || (port = port_of(eq + 1)) == 0 || (i = find_r(port)) < 0) return;
  colon = strchr(eq, ':');
  if (strncmp(text, "forwarded=", 10) == 0) {
    static const char *const act[] = {"Open in Browser"};
    char msg[160];
    R[i].state = P_ON;
    R[i].local = colon ? port_of(colon + 1) : port;
    if (R[i].local == 0) R[i].local = port;
    if (R[i].local == port) snprintf(msg, sizeof(msg), "Your application running on port %d is available.", port);
    else snprintf(msg, sizeof(msg), "Your application running on port %d is available here on port %d.", port, R[i].local);
    toast_ask(0, "Ports", msg, act, 1, open_ask, (void *)(intptr_t)port);
  }
  else if (strncmp(text, "failed=", 7) == 0) {
    R[i].state = P_FAILED;
    snprintf(R[i].why, sizeof(R[i].why), "%s", colon ? colon + 1 : "");
    toast(1, "Port %d could not be forwarded%s%s", port, R[i].why[0] ? ": " : ".", R[i].why);
  }
}


/* the host's terminal printed this: a localhost URL's port is forwarded (remote.autoForwardPorts) */
void ports_scan (const char *s, size_t n) {
  static const char *const hosts[] = {"localhost:", "127.0.0.1:", "0.0.0.0:", "[::]:", "[::1]:"};
  size_t i, k;
  if (!ports_remote() || !json_bool(settings_get("remote\\.autoForwardPorts"), 1)) return;
  for (i = 0; i < n; i++) {
    for (k = 0; k < sizeof(hosts) / sizeof(hosts[0]); k++) {
      size_t hl = strlen(hosts[k]);
      if (i + hl < n && memcmp(s + i, hosts[k], hl) == 0) {
        char num[8];
        size_t j = 0;
        int port;
        while (i + hl + j < n && j < 6 && s[i + hl + j] >= '0' && s[i + hl + j] <= '9') {
          num[j] = s[i + hl + j];
          j++;
        }
        num[j] = '\0';
        port = j >= 2 && j <= 5 && !(i + hl + j < n && s[i + hl + j] >= '0' && s[i + hl + j] <= '9') ? port_of(num) : 0;
        if (port >= 1024 && find_r(port) < 0) forward(port, 1);	/* not the system's own (ssh 22, http 80) */
        i += hl;
        break;
      }
    }
  }
}


static void ask_port (void) {
  char *s = ask_text("Port number to forward (a port of the remote machine)", NULL);
  int port;
  if (s == NULL) return;
  port = port_of(s);
  if (port == 0) toast(1, "'%s' is not a port number (1 to 65535).", s);
  else forward(port, 0);
  free(s);
}


/* Ports: the forwarded ones, each with what can be done */
static void ports_view (void) {
  Pick p;
  char label[160], detail[200];
  int i, r;
  pick_init(&p, "Ports");
  p.keep_order = 1;
  for (i = 0; i < nR; i++) {
    snprintf(label, sizeof(label), "%d  \xE2\x86\x92  localhost:%d", R[i].port, R[i].local ? R[i].local : R[i].port);
    snprintf(detail, sizeof(detail), "%s%s", R[i].state == P_ON ? "forwarded" : R[i].state == P_ASKED ? "forwarding..." :
             "failed", R[i].autof ? " (from the terminal)" : " (by you)");
    pick_add(&p, label, detail, 0);
  }
  pick_add(&p, "Forward a Port...", "", 0);
  r = pick_run(&p);
  pick_free(&p);
  if (r == nR) ask_port();
  else if (r >= 0 && r < nR) {
    static const char *const acts[] = {"Open in Browser", "Stop Forwarding Port", "Copy Local Address"};
    Pick a;
    int c;
    pick_init(&a, "Port");
    a.keep_order = 1;
    for (c = 0; c < 3; c++) pick_add(&a, acts[c], "", 0);
    c = pick_run(&a);
    pick_free(&a);
    if (c == 0) {
      if (R[r].state == P_ON) ask_local("open", R[r].port);
      else toast(1, "Port %d is not forwarded (yet).", R[r].port);
    }
    else if (c == 1) unforward(r);
    else if (c == 2) {
      char addr[40];
      snprintf(addr, sizeof(addr), "localhost:%d", R[r].local ? R[r].local : R[r].port);
      clip_set(addr, strlen(addr));
      toast(0, "Copied %s", addr);
    }
  }
}


void ports_command (int cmd) {
  if (!ports_remote()) {
    toast(1, "Ports are forwarded in a Remote-SSH window (Remote-SSH: Connect to Host...).");
    return;
  }
  if (cmd == CMD_PORT_FORWARD) ask_port();
  else ports_view();
}

/* }================================================================== */


/*
** {==================================================================
** The mme here, in the Remote-SSH window: ssh -L for each
** ===================================================================
*/

static struct {
  int port, local, ok;
  OsProc proc;
  long pid;
  int err;	/* ssh's stderr (why it failed) */
  long long t0;
} L[MAX_PORTS];
static int nL;
static char g_host[256];


void ports_host (const char *host) {	/* eremote.c: the window's host */
  snprintf(g_host, sizeof(g_host), "%s", host);
}


/* to the host's mme, on its input: "forwarded=3000:3001", or "failed=3000:why" */
static void answer (int port, int local, const char *why) {
  char s[300];
  int n;
  if (why) n = snprintf(s, sizeof(s), "\033[200~mme-ports:failed=%d:%.200s\033[201~", port, why);
  else n = snprintf(s, sizeof(s), "\033[200~mme-ports:forwarded=%d:%d\033[201~", port, local);
  if (n > 0 && n < (int)sizeof(s)) panel_send(s, (size_t)n);
}


static void local_stop (int i) {
  os_kill(L[i].pid, 15);
  os_wait(L[i].proc);
  if (L[i].err >= 0) os_close(L[i].err);
  memmove(&L[i], &L[i + 1], (size_t)(nL - i - 1) * sizeof(L[0]));
  nL--;
}


static void local_forward (int port) {
  char *ssh = find_program("ssh"), spec[80], *argv[16];
  int i, local = port, fds[2], io[3], a = 0;
  for (i = 0; i < nL; i++)
    if (L[i].port == port) {	/* again: the same answer */
      if (L[i].ok) answer(port, L[i].local, NULL);
      free(ssh);
      return;
    }
  if (ssh == NULL || nL == MAX_PORTS || !g_host[0]) {
    answer(port, 0, ssh == NULL ? "ssh was not found here" : "too many ports");
    free(ssh);
    return;
  }
  while (local < 65535 && local < port + 50 && listening(local)) local++;	/* taken here: the next one free */
  snprintf(spec, sizeof(spec), "127.0.0.1:%d:localhost:%d", local, port);
  argv[a++] = ssh;
  argv[a++] = (char *)"-N";
  argv[a++] = (char *)"-o";
  argv[a++] = (char *)"ExitOnForwardFailure=yes";
  argv[a++] = (char *)"-o";
  argv[a++] = (char *)"BatchMode=yes";	/* nothing can be typed to it: a key, or the connection's master */
#ifndef _WIN32
  argv[a++] = (char *)"-o";
  argv[a++] = (char *)"ControlPath=~/.ssh/mme-%r@%h-%p";	/* the window's own connection, when it has one */
#endif
  argv[a++] = (char *)"-L";
  argv[a++] = spec;
  argv[a++] = g_host;
  argv[a] = NULL;
  memset(&L[nL], 0, sizeof(L[0]));
  L[nL].err = -1;
  io[0] = -1;
  io[1] = -1;
  io[2] = -1;
  if (os_pipe(fds) == 0) {
    io[2] = fds[1];
    L[nL].err = fds[0];
  }
  if (os_spawn(ssh, argv, NULL, io, 3, &L[nL].proc, &L[nL].pid) != 0) {
    if (L[nL].err >= 0) {
      os_close(fds[0]);
      os_close(fds[1]);
    }
    answer(port, 0, "ssh could not be started");
    free(ssh);
    return;
  }
  if (io[2] >= 0) os_close(io[2]);
  L[nL].port = port;
  L[nL].local = local;
  L[nL].t0 = os_now_us();
  nL++;
  free(ssh);
}


/* the OSC 7717 of the host's mme (epanel.c, in a Remote-SSH window only) */
void ports_osc (const char *text) {
  const char *eq = strchr(text, '=');
  int port = eq ? port_of(eq + 1) : 0, i;
  if (!remote_mode || port == 0) return;
  if (strncmp(text, "forward=", 8) == 0) local_forward(port);
  else if (strncmp(text, "unforward=", 10) == 0) {
    for (i = 0; i < nL; i++)
      if (L[i].port == port) {
        local_stop(i);
        break;
      }
  }
  else if (strncmp(text, "open=", 5) == 0) {
    for (i = 0; i < nL; i++)
      if (L[i].port == port && L[i].ok) {
        char url[40];
        snprintf(url, sizeof(url), "http://localhost:%d", L[i].local);
        browse(url);
      }
  }
}


/* the Remote-SSH window's loop: each ssh listening yet (forwarded), or ended (failed) */
void ports_poll (void) {
  int i, st;
  for (i = 0; i < nL; i++) {
    if (!L[i].ok && listening(L[i].local)) {
      L[i].ok = 1;
      answer(L[i].port, L[i].local, NULL);
    }
    if (os_poll_proc(L[i].proc, &st) == 1) {	/* ended: what it said is why */
      char why[200] = "", *p;
      long n = 0;
      if (L[i].err >= 0 && os_wait_readable(L[i].err, 0) == 1) n = os_read(L[i].err, why, sizeof(why) - 1);
      why[n > 0 ? n : 0] = '\0';
      for (p = why; *p; p++)
        if (*p == '\r' || *p == '\n' || *p == '\a' || *p == '\033') *p = ' ';
      if (strstr(why, "Permission denied") || strstr(why, "BatchMode") || strstr(why, "password"))
        snprintf(why, sizeof(why), "ssh needs a key for this (no password can be asked here): ssh-keygen, then ssh-copy-id");
      if (!why[0]) snprintf(why, sizeof(why), "ssh ended (%d)", st);
      if (L[i].err >= 0) os_close(L[i].err);
      answer(L[i].port, 0, why);
      memmove(&L[i], &L[i + 1], (size_t)(nL - i - 1) * sizeof(L[0]));
      nL--;
      i--;
    }
  }
}


void ports_stop_all (void) {	/* the window closes: its ssh go too */
  while (nL > 0) local_stop(nL - 1);
}

/* }================================================================== */
