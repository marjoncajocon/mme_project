/*
** ehistory.c - Local History, like VS Code's workbench.localHistory
**
** Every save keeps a copy of the file in mme-data/history/<hash>/, the
** hash of the file's path: the copies, and "entries", a line for each
** (time, the copy's name, what made it), the oldest first; the first line
** is the file's path. A file keeps its last 50 copies; files bigger than
** 256 KB, and a save that changed nothing, get none. The Explorer's
** TIMELINE lists them (with the file's commits), to compare or restore.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define MAX_ENTRIES	50	/* workbench.localHistory.maxFileEntries */
#define MAX_SIZE	(256 * 1024)	/* workbench.localHistory.maxFileSize */


/* the folder of path's copies: FNV-1a of its real path (case folded on Windows) */
static char *hist_dir (const char *path) {
  char *real = os_realpath(path), hex[16], *h, *d;
  const char *p = real ? real : path;
  unsigned long v = 2166136261UL;
  for (; *p; p++) {
    int c = (unsigned char)*p;
#ifdef _WIN32
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c == '/') c = '\\';
#endif
    v = ((v ^ (unsigned long)c) * 16777619UL) & 0xFFFFFFFFUL;
  }
  free(real);
  snprintf(hex, sizeof(hex), "%08lx", v);
  h = data_path("history");
  d = path_join(h, hex);
  free(h);
  return d;
}


void history_free (HistEntry *v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    free(v[i].file);
    free(v[i].source);
  }
  free(v);
}


/* the entries as they are in the file: the oldest first */
static size_t read_entries (const char *dir, HistEntry **out) {
  char *f = path_join(dir, "entries"), *s, *line;
  HistEntry *v = NULL;
  size_t n = 0, cap = 0;
  int first = 1;
  *out = NULL;
  s = read_file(f, NULL);
  free(f);
  if (s == NULL) return 0;
  for (line = strtok(s, "\r\n"); line; line = strtok(NULL, "\r\n")) {
    char *t1, *t2;
    if (first) {	/* the file's path */
      first = 0;
      continue;
    }
    t1 = strchr(line, '\t');
    if (t1 == NULL) continue;
    *t1++ = '\0';
    t2 = strchr(t1, '\t');
    if (t2) *t2++ = '\0';
    if (n == cap) {
      cap = cap ? cap * 2 : 16;
      v = (HistEntry *)xrealloc(v, cap * sizeof(HistEntry));
    }
    v[n].time = strtoll(line, NULL, 10);
    v[n].file = path_join(dir, t1);
    v[n].source = xstrdup(t2 && *t2 ? t2 : "File Saved");
    n++;
  }
  free(s);
  *out = v;
  return n;
}


static void write_entries (const char *dir, const char *path, const HistEntry *v, size_t from, size_t n) {
  char *f = path_join(dir, "entries");
  Buf b;
  size_t i;
  int fd;
  buf_init(&b);
  buf_printf(&b, "%s\n", path);
  for (i = from; i < n; i++)
    buf_printf(&b, "%lld\t%s\t%s\n", v[i].time, path_basename(v[i].file), v[i].source);
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  buf_free(&b);
  free(f);
}


/* a copy of path as it is on disk now (it was just saved); source: "File Saved" ... */
void history_add (const char *path, const char *source) {
  char *dir, *s, name[64], *copy;
  const char *ext = strrchr(path_basename(path), '.');
  size_t len, n, i, from = 0;
  HistEntry *v;
  int fd;
  long long now = (long long)time(NULL);
  if (path == NULL || (s = read_file(path, &len)) == NULL) return;
  if (len > MAX_SIZE) {
    free(s);
    return;
  }
  dir = hist_dir(path);
  mkdir_p(dir);
  n = read_entries(dir, &v);
  if (n > 0) {	/* the same as the last one: no new copy */
    size_t ol;
    char *old = read_file(v[n - 1].file, &ol);
    int same = old && ol == len && memcmp(old, s, len) == 0;
    free(old);
    if (same) {
      history_free(v, n);
      free(dir);
      free(s);
      return;
    }
  }
  snprintf(name, sizeof(name), "%llx%lx%s", now, (unsigned long)n, ext ? ext : "");
  copy = path_join(dir, name);
  if ((fd = os_open(copy, OS_WRITE)) >= 0) {
    os_write(fd, s, len);
    os_close(fd);
    v = (HistEntry *)xrealloc(v, (n + 1) * sizeof(HistEntry));
    v[n].time = now;
    v[n].file = copy;
    v[n].source = xstrdup(source ? source : "File Saved");
    n++;
    copy = NULL;
    if (n > MAX_ENTRIES) {	/* the oldest go */
      from = n - MAX_ENTRIES;
      for (i = 0; i < from; i++) os_unlink(v[i].file);
    }
    write_entries(dir, path, v, from, n);
  }
  free(copy);
  history_free(v, n);
  free(dir);
  free(s);
}


/* path's copies, the newest first; history_free them */
size_t history_list (const char *path, HistEntry **out) {
  char *dir = hist_dir(path);
  HistEntry *v;
  size_t n = read_entries(dir, &v), i;
  free(dir);
  for (i = 0; i < n / 2; i++) {
    HistEntry t = v[i];
    v[i] = v[n - 1 - i];
    v[n - 1 - i] = t;
  }
  *out = v;
  return n;
}


/* "3 min ago", like the Timeline's */
void history_age (long long when, char *out, size_t n) {
  long long d = (long long)time(NULL) - when;
  if (d < 0) d = 0;
  if (d < 60) snprintf(out, n, "now");
  else if (d < 3600) snprintf(out, n, "%lld min%s", d / 60, d / 60 == 1 ? "" : "s");
  else if (d < 86400) snprintf(out, n, "%lld hr%s", d / 3600, d / 3600 == 1 ? "" : "s");
  else if (d < 86400 * 7) snprintf(out, n, "%lld day%s", d / 86400, d / 86400 == 1 ? "" : "s");
  else if (d < 86400 * 30) snprintf(out, n, "%lld wk%s", d / (86400 * 7), d / (86400 * 7) == 1 ? "" : "s");
  else if (d < 86400 * 365) snprintf(out, n, "%lld mo", d / (86400 * 30));
  else snprintf(out, n, "%lld yr%s", d / (86400 * 365), d / (86400 * 365) == 1 ? "" : "s");
}
