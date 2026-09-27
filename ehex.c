/*
** ehex.c - the hex viewer, like VS Code's Hex Editor
**
** A binary file shows as "offset | 16 bytes | the letters", 16 to a row.
** The keys move a cursor through the bytes (arrows, PgUp / PgDn, Home /
** End, Ctrl+G to an offset, Ctrl+F for bytes or text); the byte under it
** is told in the status line. It only reads: mme does not write binaries.
** A big file is not read whole: its bytes come from the disk a page at a
** time, as they are shown or looked through.
** A debugger's memory (View Binary Data) shows the same way, like VS
** Code's memory inspector: the whole address space, the offsets the
** addresses, the cursor on the variable's. Its bytes come from the
** adapter (readMemory) a page at a time as they are shown, a few pages
** kept; what the adapter cannot read shows as "??", and each stop reads
** again what is shown (the program may have changed it).
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define COLS	16	/* bytes a row */

#define MPAGE	4096	/* the bytes of a debuggee's memory asked at once (readMemory) */
#define NMPAGE	64	/* the pages of it kept: 256 KB a view, the one looked at least long ago goes */

enum { MB_NONE, MB_READ, MB_BAD };	/* a byte of memory: not read (yet), read, unreadable ("??") */

typedef struct MPage {	/* a page of a debuggee's memory */
  unsigned long long at;	/* its first byte's address, a multiple of MPAGE */
  unsigned char b[MPAGE];
  unsigned char st[MPAGE];	/* MB_* */
  int used, pending;	/* pending: asked, not answered yet */
  unsigned gen, want;	/* the stop it was read at (Hex.gen; older: read again when shown); asked at */
  unsigned long use;	/* when it was looked at last */
} MPage;

typedef struct Hex {
  char *path;
  unsigned char *b;	/* the bytes from off: the whole file, or the page of a big one read last */
  size_t off, nb;
  int paged;	/* too big to read whole */
  size_t n;	/* the file's size */
  size_t at;	/* the byte the cursor is on */
  size_t top;	/* the first row shown */
  int x, y, w, h;	/* where it was drawn, for the mouse */
  char find[64];	/* what was looked for last */
  size_t flen;
  int ow;	/* the offsets' columns */
  char *mref;	/* a debuggee's memory: its memoryReference (the bytes are its addresses'); NULL a file */
  unsigned long long ref;	/* the address it is */
  int known, asked;	/* ref is known (a reference not a number: the first answer says it); that is asked */
  unsigned gen;	/* counts the stops */
  unsigned long clock;
  MPage *mp;	/* NMPAGE */
  struct Hex *next;	/* the memory views open */
} Hex;

#define MAX_HEX	(64 << 20)	/* a file bigger than this is read a page at a time, not whole */
#define PAGE	(1 << 20)	/* the bytes of a big file read at once */

static Hex *g_mem;	/* the memory views, for the adapter's answers */


/* the file (again) into h: whole when it is small, else its size (the pages come as needed); -1: unreadable */
static int load (Hex *h, const char *path) {
  OsStat st;
  size_t len = 0;
  char *s = NULL;
  if (os_stat(path, &st) != 0 || st.is_dir || (unsigned long long)st.size > (size_t)-1 / 2) return -1;
  if (st.size <= MAX_HEX && (s = read_file_all(path, &len)) == NULL) return -1;
  free(h->b);
  h->b = (unsigned char *)s;
  h->paged = s == NULL;
  h->off = 0;
  h->nb = h->paged ? 0 : len;
  h->n = h->paged ? (size_t)st.size : len;
  return 0;
}


/*
** The bytes at 'at' as far as they are read in (*avail of them, need at
** least unless the file ends first); NULL: past the end, or unreadable.
** A page starts a little before at: scrolling back does not read again.
*/
static const unsigned char *span (Hex *h, size_t at, size_t need, size_t *avail) {
  if (at >= h->n) return NULL;
  if (need > PAGE / 2) need = PAGE / 2;
  if (at < h->off || at >= h->off + h->nb || (h->off + h->nb - at < need && h->off + h->nb < h->n)) {
    size_t from = at - at % (PAGE / 4), want;
    long got;
    if (!h->paged) return NULL;
    if (at - from + need > PAGE) from = at;
    want = h->n - from < PAGE ? h->n - from : PAGE;
    if (h->b == NULL) h->b = (unsigned char *)xmalloc(PAGE);
    got = fs_read_at(h->path, (unsigned long long)from, h->b, want);
    h->off = from;
    h->nb = got > 0 ? (size_t)got : 0;
    if (at >= h->off + h->nb) return NULL;	/* it got shorter since */
  }
  *avail = h->off + h->nb - at;
  return h->b + (at - h->off);
}


