/*
** efiles.c - what the Explorer does to files: rename, copy, delete (to
** the Recycle Bin / the Trash when there is one), and show a file in the
** system's file manager
*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int fs_exists (const char *path) {
  OsStat st;
  return os_stat(path, &st) == 0 && st.exists;
}


static int is_dir (const char *path) {
  OsStat st;
  return os_stat(path, &st) == 0 && st.exists && st.is_dir;
}


#ifdef _WIN32

/* UTF-8 to UTF-16, with room for the extra NUL SHFileOperation wants */
static wchar_t *wide (const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)xmalloc(((size_t)(n > 0 ? n : 1) + 1) * sizeof(wchar_t));
  if (n <= 0) n = 1, w[0] = 0;
  else MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  w[n] = 0;
  return w;
}


int fs_rename (const char *from, const char *to) {
  wchar_t *a = wide(from), *b = wide(to);
  int r = MoveFileExW(a, b, 0) ? 0 : -1;
  free(a);
  free(b);
  return r;
}


static int remove_one (const char *path, int dir) {
  wchar_t *w = wide(path);
  int r = (dir ? RemoveDirectoryW(w) : DeleteFileW(w)) ? 0 : -1;
  free(w);
  return r;
}


/* to the Recycle Bin, as VS Code does */
int fs_trash (const char *path) {
  SHFILEOPSTRUCTW op;
  wchar_t *w = wide(path);
  int r;
  memset(&op, 0, sizeof(op));
  op.wFunc = FO_DELETE;
  op.pFrom = w;	/* two NULs end it */
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  r = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted ? 0 : -1;
  free(w);
  return r;
}


/* Reveal in File Explorer: the folder, the file selected */
void fs_reveal (const char *path) {
  Buf b;
  wchar_t *w;
  buf_init(&b);
  buf_printf(&b, "/select,\"%s\"", path);
  buf_putc(&b, '\0');
  w = wide(b.s);
  ShellExecuteW(NULL, L"open", L"explorer.exe", w, NULL, SW_SHOWNORMAL);
  free(w);
  buf_free(&b);
}

#else

int fs_rename (const char *from, const char *to) {
  if (fs_exists(to)) return -1;
  return rename(from, to) == 0 ? 0 : -1;
}


static int remove_one (const char *path, int dir) {
  (void)dir;
  return remove(path) == 0 ? 0 : -1;
}


/* the Trash: ~/.Trash on macOS, the freedesktop one elsewhere; -1: none there */
int fs_trash (const char *path) {
  char *home = os_getenv("HOME"), *dir, *to, *base;
  const char *type = os_type();
  int r = -1, i;
  if (home == NULL) return -1;
  if (strstr(type, "darwin")) dir = path_join(home, ".Trash");
  else {
    char *xdg = os_getenv("XDG_DATA_HOME");
    char *share = xdg && *xdg ? xstrdup(xdg) : path_join(home, ".local/share");
    char *trash = path_join(share, "Trash");
    dir = path_join(trash, "files");
    free(trash);
    free(share);
    free(xdg);
  }
  free(home);
  if (!is_dir(dir)) mkdir_p(dir);
  base = xstrdup(path_basename(path));
  to = path_join(dir, base);
  for (i = 2; fs_exists(to) && i < 1000; i++) {	/* "name 2", "name 3" ... */
    Buf b;
    free(to);
    buf_init(&b);
    buf_printf(&b, "%s %d", base, i);
    buf_putc(&b, '\0');
    to = path_join(dir, b.s);
    buf_free(&b);
  }
  if (is_dir(dir) && rename(path, to) == 0) {
    r = 0;
    if (!strstr(type, "darwin")) {	/* the .trashinfo that says where it was */
      char *info_dir = path_dirname(dir), *idir = path_join(info_dir, "info"), *info;
      Buf b, n;
      int fd;
      buf_init(&n);
      buf_printf(&n, "%s.trashinfo", path_basename(to));
      buf_putc(&n, '\0');
      mkdir_p(idir);
      info = path_join(idir, n.s);
      buf_init(&b);
      buf_printf(&b, "[Trash Info]\nPath=%s\n", path);
      if ((fd = os_open(info, OS_WRITE)) >= 0) {
        os_write(fd, b.s, b.len);
        os_close(fd);
      }
      buf_free(&b);
      buf_free(&n);
      free(info);
      free(idir);
      free(info_dir);
    }
  }
  free(to);
  free(base);
  free(dir);
  return r;
}


