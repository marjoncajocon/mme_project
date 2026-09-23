/*
** ehex.c - the hex viewer, like VS Code's Hex Editor
**
** A binary file shows as "offset | 16 bytes | the letters", 16 to a row.
** The keys move a cursor through the bytes (arrows, PgUp / PgDn, Home /
** End, Ctrl+G to an offset, Ctrl+F for bytes or text); the byte under it
** is told in the status line. It only reads: mme does not write binaries.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define COLS	16	/* bytes a row */

typedef struct Hex {
  char *path;
  unsigned char *b;	/* the whole file */
  size_t n;
  size_t at;	/* the byte the cursor is on */
  size_t top;	/* the first row shown */
  int x, y, w, h;	/* where it was drawn, for the mouse */
  char find[64];	/* what was looked for last */
  size_t flen;
} Hex;

#define MAX_HEX	(64 << 20)	/* a file bigger than this is not read whole */


/* the page of path; NULL when it cannot be read */
void *hex_open (const char *path) {
  Hex *h;
  size_t len = 0;
  char *s = read_file(path, &len);
  OsStat st;
  if (s == NULL) return NULL;
  if (os_stat(path, &st) == 0 && st.size > MAX_HEX) {
    free(s);
    return NULL;
  }
  h = (Hex *)xmalloc(sizeof(Hex));
  memset(h, 0, sizeof(*h));
  h->path = xstrdup(path);
  h->b = (unsigned char *)s;
  h->n = len;
  return h;
}


void hex_close (void *page) {
  Hex *h = (Hex *)page;
  if (h == NULL) return;
  free(h->path);
  free(h->b);
  free(h);
}


/* the file again from the disk (it changed outside) */
void hex_reload (void *page) {
  Hex *h = (Hex *)page;
  size_t len = 0;
  char *s;
  if (h == NULL || (s = read_file(h->path, &len)) == NULL) return;
  free(h->b);
  h->b = (unsigned char *)s;
  h->n = len;
  if (h->at >= h->n) h->at = h->n ? h->n - 1 : 0;
}


size_t hex_size (void *page) {
  Hex *h = (Hex *)page;
  return h ? h->n : 0;
}


/* "Ln" of a hex page for the status bar: the offset and the byte there */
const char *hex_status (void *page) {
  static char s[80];
  Hex *h = (Hex *)page;
  if (h == NULL) return "";
  if (h->n == 0) snprintf(s, sizeof(s), "empty");
  else snprintf(s, sizeof(s), "0x%08lX of 0x%lX   0x%02X (%u)", (unsigned long)h->at,
                (unsigned long)h->n, h->b[h->at], (unsigned)h->b[h->at]);
  return s;
}


static void scroll_to (Hex *h) {
  size_t row = h->at / COLS;
  size_t rows = (size_t)(h->h > 2 ? h->h - 2 : 1);
  if (row < h->top) h->top = row;
  if (row >= h->top + rows) h->top = row - rows + 1;
}


/*
** {==================================================================
** Drawing
** ===================================================================
*/

/* the row of offsets and columns over the bytes, like VS Code's */
static void head_row (int x, int y, int w) {
  char s[160];
  int c, n = snprintf(s, sizeof(s), "%-10s", "Offset");
  for (c = 0; c < COLS && n + 4 < (int)sizeof(s); c++)
    n += snprintf(s + n, sizeof(s) - (size_t)n, "%02X ", c);
  snprintf(s + n, sizeof(s) - (size_t)n, " Decoded text");
  scr_fill(x, y, w, S_SIDE_HEAD);
  scr_putsw(x + 1, y, w - 2, s, S_SIDE_HEAD);
}