/* the page of memory that has address a, NULL: none kept */
static MPage *mfind (Hex *h, unsigned long long a) {
  unsigned long long at = a - a % MPAGE;
  int i;
  for (i = 0; i < NMPAGE; i++)
    if (h->mp[i].used && h->mp[i].at == at) return &h->mp[i];
  return NULL;
}


/*
** The page of memory that has address a, asked of the adapter when it is
** not kept or was read before the last stop (the old bytes show until the
** new come); the page looked at least long ago makes room.
*/
static MPage *mpage (Hex *h, unsigned long long a) {
  MPage *p = mfind(h, a);
  if (p == NULL) {
    int i;
    for (i = 0; i < NMPAGE; i++) {
      MPage *q = &h->mp[i];
      if (!q->used) {
        p = q;
        break;
      }
      if (p == NULL || (p->pending && !q->pending) || (p->pending == q->pending && q->use < p->use)) p = q;
    }
    memset(p->st, MB_NONE, sizeof(p->st));
    p->used = 1;
    p->at = a - a % MPAGE;
    p->pending = 0;
    p->gen = h->gen - 1;
  }
  p->use = ++h->clock;
  if (h->known && !p->pending && p->gen != h->gen &&
      dbg_read_memory(h->mref, (long long)(p->at - h->ref), p->at, MPAGE) == 0) {
    p->pending = 1;
    p->want = h->gen;
  }
  return p;
}


/* a byte of memory: 0..255, else -1 not read (yet), -2 unreadable */
static int mem_byte (Hex *h, size_t at) {
  MPage *p = mpage(h, at);
  size_t i = (size_t)(at - p->at);
  return p->st[i] == MB_READ ? p->b[i] : p->st[i] == MB_BAD ? -2 : -1;
}


static unsigned char byte_at (Hex *h, size_t at) {
  size_t n;
  const unsigned char *p;
  if (h->mref) {
    int b = mem_byte(h, at);
    return b >= 0 ? (unsigned char)b : 0;
  }
  p = span(h, at, 1, &n);
  return p ? *p : 0;
}


/* a byte as it shows: 0..255, or -1 nothing (memory not read yet), -2 "??" */
static int shown_byte (Hex *h, size_t at) {
  return h->mref ? mem_byte(h, at) : byte_at(h, at);
}


/* the page of path; NULL when it cannot be read */
void *hex_open (const char *path) {
  Hex *h = (Hex *)xmalloc(sizeof(Hex));
  memset(h, 0, sizeof(*h));
  if (load(h, path) != 0) {
    free(h);
    return NULL;
  }
  h->path = xstrdup(path);
  h->ow = 8;
  return h;
}


/*
** The memory view of a memoryReference ("0x2000"): every address there is,
** the cursor on the reference's, a few rows over it shown. Nothing is read
** yet: the rows ask for their pages as they are drawn.
*/
void *hex_mem_open (const char *mref) {
  Hex *h = (Hex *)xmalloc(sizeof(Hex));
  char *e;
  memset(h, 0, sizeof(*h));
  h->path = xstrdup("memory");
  h->mref = xstrdup(mref);
  h->mp = (MPage *)xmalloc(NMPAGE * sizeof(MPage));
  memset(h->mp, 0, NMPAGE * sizeof(MPage));
  h->n = (size_t)-1;	/* addresses 0 .. 2^64 - 2 */
  h->ref = strtoull(mref, &e, 0);
  h->known = *mref != '\0' && *e == '\0';
  h->at = (size_t)h->ref;
  h->top = h->at / COLS > 4 ? h->at / COLS - 4 : 0;
  h->ow = 8;
  h->next = g_mem;
  g_mem = h;
  return h;
}


/* the memoryReference of a memory view; NULL: a file's */
const char *hex_mem_ref (void *page) {
  Hex *h = (Hex *)page;
  return h ? h->mref : NULL;
}


/* the program stopped (or said its memory changed): what is shown is read again */
void hex_mem_stale (void) {
  Hex *h;
  for (h = g_mem; h; h = h->next) h->gen++;
}


