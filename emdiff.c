/*
** emdiff.c - the multi-diff editor, VS Code's "View All Changes"
**
** Source Control's title (the diff icon), or "Git: View All Changes" /
** "Git: View Staged Changes", opens a tab "Git: Changes": every changed
** file one under the other, each a header (its chevron, icon, name, folder,
** the lines it adds and removes, its letter) over its diff - side by side
** when there is room, one above the other when not - with the lines that
** did not change folded away, as VS Code shows them. The diffs are egit.c's
** own: each file's is kept aside and put in the diff editor's place to be
** drawn. A click or Enter on a header folds the file; Enter or a double
** click on a line opens the file there, on a fold opens the fold. The tab
** follows the repository: when git's status is read again, so are the diffs.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define MAX_FILES	500	/* more changes than this: the first ones, and a notice */
#define DCLICK_US	500000


typedef struct MFile {
  char *path;	/* native */
  char *rel;	/* from the repository's top, shown */
  char letter;	/* M A D U R ... */
  void *diff;	/* its diff, set aside (diff_take); NULL: none could be made */
  int open;	/* not folded */
  size_t add, del;	/* the lines that came and went */
  size_t rows;	/* its diff's rows at the width drawn */
} MFile;

typedef struct MDiff {
  int staged;	/* Staged Changes, else Changes */
  MFile *f;
  size_t n;
  size_t more;	/* the changes past MAX_FILES */
  unsigned gen;	/* git's status it was read for */
  size_t top, sel;	/* the first row shown, the row of the cursor (every file's rows one after the other) */
  int x, y, w, h;	/* where it was drawn: for the mouse, and the keys' pages */
  long long click_at;	/* the last click: a second one soon on the same row opens */
  size_t click_row;
} MDiff;


static void files_free (MDiff *m) {
  size_t i;
  for (i = 0; i < m->n; i++) {
    free(m->f[i].path);
    free(m->f[i].rel);
    if (m->f[i].diff) diff_drop(m->f[i].diff);
  }
  free(m->f);
  m->f = NULL;
  m->n = m->more = 0;
}


/* the changes, and each one's diff (a file folded before stays folded) */
static void read_all (MDiff *m) {
  MFile *old = m->f;
  size_t nold = m->n, i, k, n = git_changes(m->staged);
  const char *top = git_root();
  void *user;
  m->f = NULL;
  m->n = 0;
  m->more = n > MAX_FILES ? n - MAX_FILES : 0;
  if (n > MAX_FILES) n = MAX_FILES;
  m->gen = git_status_gen();
  if (n) m->f = (MFile *)calloc(n, sizeof(MFile));
  user = diff_take();	/* the diff editor's own diff: back when these are made */
  for (i = 0; i < n && m->f; i++) {
    char letter = 'M';
    const char *p = git_change(m->staged, i, &letter);
    MFile *f = &m->f[m->n];
    size_t tl = top ? strlen(top) : 0;
    if (p == NULL) break;
    f->path = xstrdup(p);
    f->rel = xstrdup(top && strncmp(p, top, tl) == 0 && (p[tl] == '/' || p[tl] == '\\') ? p + tl + 1 : p);
    for (k = 0; f->rel[k]; k++)
      if (f->rel[k] == '\\') f->rel[k] = '/';
    f->letter = letter;
    f->open = 1;
    for (k = 0; k < nold; k++)
      if (m_fncmp(old[k].path, p) == 0) f->open = old[k].open;
    if (diff_open(p, m->staged) == 0) {
      diff_embed();
      diff_embed_stats(&f->add, &f->del);
      f->diff = diff_take();
    }
    m->n++;
  }
  diff_put_back(user);
  for (k = 0; k < nold; k++) {
    free(old[k].path);
    free(old[k].rel);
    if (old[k].diff) diff_drop(old[k].diff);
  }
  free(old);
}


void *mdiff_new (int staged) {
  MDiff *m = (MDiff *)calloc(1, sizeof(MDiff));
  if (m == NULL) return NULL;
  m->staged = staged;
  read_all(m);
  return m;
}


void mdiff_close (void *page) {
  MDiff *m = (MDiff *)page;
  if (m == NULL) return;
  files_free(m);
  free(m);
}


const char *mdiff_title (void *page) {
  MDiff *m = (MDiff *)page;
  return m && m->staged ? "Git: Staged Changes" : "Git: Changes";
}


/* the rows of file i: its header, and its diff when it is open */
static size_t file_rows (const MFile *f) {
  return 1 + (f->open && f->diff ? f->rows : 0);
}


static size_t all_rows (const MDiff *m) {
  size_t i, n = 0;
  for (i = 0; i < m->n; i++) n += file_rows(&m->f[i]);
  return n;
}