void hex_draw (void *page, int x, int y, int w, int h, int focus) {
  Hex *hx = (Hex *)page;
  int r, rows;
  size_t row;
  char s[32];
  if (hx == NULL) return;
  hx->x = x;
  hx->y = y;
  hx->w = w;
  hx->h = h;
  for (r = 0; r < h; r++) scr_fill(x, y + r, w, S_TEXT);
  if (h < 2 || w < 20) return;
  head_row(x, y, w);
  rows = h - 2;
  for (r = 0, row = hx->top; r < rows; r++, row++) {
    int cx = x + 1, c;
    size_t off = row * COLS;
    if (off >= hx->n && !(hx->n == 0 && row == 0)) break;
    snprintf(s, sizeof(s), "%08lX", (unsigned long)off);
    cx += scr_puts(cx, y + 1 + r, s, S_LINE) + 2;
    for (c = 0; c < COLS; c++) {	/* the bytes */
      size_t at = off + (size_t)c;
      int st = S_TEXT;
      if (at >= hx->n) {
        cx += 3;
        continue;
      }
      if (at == hx->at) st = focus ? S_SEL : S_MATCH;
      else if (hx->b[at] == 0) st = S_LINE;	/* zeros dim, as VS Code shows them */
      snprintf(s, sizeof(s), "%02X", hx->b[at]);
      if (cx + 2 < x + w) scr_puts(cx, y + 1 + r, s, st);
      cx += 3;
    }
    cx++;
    for (c = 0; c < COLS; c++) {	/* the letters */
      size_t at = off + (size_t)c;
      unsigned char b;
      int st = S_TEXT;
      if (at >= hx->n || cx >= x + w) break;
      b = hx->b[at];
      if (at == hx->at) st = focus ? S_SEL : S_MATCH;
      else if (b < 32 || b >= 127) st = S_LINE;
      scr_put(cx++, y + 1 + r, (b >= 32 && b < 127) ? b : '.', st);
    }
  }
  {	/* the line under it: the file, where the cursor is, what the byte is */
    char line[200];
    snprintf(line, sizeof(line), " %s   %lu bytes   %s   read-only", path_basename(hx->path),
             (unsigned long)hx->n, hex_status(page));
    scr_fill(x, y + h - 1, w, S_STATUS);
    scr_putsw(x, y + h - 1, w, line, S_STATUS);
  }
  if (focus) {	/* the cursor sits on the byte */
    size_t r2 = hx->at / COLS;
    if (r2 >= hx->top && r2 < hx->top + (size_t)rows)
      scr_cursor(x + 1 + 10 + (int)(hx->at % COLS) * 3, y + 1 + (int)(r2 - hx->top));
  }
}

/* }================================================================== */


/*
** {==================================================================
** Keys
** ===================================================================
*/

/* "4d 5a" or "MZ": the bytes looked for; how many */
static size_t find_bytes (const char *q, unsigned char *out, size_t max) {
  size_t n = 0, i = 0, len = strlen(q);
  int hex = 1;
  for (i = 0; i < len; i++)	/* only hex digits and spaces: bytes */
    if (!((q[i] >= '0' && q[i] <= '9') || (q[i] >= 'a' && q[i] <= 'f') ||
          (q[i] >= 'A' && q[i] <= 'F') || q[i] == ' ')) hex = 0;
  if (hex) {
    for (i = 0; i < len && n < max;) {
      char b[3];
      while (i < len && q[i] == ' ') i++;
      if (i >= len) break;
      b[0] = q[i++];
      b[1] = (i < len && q[i] != ' ') ? q[i++] : '\0';
      b[2] = '\0';
      out[n++] = (unsigned char)strtol(b, NULL, 16);
    }
    if (n) return n;
  }
  for (i = 0; i < len && n < max; i++) out[n++] = (unsigned char)q[i];
  return n;
}


/* the next place of the bytes from 'from'; (size_t)-1 none */
static size_t find_from (const Hex *h, const unsigned char *q, size_t m, size_t from) {
  size_t i;
  if (m == 0 || m > h->n) return (size_t)-1;
  for (i = from; i + m <= h->n; i++)
    if (h->b[i] == q[0] && memcmp(h->b + i, q, m) == 0) return i;
  return (size_t)-1;
}