/*
** The adapter's answer to the readMemory of count bytes at address at: n
** bytes from daddr, then skip it could not read (its unreadableBytes); ok
** 0: it failed (all of it is "??"). The rest of the page after the bytes
** it skipped is asked again, as VS Code's inspector does.
*/
void hex_mem_got (const char *mref, unsigned long long at, unsigned count, int ok, unsigned long long daddr,
                  const unsigned char *b, size_t n, unsigned long long skip) {
  Hex *h;
  for (h = g_mem; h; h = h->next) {
    unsigned long long rest = count;	/* from at: what is asked again */
    MPage *p = NULL;
    unsigned k;
    if (strcmp(h->mref, mref) != 0) continue;
    if (!h->known) {	/* the first answer: where the reference is; its pages are asked as they show */
      h->known = 1;
      h->ref = ok ? daddr : 0;
      h->at = (size_t)h->ref;
      h->top = h->at / COLS > 4 ? h->at / COLS - 4 : 0;
      continue;
    }
    if (ok && n + skip > 0 && daddr - at < count && (daddr - at) + n + skip < count) rest = (daddr - at) + n + skip;
    for (k = 0; k < count; k++) {
      unsigned long long a = at + k, d = a - daddr;
      size_t i;
      if (p == NULL || a - p->at >= MPAGE) p = mfind(h, a);
      if (p == NULL) continue;
      i = (size_t)(a - p->at);
      if (ok && d < n) {
        p->st[i] = MB_READ;
        p->b[i] = b[d];
      }
      else p->st[i] = k >= rest ? MB_NONE : MB_BAD;
    }
    if (rest < count && dbg_read_memory(h->mref, (long long)(at + rest - h->ref), at + rest,
                                        (unsigned)(count - rest)) == 0) continue;	/* the page waits for it */
    for (k = 0; k < count; k += MPAGE)
      if ((p = mfind(h, at + k)) != NULL && p->pending) {
        p->pending = 0;
        p->gen = p->want;
      }
    if ((p = mfind(h, at + count - 1)) != NULL && p->pending) {
      p->pending = 0;
      p->gen = p->want;
    }
  }
}


void hex_close (void *page) {
  Hex *h = (Hex *)page, **pp;
  if (h == NULL) return;
  for (pp = &g_mem; *pp; pp = &(*pp)->next)
    if (*pp == h) {
      *pp = h->next;
      break;
    }
  free(h->path);
  free(h->b);
  free(h->mref);
  free(h->mp);
  free(h);
}