/* Reveal in Finder / the file manager */
void fs_reveal (const char *path) {
  int null = os_open("/dev/null", OS_RDWR), io[3];
  char *argv[4];
  OsProc proc;
  long pid;
  char *dir = path_dirname(path);
  io[0] = io[1] = io[2] = null;
  if (strstr(os_type(), "darwin")) {
    argv[0] = "open";
    argv[1] = "-R";
    argv[2] = (char *)path;
  }
  else {
    argv[0] = "xdg-open";
    argv[1] = is_dir(path) ? (char *)path : dir;
    argv[2] = NULL;
  }
  argv[3] = NULL;
  if (os_spawn(argv[0], argv, NULL, io, 3, &proc, &pid) == 0) os_detach(proc);
  if (null >= 0) os_close(null);
  free(dir);
}

#endif


/* a file or a folder with all that is in it; -1 when something stayed */
int fs_remove (const char *path) {
  int r = 0;
  if (is_dir(path)) {
    Vec v;
    size_t i;
    vec_init(&v);
    os_listdir(path, &v);
    for (i = 0; i < v.n; i++) {
      char *p = path_join(path, v.v[i]);
      if (fs_remove(p) != 0) r = -1;
      free(p);
    }
    vec_free(&v);
    if (remove_one(path, 1) != 0) r = -1;
  }
  else if (remove_one(path, 0) != 0) r = -1;
  return r;
}


/* a copy of a file, or of a folder and all in it; -1: it failed */
int fs_copy (const char *from, const char *to) {
  if (fs_exists(to)) return -1;
  if (is_dir(from)) {
    Vec v;
    size_t i;
    int r = 0;
    size_t n = strlen(from);
    if (strncmp(to, from, n) == 0 && path_is_sep(to[n])) return -1;	/* into itself */
    if (os_mkdir(to) != 0 && !is_dir(to)) return -1;
    vec_init(&v);
    os_listdir(from, &v);
    for (i = 0; i < v.n; i++) {
      char *a = path_join(from, v.v[i]), *b = path_join(to, v.v[i]);
      if (fs_copy(a, b) != 0) r = -1;
      free(a);
      free(b);
    }
    vec_free(&v);
    return r;
  }
  else {
    size_t len;
    char *s = read_file(from, &len);
    int fd, r = -1;
    if (s == NULL) return -1;
    if ((fd = os_open(to, OS_EXCL)) >= 0) {
      r = os_write(fd, s, len) == (long)len ? 0 : -1;
      os_close(fd);
    }
    free(s);
    return r;
  }
}


/*
** A name for a copy in folder dir that is not taken, like VS Code's:
** "a.c" -> "a copy.c", "a copy 2.c" ...; malloc'd, the whole path.
*/
char *fs_copy_name (const char *dir, const char *name, int is_folder) {
  const char *dot = is_folder ? NULL : strrchr(name, '.');
  size_t stem = (dot && dot != name) ? (size_t)(dot - name) : strlen(name);
  char *p = path_join(dir, name);
  int i;
  for (i = 1; fs_exists(p) && i < 10000; i++) {
    Buf b;
    free(p);
    buf_init(&b);
    buf_putn(&b, name, stem);
    if (i == 1) buf_puts(&b, " copy");
    else buf_printf(&b, " copy %d", i);
    if (stem < strlen(name)) buf_puts(&b, name + stem);
    buf_putc(&b, '\0');
    p = path_join(dir, b.s);
    buf_free(&b);
  }
  return p;
}