static void do_find (Hex *h, int again) {
  unsigned char q[64];
  size_t m, at;
  if (!again) {
    char *s = ask_text("Find bytes (4d 5a) or text", h->find);
    if (s == NULL) return;
    snprintf(h->find, sizeof(h->find), "%s", s);
    free(s);
  }
  if (h->find[0] == '\0') return;
  m = find_bytes(h->find, q, sizeof(q));
  at = find_from(h, q, m, h->at + 1);
  if (at == (size_t)-1) at = find_from(h, q, m, 0);	/* around again */
  if (at == (size_t)-1) {
    toast(0, "No results");
    return;
  }
  h->at = at;
  h->flen = m;
  scroll_to(h);
}


/* a key in the hex page; 1 when it was its */
int hex_key (void *page, int k) {
  Hex *h = (Hex *)page;
  size_t rows;
  int code = KEY_CODE(k);
  if (h == NULL) return 0;
  rows = (size_t)(h->h > 2 ? h->h - 2 : 1);
  switch (code) {
    case K_LEFT: if (h->at > 0) h->at--; break;
    case K_RIGHT: if (h->at + 1 < h->n) h->at++; break;
    case K_UP: h->at = h->at >= COLS ? h->at - COLS : 0; break;
    case K_DOWN: if (h->at + COLS < h->n) h->at += COLS; break;
    case K_PGUP: h->at = h->at >= COLS * rows ? h->at - COLS * rows : 0; break;
    case K_PGDN:
      h->at = h->at + COLS * rows < h->n ? h->at + COLS * rows : (h->n ? h->n - 1 : 0);
      break;
    case K_HOME: h->at = (k & KM_CTRL) ? 0 : h->at - h->at % COLS; break;
    case K_END:
      h->at = (k & KM_CTRL) ? (h->n ? h->n - 1 : 0) : h->at - h->at % COLS + COLS - 1;
      if (h->at >= h->n) h->at = h->n ? h->n - 1 : 0;
      break;
    case K_F3: do_find(h, 1); return 1;
    default:
      if (k == CTRL('f')) do_find(h, 0);
      else if (k == CTRL('g')) {	/* Ctrl+G: to an offset, "1f40" or "0x1f40" or "2048" */
        char *s = ask_text("Go to offset (hex, or 10# for decimal)", "");
        if (s) {
          size_t at;
          const char *p = s;
          if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
          at = strchr(p, '#') ? (size_t)strtoul(strchr(p, '#') + 1, NULL, 10) : (size_t)strtoul(p, NULL, 16);
          if (at < h->n) h->at = at;
          free(s);
        }
      }
      else return 0;
      break;
  }
  scroll_to(h);
  return 1;
}


void hex_wheel (void *page, int d) {
  Hex *h = (Hex *)page;
  size_t rows;
  if (h == NULL) return;
  rows = h->n / COLS + 1;
  if (d < 0) h->top = h->top > 3 ? h->top - 3 : 0;
  else if (h->top + 3 < rows) h->top += 3;
}


/* a click puts the cursor on the byte there; 1 when it was in the page */
int hex_click (void *page, int mx, int my) {
  Hex *h = (Hex *)page;
  int r, c;
  size_t at;
  if (h == NULL || mx < h->x || mx >= h->x + h->w || my < h->y + 1 || my >= h->y + h->h - 1) return 0;
  r = my - h->y - 1;
  c = (mx - h->x - 12) / 3;	/* the bytes' columns */
  if (mx - h->x >= 12 + COLS * 3) c = mx - h->x - 12 - COLS * 3 - 1;	/* the letters */
  if (c < 0) c = 0;
  if (c >= COLS) c = COLS - 1;
  at = (h->top + (size_t)r) * COLS + (size_t)c;
  if (at < h->n) h->at = at;
  return 1;
}

/* }================================================================== */
