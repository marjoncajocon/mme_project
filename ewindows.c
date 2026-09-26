/*
** ewindows.c - mme's windows know each other, as VS Code's do: Exit
** closes them all, and a folder open in one window is shown there instead
** of being opened again in another.
**
** Each window keeps mme-data/windows/<pid>: "<front>\n<folder>\n", front 1
** when it can bring itself to the front (mme-sdl; a terminal cannot). One
** window asks another something by writing mme-data/windows/<pid>.ask (a
** word: quit, focus), which that window reads twice a second.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


static char *g_me;	/* windows/<pid> */
static long long g_seen;	/* when the mailbox was looked at last */


static char *win_dir (void) {
  char *d = data_path("windows");
  os_mkdir(d);
  return d;
}


static char *win_file (long pid, const char *ext) {
  char name[48], *d = win_dir(), *f;
  snprintf(name, sizeof(name), "%ld%s", pid, ext);
  f = path_join(d, name);
  free(d);
  return f;
}


static void write_text (const char *path, const char *s) {
  int fd = os_open(path, OS_WRITE);
  if (fd < 0) return;
  os_write(fd, s, strlen(s));
  os_close(fd);
}


/* this window: its folder ("" when it has none: New Window) */
void win_register (const char *folder, int front) {
  Buf b;
  if (g_me == NULL) g_me = win_file(os_getpid(), "");
  buf_init(&b);
  buf_printf(&b, "%d\n%s\n", front, folder ? folder : "");
  write_text(g_me, b.s);
  buf_free(&b);
}


void win_unregister (void) {
  char *ask;
  if (g_me == NULL) return;
  os_unlink(g_me);
  ask = win_file(os_getpid(), ".ask");
  os_unlink(ask);
  free(ask);
  free(g_me);
  g_me = NULL;
}


/*
** The other windows that run: their pids into pids (at most max), and
** with folder != NULL only the one showing that folder that can come to
** the front. The files of windows gone (ended without cleaning up) go.
*/
static int others (long *pids, int max, const char *folder) {
  Vec v;
  char *d = win_dir();
  size_t i;
  int n = 0;
  long me = os_getpid();
  vec_init(&v);
  os_listdir(d, &v);
  for (i = 0; i < v.n && n < max; i++) {
    const char *name = v.v[i];
    char *f, *s;
    size_t len = 0, k;
    long pid;
    for (k = 0; name[k] >= '0' && name[k] <= '9'; k++) ;
    if (k == 0 || name[k] != '\0') continue;	/* windows/<pid> only */
    pid = strtol(name, NULL, 10);
    if (pid == me) continue;
    f = path_join(d, name);
    if (os_kill(pid, 0) != 0) {	/* it is gone */
      char *ask = win_file(pid, ".ask");
      os_unlink(f);
      os_unlink(ask);
      free(ask);
      free(f);
      continue;
    }
    s = read_file(f, &len);
    free(f);
    if (s == NULL) continue;
    if (folder) {
      char *nl = strchr(s, '\n'), *fo = nl ? nl + 1 : NULL, *e = fo ? strchr(fo, '\n') : NULL;
      if (e) *e = '\0';
      if (s[0] != '1' || fo == NULL || *fo == '\0' || m_fncmp(fo, folder) != 0) {
        free(s);
        continue;
      }
    }
    free(s);
    pids[n++] = pid;
  }
  vec_free(&v);
  free(d);
  return n;
}


long win_with (const char *folder) {	/* the window showing folder (it comes to the front when asked); 0 none */
  long pid = 0;
  if (folder == NULL || *folder == '\0') return 0;
  return others(&pid, 1, folder) ? pid : 0;
}


void win_ask (long pid, const char *what) {
  char *f = win_file(pid, ".ask");
#ifdef _WIN32
  AllowSetForegroundWindow((DWORD)pid);	/* this one is in front: it lets that one come there */
#endif
  write_text(f, what);
  free(f);
}


void win_ask_all (const char *what) {	/* every other window */
  long pids[64];
  int n = others(pids, 64, NULL), i;
  for (i = 0; i < n; i++) win_ask(pids[i], what);
}


/* what another window asked of this one: 'q' quit, 'f' come to the front; 0 nothing */
int win_poll (void) {
  long long now = os_now_us();
  char *f, *s;
  size_t len = 0;
  int r = 0;
  if (g_me == NULL || now - g_seen < 500000) return 0;
  g_seen = now;
  f = win_file(os_getpid(), ".ask");
  s = read_file(f, &len);
  if (s) {
    os_unlink(f);
    if (strncmp(s, "quit", 4) == 0) r = 'q';
    else if (strncmp(s, "focus", 5) == 0) r = 'f';
    free(s);
  }
  free(f);
  return r;
}