/* row r: its file, and -1 for the header or the row of the file's diff */
static int row_of (const MDiff *m, size_t r, size_t *fi, long *sub) {
  size_t i;
  for (i = 0; i < m->n; i++) {
    size_t fr = file_rows(&m->f[i]);
    if (r < fr) {
      *fi = i;
      *sub = (long)r - 1;
      return 1;
    }
    r -= fr;
  }
  return 0;
}


/* the first row of file fi */
static size_t file_start (const MDiff *m, size_t fi) {
  size_t i, r = 0;
  for (i = 0; i < fi && i < m->n; i++) r += file_rows(&m->f[i]);
  return r;
}


/* each diff's rows at width w */
static void measure (MDiff *m, int w) {
  size_t i;
  for (i = 0; i < m->n; i++)
    if (m->f[i].diff) {
      diff_swap(m->f[i].diff);
      m->f[i].rows = diff_embed_rows(w);
      diff_swap(m->f[i].diff);
    }
}


/* the cursor's row in view */
static void follow (MDiff *m) {
  size_t h = m->h > 0 ? (size_t)m->h : 1;
  if (m->sel < m->top) m->top = m->sel;
  if (m->sel >= m->top + h) m->top = m->sel - h + 1;
}


static void clamp_rows (MDiff *m) {
  size_t n = all_rows(m), h = m->h > 0 ? (size_t)m->h : 1;
  if (m->sel >= n) m->sel = n ? n - 1 : 0;
  if (m->top + h > n) m->top = n > h ? n - h : 0;
}


/* file i's header at row y */
static void draw_header (const MDiff *m, size_t i, int x, int y, int w, int on) {
  const MFile *f = &m->f[i];
  const char *base = strrchr(f->rel, '/');
  char t[64];
  int st = on ? S_SIDE_SEL : S_SIDE, ist, cx = x + 1, rw;
  uint32_t icon;
  base = base ? base + 1 : f->rel;
  scr_fill(x, y, w, st);
  scr_put(cx, y, f->open ? 0xEAB4 : 0xEAB6, st);	/* chevron-down / chevron-right */
  cx += 2;
  icon = file_icon(base, &ist);
  scr_put(cx, y, icon, on ? st : ist);
  cx += 2;
  if (f->diff) snprintf(t, sizeof(t), " +%lu -%lu  %c ", (unsigned long)f->add, (unsigned long)f->del, f->letter);
  else snprintf(t, sizeof(t), " %c ", f->letter);
  rw = (int)strlen(t);
  cx += scr_putsw(cx, y, x + w - rw - cx, base, st == S_SIDE ? S_SIDE_TITLE : st);
  if (base != f->rel && cx + 2 < x + w - rw) {	/* its folder, dim */
    char dir[512];
    snprintf(dir, sizeof(dir), "%.*s", (int)(base - f->rel - 1), f->rel);
    cx += 1;
    scr_putsw(cx, y, x + w - rw - cx, dir, st == S_SIDE ? S_SIDE_DIM : st);
  }
  if (rw < w) {
    int rx = x + w - rw;
    if (f->diff) {
      char a[24], d[24];
      snprintf(a, sizeof(a), " +%lu", (unsigned long)f->add);
      snprintf(d, sizeof(d), " -%lu", (unsigned long)f->del);
      rx += scr_puts(rx, y, a, st == S_SIDE ? S_GIT_A : st);
      rx += scr_puts(rx, y, d, st == S_SIDE ? S_GIT_D : st);
      rx += scr_puts(rx, y, "  ", st);
    }
    else rx += scr_puts(rx, y, " ", st);
    scr_put(rx, y, (uint32_t)f->letter, st == S_SIDE ? git_letter_style(f->letter) : st);
  }
}


