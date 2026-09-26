/*
** hx.c - the regression suite's harness: ../harness.c with two more steps.
**
** usage: hx <exe> <file-or-folder> <steps...>
**   k:<keys>   backtick is ESC, | is Enter, ^X is Ctrl-X
**   x:<hex>    the same, as hex byte pairs: a command line cannot carry
**              bytes over 127 (argv is the ANSI code page), so text that is
**              not ASCII goes in this way
**   w:<ms>     wait
**   r:<c>,<r>  the terminal resized to c columns and r rows (a window made smaller)
**   d          dump the screen as text
**   c:<row>    the row with a {rrggbb} marker where the text colour changes
**   b:<row>    the row with a [rrggbb] marker where the BACKGROUND changes
**   a:<row>    the row with a <bold,under,...> marker where the attributes change
**
** b: and a: are what c: cannot see: the selection, the current-line highlight,
** the find matches, the diff's added and removed backgrounds, the squiggles
** under a diagnostic and the italics of a preview tab.
*/
#include "mterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static Pty *p;
static Vt vt;
static Grid *g;

static void pump (int ms) {
  DWORD end = GetTickCount() + (DWORD)ms;
  char buf[65536];
  long n;
  do {
    while ((n = pty_read(p, buf, sizeof buf)) > 0) vt_feed(&vt, buf, (size_t)n);
    Sleep(10);
  } while (GetTickCount() < end);
}

static void dump (void) {
  int y;
  printf("---- cursor %d,%d\n", g->cx, g->cy);
  for (y = 0; y < g->rows; y++) {
    char *t = grid_text(g, 0, y, g->cols - 1, y);
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') t[--n] = 0;
    printf("|%s\n", t);
    free(t);
  }
}

static void put_ch (uint32_t ch) {
  if (ch >= 32 && ch < 127) putchar((int)ch);
  else putchar(ch ? '?' : ' ');
}

/* the text colour along a row */
static void row_fg (int y) {
  uint32_t last = 0xFFFFFFFF;
  const Line *l = grid_line(g, y);
  int x;
  for (x = 0; x < g->cols; x++) {
    uint32_t f = l->c[x].fg;
    if (f != last) { printf("{%06x}", (unsigned)(f & 0xFFFFFF)); last = f; }
    put_ch(l->c[x].ch);
  }
  putchar(10);
}

/* the background colour along a row */
static void row_bg (int y) {
  uint32_t last = 0xFFFFFFFF;
  const Line *l = grid_line(g, y);
  int x;
  for (x = 0; x < g->cols; x++) {
    uint32_t b = l->c[x].bg;
    if (b != last) { printf("[%06x]", (unsigned)(b & 0xFFFFFF)); last = b; }
    put_ch(l->c[x].ch);
  }
  putchar(10);
}

static const struct { unsigned bit; const char *name; } ATTR[] = {
  {A_BOLD, "bold"}, {A_DIM, "dim"}, {A_ITALIC, "italic"}, {A_UNDER, "under"},
  {A_REVERSE, "reverse"}, {A_STRIKE, "strike"}, {A_HIDDEN, "hidden"},
  {A_BLINK, "blink"}, {A_OVER, "over"}
};

/* the attributes along a row (double-width halves are left out: they are layout) */
static void row_attr (int y) {
  unsigned last = 0xFFFFu;
  const Line *l = grid_line(g, y);
  int x;
  for (x = 0; x < g->cols; x++) {
    unsigned a = (unsigned)l->c[x].attr & ~(unsigned)(A_WIDE | A_WCONT);
    if (a != last) {
      size_t i;
      int first = 1;
      putchar('<');
      for (i = 0; i < sizeof(ATTR) / sizeof(ATTR[0]); i++)
        if (a & ATTR[i].bit) {
          if (!first) putchar(',');
          fputs(ATTR[i].name, stdout);
          first = 0;
        }
      if (a & A_ULSTYLE) printf("%sulstyle%u", first ? "" : ",", (a & A_ULSTYLE) >> 11);
      if (first && !(a & A_ULSTYLE)) fputs("-", stdout);
      putchar('>');
      last = a;
    }
    put_ch(l->c[x].ch);
  }
  putchar(10);
}

static size_t unesc (const char *s, char *o) {
  size_t n = 0;
  while (*s) {
    if (*s == '`') o[n++] = 27, s++;
    else if (*s == '|') o[n++] = 13, s++;
    else if (*s == '^' && s[1]) { o[n++] = (char)(s[1] & 0x1f); s += 2; }
    else o[n++] = *s++;
  }
  return n;
}

int main (int argc, char **argv) {
  char *av[3];
  char keys[4096];
  int i, code;
  g = grid_new(120, 30, 0);
  vt_init(&vt, g);
  av[0] = argv[1];
  av[1] = argv[2];
  av[2] = NULL;
  p = pty_spawn(argv[1], av, 120, 30);
  if (!p) {
    printf("spawn failed\n");
    return 1;
  }
  pump(800);
  for (i = 3; i < argc; i++) {
    if (argv[i][0] == 'k') {
      size_t n = unesc(argv[i] + 2, keys);
      pty_write(p, keys, n);
      pump(250);
    }
    else if (argv[i][0] == 'x') {	/* hex byte pairs: what k: cannot spell */
      const char *h = argv[i] + 2;
      size_t n = 0;
      while (h[0] && h[1] && n < sizeof keys) {
        char t[3];
        t[0] = h[0];
        t[1] = h[1];
        t[2] = 0;
        keys[n++] = (char)strtol(t, NULL, 16);
        h += 2;
      }
      pty_write(p, keys, n);
      pump(250);
    }
    else if (argv[i][0] == 'r') {	/* r:cols,rows: the terminal made that size (a window resized) */
      const char *q = strchr(argv[i] + 2, ',');
      int cols = atoi(argv[i] + 2), rows = q ? atoi(q + 1) : 30;
      grid_resize(g, cols, rows);
      pty_resize(p, cols, rows);
      pump(600);
    }
    else if (argv[i][0] == 'w') pump(atoi(argv[i] + 2));
    else if (argv[i][0] == 'd') dump();
    else if (argv[i][0] == 'c') row_fg(atoi(argv[i] + 2));
    else if (argv[i][0] == 'b') row_bg(atoi(argv[i] + 2));
    else if (argv[i][0] == 'a') row_attr(atoi(argv[i] + 2));
  }
  pump(300);
  printf("exited=%d\n", pty_exited(p, &code));
  return 0;
}

void win_wake (void) {}
void win_message (const char *t, const char *s) { (void)t; (void)s; }