/* the file again from the disk (it changed outside); memory: read again */
void hex_reload (void *page) {
  Hex *h = (Hex *)page;
  if (h && h->mref) {
    h->gen++;
    return;
  }
  if (h == NULL || load(h, h->path) != 0) return;
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
  unsigned b;
  if (h == NULL) return "";
  if (h->mref) {	/* memory: the address, and the byte there as far as it is read */
    int m = h->known ? mem_byte(h, h->at) : -1;
    if (m >= 0) snprintf(s, sizeof(s), "0x%0*llX   0x%02X (%d)", h->ow, (unsigned long long)h->at, (unsigned)m, m);
    else snprintf(s, sizeof(s), "0x%0*llX   %s", h->ow, (unsigned long long)h->at, m == -2 ? "unreadable" : "reading...");
    return s;
  }
  b = h->n ? byte_at(h, h->at) : 0;
  if (h->n == 0) snprintf(s, sizeof(s), "empty");
  else snprintf(s, sizeof(s), "0x%08llX of 0x%llX   0x%02X (%u)", (unsigned long long)h->at,
                (unsigned long long)h->n, b, b);
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
static void head_row (int x, int y, int w, int ow, int addr) {
  char s[160];
  int c, n = snprintf(s, sizeof(s), "%-*s", ow + 2, addr ? "Address" : "Offset");
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
  rows = h - 2;
  if (hx->mref) {	/* the addresses' columns: 16 once they are past 4 GB */
    unsigned long long last = ((unsigned long long)hx->top + (unsigned long long)rows) * COLS;
    hx->ow = hx->top > 0xFFFFFFFFULL / COLS || last > 0xFFFFFFFFULL ? 16 : 8;
  }
  head_row(x, y, w, hx->ow, hx->mref != NULL);
  if (hx->mref && !hx->known) {	/* a reference that is not a number: where it is, asked first */
    if (!hx->asked && dbg_read_memory(hx->mref, 0, 0, MPAGE) == 0) hx->asked = 1;
    scr_putsw(x + 1, y + 1, w - 2, "Reading the memory...", S_LINE);
    rows = 0;
  }
  for (r = 0, row = hx->top; r < rows; r++, row++) {
    int cx = x + 1, c;
    size_t off = row * COLS;
    if (off >= hx->n && !(hx->n == 0 && row == 0)) break;
    snprintf(s, sizeof(s), "%0*llX", hx->ow, (unsigned long long)off);
    cx += scr_puts(cx, y + 1 + r, s, S_LINE) + 2;
    for (c = 0; c < COLS; c++) {	/* the bytes: "??" what the adapter cannot read, blank what it has not yet */
      size_t at = off + (size_t)c;
      int st = S_TEXT, b;
      if (at >= hx->n) {
        cx += 3;
        continue;
      }
      b = shown_byte(hx, at);
      if (at == hx->at) st = focus ? S_SEL : S_MATCH;
      else if (b <= 0) st = S_LINE;	/* zeros dim, as VS Code shows them */
      if (b >= 0) snprintf(s, sizeof(s), "%02X", (unsigned)b);
      else snprintf(s, sizeof(s), "%s", b == -2 ? "??" : "  ");
      if (cx + 2 < x + w) scr_puts(cx, y + 1 + r, s, st);
      cx += 3;
    }
    cx++;
    for (c = 0; c < COLS; c++) {	/* the letters */
      size_t at = off + (size_t)c;
      int b, st = S_TEXT;
      if (at >= hx->n || cx >= x + w) break;
      b = shown_byte(hx, at);
      if (at == hx->at) st = focus ? S_SEL : S_MATCH;
      else if (b < 32 || b >= 127) st = S_LINE;
      scr_put(cx++, y + 1 + r, (b >= 32 && b < 127) ? b : b == -1 ? ' ' : b == -2 ? '?' : '.', st);
    }
  }
  {	/* the line under it: the file, where the cursor is, what the byte is */
    char line[300];
    if (hx->mref)
      snprintf(line, sizeof(line), " Memory %.64s   %s   read-only", hx->mref,
               hex_status(page));
    else snprintf(line, sizeof(line), " %s   %llu bytes   %s   read-only%s", path_basename(hx->path),
                  (unsigned long long)hx->n, hex_status(page), hx->paged ? " (read from the disk as it is shown)" : "");
    scr_fill(x, y + h - 1, w, S_STATUS);
    scr_putsw(x, y + h - 1, w, line, S_STATUS);
  }
  if (focus) {	/* the cursor sits on the byte */
    size_t r2 = hx->at / COLS;
    if (r2 >= hx->top && r2 < hx->top + (size_t)rows)
      scr_cursor(x + 1 + hx->ow + 2 + (int)(hx->at % COLS) * 3, y + 1 + (int)(r2 - hx->top));
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


/* a byte of memory as it was read, -1 not (nothing is asked) */
static int mem_peek (Hex *h, unsigned long long a) {
  MPage *p = mfind(h, a);
  return p && p->st[a - p->at] == MB_READ ? p->b[a - p->at] : -1;
}


/* the bytes in the memory read so far, from 'from' up: the address space is too big to be read through */
static size_t mem_find (Hex *h, const unsigned char *q, size_t m, size_t from) {
  unsigned long long cur = from;
  for (;;) {
    MPage *best = NULL;
    unsigned long long a, end;
    int i;
    for (i = 0; i < NMPAGE; i++)	/* the next page kept */
      if (h->mp[i].used && h->mp[i].at + (MPAGE - 1) >= cur && (best == NULL || h->mp[i].at < best->at)) best = &h->mp[i];
    if (best == NULL || m == 0) return (size_t)-1;
    end = best->at + (MPAGE - 1);
    for (a = cur > best->at ? cur : best->at;; a++) {
      size_t k;
      for (k = 0; k < m && mem_peek(h, a + k) == q[k]; k++) {}
      if (k == m) return (size_t)a;
      if (a == end) break;
    }
    if (end == (unsigned long long)-1) return (size_t)-1;
    cur = end + 1;
  }
}


/* the next place of the bytes from 'from'; (size_t)-1 none. A big file is looked through a page at a time */
static size_t find_from (Hex *h, const unsigned char *q, size_t m, size_t from) {
  size_t i = from;
  if (h->mref) return mem_find(h, q, m, from);
  if (m == 0 || m > h->n) return (size_t)-1;
  while (i + m <= h->n) {
    size_t n, k;
    const unsigned char *p = span(h, i, m, &n);
    if (p == NULL || n < m) break;
    for (k = 0; k + m <= n; k++)
      if (p[k] == q[0] && memcmp(p + k, q, m) == 0) return i + k;
    i += n - m + 1;
  }
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
    toast(0, h->mref ? "No results in the memory read so far" : "No results");
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
      else if (k == CTRL('g')) {	/* Ctrl+G: to an offset, "1f40" or "0x1f40" or "2048"; memory: an address */
        char *s = ask_text(h->mref ? "Go to address (hex, or 10# for decimal)" : "Go to offset (hex, or 10# for decimal)", "");
        if (s) {
          size_t at;
          const char *p = s;
          if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
          at = strchr(p, '#') ? (size_t)strtoull(strchr(p, '#') + 1, NULL, 10) : (size_t)strtoull(p, NULL, 16);
          if (at < h->n) {
            h->at = at;
            if (h->mref) h->top = at / COLS > rows / 2 ? at / COLS - rows / 2 : 0;	/* in the middle, memory around it */
          }
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
  c = (mx - h->x - h->ow - 4) / 3;	/* the bytes' columns */
  if (mx - h->x >= h->ow + 4 + COLS * 3) c = mx - h->x - h->ow - 4 - COLS * 3 - 1;	/* the letters */
  if (c < 0) c = 0;
  if (c >= COLS) c = COLS - 1;
  at = (h->top + (size_t)r) * COLS + (size_t)c;
  if (at < h->n) h->at = at;
  return 1;
}

/* }================================================================== */