void mdiff_draw (void *page, int x, int y, int w, int h, int focus) {
  MDiff *m = (MDiff *)page;
  size_t r, i, row0 = 0;
  if (m == NULL) return;
  if (m->gen != git_status_gen()) read_all(m);	/* git's status was read again: so are the diffs */
  m->x = x;
  m->y = y;
  m->w = w;
  m->h = h;
  measure(m, w);
  clamp_rows(m);
  scr_box(x, y, w, h, S_TEXT);
  if (m->n == 0) {
    const char *t = git_root() ? (m->staged ? "No staged changes." : "No changes.") : "The folder is not a git repository.";
    scr_putsw(x + 2, y + 1, w - 3, t, S_SIDE_DIM);
    return;
  }
  for (i = 0; i < m->n; i++) {	/* each file's rows that are in view */
    MFile *f = &m->f[i];
    size_t fr = file_rows(f), d0 = row0 + 1;
    if (row0 >= m->top + (size_t)h) break;
    if (row0 + fr > m->top) {
      if (row0 >= m->top) draw_header(m, i, x, y + (int)(row0 - m->top), w, focus && m->sel == row0);
      if (fr > 1) {
        size_t skip = m->top > d0 ? m->top - d0 : 0, sy = d0 + skip - m->top;
        size_t cnt = f->rows - skip;
        if (cnt > (size_t)h - sy) cnt = (size_t)h - sy;
        if (cnt > 0 && skip < f->rows) {
          long sel = m->sel >= d0 && m->sel < d0 + f->rows && focus ? (long)(m->sel - d0) : -1;
          diff_swap(f->diff);
          diff_embed_draw(x, y + (int)sy, w, skip, (int)cnt, sel);
          diff_swap(f->diff);
        }
      }
    }
    row0 += fr;
  }
  r = row0;
  if (m->more && r >= m->top && r < m->top + (size_t)h) {
    char t[96];
    snprintf(t, sizeof(t), "... and %lu more changed files", (unsigned long)m->more);
    scr_putsw(x + 2, y + (int)(r - m->top), w - 3, t, S_SIDE_DIM);
  }
}


/* Enter on row r: a header folds, a fold opens, a line opens the file there */
static void activate (MDiff *m, size_t r, SideAct *act) {
  size_t fi, line;
  long sub;
  int fold;
  MFile *f;
  if (!row_of(m, r, &fi, &sub)) return;
  f = &m->f[fi];
  if (sub < 0) {
    f->open = !f->open;
    return;
  }
  diff_swap(f->diff);
  line = diff_embed_line((size_t)sub, &fold);
  if (fold) diff_embed_expand((size_t)sub);
  diff_swap(f->diff);
  if (fold) return;
  act->what = SA_GO;	/* the file, the cursor at the line */
  act->path = f->path;
  act->line = line;
  act->col = 0;
}


void mdiff_key (void *page, int k, SideAct *act) {
  MDiff *m = (MDiff *)page;
  int code = KEY_CODE(k);
  size_t n, h, fi;
  long sub;
  if (m == NULL) return;
  n = all_rows(m);
  h = m->h > 1 ? (size_t)m->h : 1;
  switch (code) {
    case K_UP: if (m->sel > 0) m->sel--; break;
    case K_DOWN: if (m->sel + 1 < n) m->sel++; break;
    case K_PGUP: m->sel = m->sel > h ? m->sel - h : 0; break;
    case K_PGDN: m->sel = m->sel + h < n ? m->sel + h : (n ? n - 1 : 0); break;
    case K_HOME: m->sel = 0; break;
    case K_END: m->sel = n ? n - 1 : 0; break;
    case K_ENTER: case ' ': activate(m, m->sel, act); break;
    case K_LEFT:	/* to the file's header; on it: folded */
      if (row_of(m, m->sel, &fi, &sub)) {
        if (sub >= 0) m->sel = file_start(m, fi);
        else m->f[fi].open = 0;
      }
      break;
    case K_RIGHT:
      if (row_of(m, m->sel, &fi, &sub) && sub < 0) m->f[fi].open = 1;
      break;
    default: return;
  }
  clamp_rows(m);
  follow(m);
}


void mdiff_mouse (void *page, const Mouse *m, SideAct *act) {
  MDiff *d = (MDiff *)page;
  size_t r, fi;
  long sub;
  if (d == NULL) return;
  if (m->wheel) {
    size_t n = all_rows(d), h = d->h > 0 ? (size_t)d->h : 1, lt = n > h ? n - h : 0;
    if (m->wheel < 0) d->top = d->top > 3 ? d->top - 3 : 0;
    else d->top = d->top + 3 < lt ? d->top + 3 : lt;
    return;
  }
  if (m->button != 0 || !m->press || m->drag || m->y < d->y || m->y >= d->y + d->h) return;
  r = d->top + (size_t)(m->y - d->y);
  if (!row_of(d, r, &fi, &sub)) return;
  d->sel = r;
  if (sub < 0) {	/* a header folds */
    d->f[fi].open = !d->f[fi].open;
    clamp_rows(d);
    d->click_at = 0;
    return;
  }
  if (d->click_row == r && os_now_us() - d->click_at < DCLICK_US) {	/* a double click opens */
    d->click_at = 0;
    activate(d, r, act);
    return;
  }
  d->click_row = r;
  d->click_at = os_now_us();
}


/* Collapse All / Expand All */
void mdiff_fold_all (void *page, int fold) {
  MDiff *m = (MDiff *)page;
  size_t i;
  if (m == NULL) return;
  for (i = 0; i < m->n; i++) m->f[i].open = !fold;
  m->sel = m->top = 0;
}
