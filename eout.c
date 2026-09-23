/*
** eout.c - the OUTPUT view: VS Code's output channels
**
** Every part of mme that runs something tells its channel what it did:
** Git the git commands, each language server what it logged, Extensions
** the installs. The panel's OUTPUT tab shows one channel at a time, picked
** from the list in its title row, and follows its end like VS Code's.
*/

#include "mme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define MAX_CHAN	16
#define MAX_LINES	5000	/* a channel keeps its last lines */


typedef struct Chan {
  char name[64];
  char **line;	/* a ring of MAX_LINES */
  int head, n;	/* the first one, how many */
  int open;	/* the last line has not ended yet */
  int top;	/* the first line shown */
  int follow;	/* the view stays at the end */
} Chan;

static Chan g_chan[MAX_CHAN];
static int g_nchan, g_cur = -1;


static Chan *chan (const char *name) {
  int i;
  for (i = 0; i < g_nchan; i++)
    if (strcmp(g_chan[i].name, name) == 0) return &g_chan[i];
  if (g_nchan == MAX_CHAN) return &g_chan[MAX_CHAN - 1];
  i = g_nchan++;
  memset(&g_chan[i], 0, sizeof(g_chan[i]));
  snprintf(g_chan[i].name, sizeof(g_chan[i].name), "%s", name);
  g_chan[i].line = (char **)xmalloc(MAX_LINES * sizeof(char *));
  g_chan[i].follow = 1;
  if (g_cur < 0) g_cur = i;
  return &g_chan[i];
}


static char **at (Chan *c, int i) {
  return &c->line[(c->head + i) % MAX_LINES];
}


static void add_line (Chan *c, const char *s, size_t n) {
  char *l = (char *)xmalloc(n + 1);
  memcpy(l, s, n);
  l[n] = '\0';
  if (c->n == MAX_LINES) {	/* the oldest goes */
    free(c->line[c->head]);
    c->line[c->head] = l;
    c->head = (c->head + 1) % MAX_LINES;
    if (c->top > 0) c->top--;
  }
  else c->line[(c->head + c->n++) % MAX_LINES] = l;
}


/* text into a channel; lines end at '\n' (a '\r' before it goes), the last may stay open */
void out_append (const char *name, const char *s, size_t n) {
  Chan *c = chan(name);
  while (n > 0) {
    const char *nl = (const char *)memchr(s, '\n', n);
    size_t len = nl ? (size_t)(nl - s) : n;
    size_t k = len;
    if (k > 0 && s[k - 1] == '\r') k--;
    if (c->open && c->n > 0) {	/* the rest of the last line */
      char **l = at(c, c->n - 1);
      size_t old = strlen(*l);
      *l = (char *)xrealloc(*l, old + k + 1);
      memcpy(*l + old, s, k);
      (*l)[old + k] = '\0';
    }
    else add_line(c, s, k);
    c->open = nl == NULL;
    if (nl == NULL) break;
    s = nl + 1;
    n -= len + 1;
  }
}


/* a whole line, after the time like VS Code's: "21:40:02.123 [info] > git status" */
void out_log (const char *name, const char *fmt, ...) {
  char msg[2048], t[32];
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  va_list ap;
  Chan *c = chan(name);
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (tm) strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", tm);
  else t[0] = '\0';
  if (c->open) out_append(name, "\n", 1);
  out_append(name, t, strlen(t));
  out_append(name, " ", 1);
  out_append(name, msg, strlen(msg));
  out_append(name, "\n", 1);
}


int out_count (void) {
  return g_nchan;
}


const char *out_name (int i) {
  return (i >= 0 && i < g_nchan) ? g_chan[i].name : "";
}


int out_current (void) {
  return g_cur;
}


void out_select (int i) {
  if (i >= 0 && i < g_nchan) g_cur = i;
}


/* Output: Clear Output */
void out_clear (void) {
  Chan *c;
  int i;
  if (g_cur < 0) return;
  c = &g_chan[g_cur];
  for (i = 0; i < c->n; i++) free(*at(c, i));
  c->n = c->head = c->top = 0;
  c->open = 0;
  c->follow = 1;
}


static int g_h = 1;	/* the rows it had when drawn */


static void scroll (Chan *c, int d) {
  int max = c->n - g_h > 0 ? c->n - g_h : 0;
  c->top += d;
  if (c->top > max) c->top = max;
  if (c->top < 0) c->top = 0;
  c->follow = c->top >= max;	/* back at the end: it follows again */
}


void out_draw (int x, int y, int w, int h, int focus) {
  Chan *c;
  int row;
  (void)focus;
  g_h = h > 0 ? h : 1;
  scr_box(x, y, w, h, S_PANEL);
  if (g_cur < 0) {
    scr_putsw(x + 2, y, w - 4, "Nothing has been written to the output yet.", S_PANEL_TAB);
    return;
  }
  c = &g_chan[g_cur];
  if (c->follow) c->top = c->n - h > 0 ? c->n - h : 0;
  if (c->top > c->n) c->top = c->n;
  for (row = 0; row < h && c->top + row < c->n; row++) {
    const char *l = *at(c, c->top + row);
    const char *info = strstr(l, "[info]"), *err = strstr(l, "[error]"), *warn = strstr(l, "[warning]");
    const char *tag = err ? err : warn ? warn : info;
    if (tag && tag - l < 26) {	/* the time dim, the level colored, the rest plain */
      int cx = x + 1, n = (int)(tag - l), tl = err ? 7 : warn ? 9 : 6;
      char part[64];
      snprintf(part, sizeof(part), "%.*s", n, l);
      cx += scr_putsw(cx, y + row, w - 2, part, S_PANEL_TAB);
      snprintf(part, sizeof(part), "%.*s", tl, tag);
      cx += scr_putsw(cx, y + row, x + w - 1 - cx, part, err ? S_TOAST_WARN : S_PANEL_TAB);
      if (x + w - 1 - cx > 0) scr_putsw(cx, y + row, x + w - 1 - cx, tag + tl, S_PANEL);
    }
    else scr_putsw(x + 1, y + row, w - 2, l, S_PANEL);
  }
}


void out_key (int k) {
  Chan *c;
  if (g_cur < 0) return;
  c = &g_chan[g_cur];
  switch (KEY_CODE(k)) {
    case K_UP: scroll(c, -1); break;
    case K_DOWN: scroll(c, 1); break;
    case K_PGUP: scroll(c, -(g_h - 1)); break;
    case K_PGDN: scroll(c, g_h - 1); break;
    case K_HOME: scroll(c, -c->n); break;
    case K_END: scroll(c, c->n); break;
  }
}


void out_wheel (int d) {
  if (g_cur >= 0) scroll(&g_chan[g_cur], d * wheel_step(0));
}
