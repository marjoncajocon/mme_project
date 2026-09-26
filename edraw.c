/*
** edraw.c - the screen: a grid of cells, sent to the terminal by lines
**
** The editor draws the whole picture into 'back' every time; scr_flush
** compares it with 'front' (what the terminal shows) and sends only the
** lines that changed, in one write. The colors come from etheme.c.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct ECell {
  uint32_t ch;	/* 0: the right half of a wide character */
  uint16_t st;	/* S_*, TOK(), or S_RGB: fg, bg and at say it */
  uint16_t w;	/* columns: 1 or 2 */
  uint32_t fg, bg;	/* S_RGB: 0xRRGGBB */
  uint32_t at;	/* S_RGB: RGB_BOLD ... */
  uint32_t ul;	/* S_RGB with RGB_CURLY: the underline's color */
} ECell;

static struct {
  int cols, rows;
  ECell *back, *front;
  int full;	/* send every line */
  int cx, cy;	/* the cursor, cy < 0: hidden */
  int cz;	/* the zone the cursor is in, -1: the screen */
} S;


/*
** Text zones (mme.h): a rectangle of the screen with cells of another
** size. D is where the scr_* calls draw now: the screen, or a zone, whose
** cell (col, row) is still asked for at (ox + col, oy + row).
*/
#define MAX_ZONE	8

typedef struct Zone {
  int on;	/* its cells are not the screen's */
  int x, y, w, h;	/* the screen's cells it covers */
  int cols, rows;	/* its own */
  ECell *back, *front;
  int full;	/* painted again whole */
} Zone;

static Zone Z[MAX_ZONE];
static int zm_uw, zm_uh, zm_tw, zm_th;	/* the cells' pixels: the screen's, a zone's; 0: one size (a terminal) */

static struct {
  ECell *b;
  int ox, oy, cols, rows, z;
} D;


static void draw_screen (void) {
  D.b = S.back;
  D.ox = D.oy = 0;
  D.cols = S.cols;
  D.rows = S.rows;
  D.z = -1;
}


/* the cell being drawn at x, y; NULL: outside */
static ECell *cell_at (int x, int y) {
  x -= D.ox;
  y -= D.oy;
  if (x < 0 || y < 0 || x >= D.cols || y >= D.rows) return NULL;
  return &D.b[(size_t)y * (size_t)D.cols + (size_t)x];
}


static int zones_differ (void) {
  return zm_uw > 0 && zm_uh > 0 && zm_tw > 0 && zm_th > 0 && (zm_uw != zm_tw || zm_uh != zm_th);
}


void scr_zone_metrics (int ui_w, int ui_h, int tx_w, int tx_h) {
  if (ui_w == zm_uw && ui_h == zm_uh && tx_w == zm_tw && tx_h == zm_th) return;
  zm_uw = ui_w;
  zm_uh = ui_h;
  zm_tw = tx_w;
  zm_th = tx_h;
  S.full = 1;
}


void scr_zone_size (int w, int h, int *cols, int *rows) {
  *cols = w;
  *rows = h;
  if (w <= 0 || h <= 0 || !zones_differ()) return;
  *cols = (int)((long)w * zm_uw / zm_tw);
  *rows = (int)((long)h * zm_uh / zm_th);
  if (*cols < 1) *cols = 1;
  if (*rows < 1) *rows = 1;
}


void scr_zone_set (int id, int x, int y, int w, int h, int *cols, int *rows) {
  Zone *z;
  int c = w, r = h;
  if (id < 0 || id >= MAX_ZONE) return;
  z = &Z[id];
  if (w > 0 && h > 0 && zones_differ()) {
    scr_zone_size(w, h, &c, &r);
    if (!z->on || c != z->cols || r != z->rows) {
      free(z->back);
      free(z->front);
      z->back = (ECell *)calloc((size_t)c * (size_t)r, sizeof(ECell));
      z->front = (ECell *)calloc((size_t)c * (size_t)r, sizeof(ECell));
      if (z->back == NULL || z->front == NULL) xmalloc((size_t)-1);	/* dies */
      z->full = 1;
    }
    if (z->x != x || z->y != y || z->w != w || z->h != h) z->full = 1;
    z->on = 1;
    z->x = x;
    z->y = y;
    z->w = w;
    z->h = h;
    z->cols = c;
    z->rows = r;
  }
  else {
    if (z->on) S.full = 1;	/* the screen's cells under it are seen again */
    z->on = 0;
  }
  if (cols) *cols = c;
  if (rows) *rows = r;
}


#define ZONE_HOLE	0x110000u	/* a screen's cell that shows the zone under it (not a character) */

void scr_zone_hole (int id) {
  const Zone *z = id >= 0 && id < MAX_ZONE ? &Z[id] : NULL;
  int x, y;
  if (z == NULL || !z->on) return;
  for (y = z->y; y < z->y + z->h && y < S.rows; y++)
    for (x = z->x; x < z->x + z->w && x < S.cols; x++) {
      ECell *c = &S.back[(size_t)y * (size_t)S.cols + (size_t)x];
      c->ch = ZONE_HOLE;
      c->st = S_TEXT;
      c->w = 1;
      c->fg = c->bg = c->at = c->ul = 0;
    }
}


void scr_zone_use (int id) {
  Zone *z = id >= 0 && id < MAX_ZONE ? &Z[id] : NULL;
  if (z == NULL || !z->on) {
    draw_screen();
    return;
  }
  D.b = z->back;
  D.ox = z->x;
  D.oy = z->y;
  D.cols = z->cols;
  D.rows = z->rows;
  D.z = id;
}


void scr_zone_to_ui (int id, int zx, int zy, int up, int *ux, int *uy) {
  const Zone *z = id >= 0 && id < MAX_ZONE ? &Z[id] : NULL;
  long r = up ? 1 : 0;
  *ux = zx;
  *uy = zy;
  if (z == NULL || !z->on) return;
  *ux = z->x + (int)(((long)(zx - z->x) * zm_tw + r * (zm_uw - 1)) / zm_uw);
  *uy = z->y + (int)(((long)(zy - z->y) * zm_th + r * (zm_uh - 1)) / zm_uh);
}


static int floor_div (long a, long b) {
  return (int)(a >= 0 ? a / b : -((-a + b - 1) / b));
}


void scr_zone_pt (int id, const Mouse *m, int *tx, int *ty) {
  const Zone *z = id >= 0 && id < MAX_ZONE ? &Z[id] : NULL;
  long fx, fy;
  *tx = m->x;
  *ty = m->y;
  if (z == NULL || !z->on) return;
  fx = (m->fx || m->fy) ? m->fx : m->x * 256L + 128;	/* in 256ths of the screen's cell */
  fy = (m->fx || m->fy) ? m->fy : m->y * 256L + 128;
  *tx = z->x + floor_div((fx - z->x * 256L) * zm_uw, 256L * zm_tw);
  *ty = z->y + floor_div((fy - z->y * 256L) * zm_uh, 256L * zm_th);
}


/*
** Round corners (scr_round): a terminal has whole cells only, mme-sdl paints
** them round (esdl.c). Each corner keeps the style its cell had: one that
** something else was drawn over since is left square.
*/
#define MAX_ROUND	128

typedef struct {
  int x, y, w, h, corners, size;
  uint16_t st[4];	/* the corners' cells' styles: top left, top right, bottom left, bottom right */
} Round;

static Round g_round[MAX_ROUND];
static int g_nround;


static const char *style_sgr (int st) {
  return theme_sgr(st);
}


/*
** {==================================================================
** UTF-8
** ===================================================================
*/

uint32_t utf8_decode (const char *s, size_t n, size_t *len) {
  const unsigned char *u = (const unsigned char *)s;
  uint32_t cp;
  size_t need, i;
  if (u[0] < 0x80) {
    *len = 1;
    return u[0];
  }
  if (u[0] >= 0xF0 && u[0] < 0xF8) { need = 4; cp = u[0] & 0x07; }
  else if (u[0] >= 0xE0) { need = 3; cp = u[0] & 0x0F; }
  else if (u[0] >= 0xC0) { need = 2; cp = u[0] & 0x1F; }
  else need = 0, cp = 0;
  if (need == 0 || need > n) {
    *len = 1;
    return 0xFFFD;
  }
  for (i = 1; i < need; i++) {
    if ((u[i] & 0xC0) != 0x80) {
      *len = 1;
      return 0xFFFD;
    }
    cp = (cp << 6) | (u[i] & 0x3F);
  }
  *len = need;
  return cp;
}


int utf8_encode (uint32_t cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}


size_t str_cols (const char *s) {
  size_t n = strlen(s), i = 0, len, w = 0;
  while (i < n) {
    w += (size_t)uc_width(utf8_decode(s + i, n - i, &len));
    i += len;
  }
  return w;
}

/* }================================================================== */


void scr_resize (int cols, int rows) {
  free(S.back);
  free(S.front);
  S.cols = cols;
  S.rows = rows;
  S.back = (ECell *)calloc((size_t)cols * (size_t)rows, sizeof(ECell));
  S.front = (ECell *)calloc((size_t)cols * (size_t)rows, sizeof(ECell));
  if (S.back == NULL || S.front == NULL) xmalloc((size_t)-1);	/* dies */
  S.full = 1;
  S.cy = -1;
  draw_screen();
}


int scr_cols (void) {
  return S.cols;
}


int scr_rows (void) {
  return S.rows;
}


void scr_redraw (void) {
  S.full = 1;
}


static void cells_clear (ECell *c, size_t n, int st) {
  size_t i;
  for (i = 0; i < n; i++) {
    c[i].ch = ' ';
    c[i].st = (uint16_t)st;
    c[i].w = 1;
    c[i].fg = c[i].bg = c[i].at = c[i].ul = 0;
  }
}


void scr_clear (int st) {
  int k;
  g_nround = 0;	/* a new picture: its own round corners */
  cells_clear(S.back, (size_t)S.cols * (size_t)S.rows, st);
  for (k = 0; k < MAX_ZONE; k++)
    if (Z[k].on) cells_clear(Z[k].back, (size_t)Z[k].cols * (size_t)Z[k].rows, st);
  draw_screen();
}


int scr_put (int x, int y, uint32_t ch, int st) {
  ECell *c = cell_at(x, y);
  int w;
  if (c == NULL) return 0;
  w = uc_width(ch);
  if (w == 0) return 0;	/* a mark that joins the one before: not kept */
  if (w == 2 && x + 1 - D.ox >= D.cols) {
    ch = ' ';
    w = 1;
  }
  c->ch = ch;
  c->st = (uint16_t)st;
  c->w = (uint16_t)w;
  c->fg = c->bg = c->at = c->ul = 0;
  if (w == 2) {
    c[1].ch = 0;
    c[1].st = (uint16_t)st;
    c[1].w = 1;
    c[1].fg = c[1].bg = c[1].at = c[1].ul = 0;
  }
  return w;
}


int scr_put_rgb (int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, int at) {
  int w = scr_put(x, y, ch, S_RGB), i;
  for (i = 0; i < w; i++) {
    ECell *c = cell_at(x + i, y);
    c->fg = fg;
    c->bg = bg;
    c->at = (uint32_t)at;
  }
  return w;
}


/* the cell as S_RGB: the colors its style had */
static ECell *own_colors (int x, int y) {
  ECell *c = cell_at(x, y);
  if (c == NULL) return NULL;
  if (c->st != S_RGB) {
    if (c->st >= S_N) theme_tok(c->st, &c->fg, &c->bg);
    else {
      c->fg = ui_color(C_EDITOR_FG);
      c->bg = ui_color(C_EDITOR_BG);
    }
    c->st = S_RGB;
    c->at = 0;
  }
  return c;
}


/* a cell's text color, or its background, changed; the rest kept */
void scr_set_fg (int x, int y, uint32_t fg) {
  ECell *c = own_colors(x, y);
  if (c) c->fg = fg;
}


/* a thin glyph (an indent guide, a ruler) in color fg, the cell's background kept */
void scr_glyph (int x, int y, uint32_t ch, uint32_t fg) {
  ECell *c = own_colors(x, y);
  if (c == NULL || c->w != 1) return;
  c->ch = ch;
  c->fg = fg;
  c->at = 0;
}


/* the character a cell has now (0: the right half of a wide one) */
uint32_t scr_ch (int x, int y) {
  const ECell *c = cell_at(x, y);
  return c ? c->ch : 0;
}


void scr_set_bg (int x, int y, uint32_t bg) {
  ECell *c = own_colors(x, y);
  if (c) c->bg = bg;
}


/* a link under the mouse (Ctrl+hover): underlined, in the link's color */
void scr_underline (int x, int y, int w, uint32_t color) {
  for (; w > 0; w--, x++) {
    ECell *c = own_colors(x, y);
    if (c == NULL) continue;
    c->at |= RGB_UNDER;
    c->fg = color;
  }
}


/* VS Code's squiggle under cells, their colors kept */
void scr_squiggle (int x, int y, int w, uint32_t color) {
  for (; w > 0; w--, x++) {
    ECell *c = cell_at(x, y);
    if (c == NULL) continue;
    if (c->st != S_RGB) {	/* a token on a background: its colors, now its own */
      int st = c->st;
      if (st >= S_N) theme_tok(st, &c->fg, &c->bg);
      else {
        c->fg = ui_color(C_EDITOR_FG);
        c->bg = ui_color(C_EDITOR_BG);
      }
      c->st = S_RGB;
      c->at = 0;
    }
    c->at |= RGB_CURLY;
    c->ul = color;
  }
}


uint32_t tok_color (int t) {
  return ui_color(C_TOK + (t >= 0 && t < T_N ? t : T_TEXT));	/* a token out of range must not leave g_color */
}


int scr_putsw (int x, int y, int w, const char *s, int st) {
  size_t n = strlen(s), i = 0, len;
  int x0 = x;
  while (i < n && x < D.ox + D.cols) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    if (x - x0 + uc_width(cp) > w) break;
    x += scr_put(x, y, cp, st);
    i += len;
  }
  return x - x0;
}


int scr_puts (int x, int y, const char *s, int st) {
  return scr_putsw(x, y, D.ox + D.cols, s, st);
}


void scr_fill (int x, int y, int w, int st) {
  int x0 = x, w0 = w;
  for (; w > 0 && x < D.ox + D.cols; w--, x++) scr_put(x, y, ' ', st);
  if ((st == S_INPUT || st == S_MENU_SEL) && w0 >= 3) scr_round(x0, y, w0, 1, RC_ALL, RR_SMALL);	/* a box to type in, a menu's item */
}


void scr_box (int x, int y, int w, int h, int st) {
  int i;
  for (i = 0; i < h; i++) scr_fill(x, y + i, w, st);
  if ((st == S_BOX || st == S_MENU || st == S_TOAST) && w < S.cols) scr_round(x, y, w, h, RC_ALL, RR_BOX);	/* a popup */
}


/* cells x .. x+w-1, y .. y+h-1 as drawn now, with round corners (in mme-sdl); a row just under one of the same kind makes it taller */
void scr_round (int x, int y, int w, int h, int corners, int size) {
  Round *r;
  int k;
  if (D.z >= 0) return;	/* in a text zone: square */
  if (w < 1 || h < 1 || x < 0 || y < 0 || x + w > S.cols || y + h > S.rows) return;
  if (g_nround > 0) {
    r = &g_round[g_nround - 1];
    if (r->x == x && r->w == w && r->y + r->h == y && r->size == size && r->corners == corners &&
        S.back[(size_t)y * (size_t)S.cols + (size_t)x].st == r->st[0]) {
      r->h += h;
      r->st[2] = S.back[(size_t)(y + h - 1) * (size_t)S.cols + (size_t)x].st;
      r->st[3] = S.back[(size_t)(y + h - 1) * (size_t)S.cols + (size_t)(x + w - 1)].st;
      return;
    }
  }
  if (g_nround == MAX_ROUND) return;
  r = &g_round[g_nround++];
  r->x = x;
  r->y = y;
  r->w = w;
  r->h = h;
  r->corners = corners;
  r->size = size;
  for (k = 0; k < 4; k++) {
    int cx = k & 1 ? x + w - 1 : x, cy = k & 2 ? y + h - 1 : y;
    r->st[k] = S.back[(size_t)cy * (size_t)S.cols + (size_t)cx].st;
  }
}


void scr_restyle (int x, int y, int w, int st) {
  for (; w > 0; w--, x++) {
    ECell *c = cell_at(x, y);
    if (c) c->st = (uint16_t)st;
  }
}


size_t scr_text (int x, int y, int w, const char *s, size_t n, size_t left,
                 int st, size_t h0, size_t h1, int hst) {
  size_t i = 0, col = 0, len, right = left + (size_t)(w > 0 ? w : 0);
  while (i < n) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    size_t cw, k;
    int cs = (i >= h0 && i < h1) ? hst : st;
    if (cp == '\t') cw = TABW - col % TABW;
    else if (cp < 32 || cp == 127) cw = 2;
    else cw = (size_t)uc_width(cp);
    if (col >= right) {
      col += cw;
      i += len;
      continue;
    }
    for (k = 0; k < cw; k++) {
      size_t c = col + k;
      int sx = x + (int)(c - left);
      if (c < left || c >= right) continue;
      if (cp == '\t') scr_put(sx, y, ' ', cs);
      else if (cp < 32 || cp == 127) scr_put(sx, y, k == 0 ? '^' : (cp == 127 ? '?' : cp + '@'), cs);
      else if (k == 0 && col >= left && col + cw <= right) scr_put(sx, y, cp, cs);
      else if (k == 0 || col < left) scr_put(sx, y, ' ', cs);
    }
    col += cw;
    i += len;
  }
  return col;
}


size_t scr_code (int x, int y, int w, const char *s, size_t n, size_t left,
                 const unsigned char *tok, int bg, size_t h0, size_t h1, int hbg) {
  size_t i = 0, col = 0, len, right = left + (size_t)(w > 0 ? w : 0);
  while (i < n && col < right) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    size_t cw, k;
    int t = tok ? tok[i] : T_TEXT;
    int cs = TOK(t, (i >= h0 && i < h1) ? hbg : bg);
    if (cp == '\t') cw = TABW - col % TABW;
    else if (cp < 32 || cp == 127) cw = 2;
    else cw = (size_t)uc_width(cp);
    for (k = 0; k < cw; k++) {
      size_t c = col + k;
      int sx = x + (int)(c - left);
      if (c < left || c >= right) continue;
      if (cp == '\t') scr_put(sx, y, ' ', cs);
      else if (cp < 32 || cp == 127) scr_put(sx, y, k == 0 ? '^' : (cp == 127 ? '?' : cp + '@'), cs);
      else if (k == 0 && col >= left && col + cw <= right) scr_put(sx, y, cp, cs);
      else if (k == 0 || col < left) scr_put(sx, y, ' ', cs);
    }
    col += cw;
    i += len;
  }
  return col;
}


void scr_cursor (int x, int y) {
  S.cx = x;
  S.cy = y;
  S.cz = y < 0 ? -1 : D.z;
}


/* editor.cursorStyle and editor.cursorBlinking: sent with the next picture */
static int g_shape = 5, g_shape_sent = 5;	/* term_open asks for a blinking bar */

void scr_cursor_shape (int decscusr) {
  g_shape = decscusr;
}


/*
** The mouse pointer's shape: OSC 22 ; <name> ST, which xterm, kitty and
** WezTerm understand ("default" the arrow, "text" the I beam, "pointer" the
** hand over what can be clicked). A terminal that does not know it drops
** the sequence.
*/
static int g_ptr = PTR_TEXT, g_ptr_sent = -1;

void scr_pointer (int shape) {
  g_ptr = shape;
}


/* the pictures waiting to be sent (the image preview's, a notebook's plots) */
#define MAX_IMG	8

typedef struct Pict {
  int x, y, w, h;
  char *data;
  size_t n;
} Pict;

static Pict IMG[MAX_IMG];
static int NIMG;
static Pict SENT[MAX_IMG];	/* what the terminal already has */
static int NSENT;


void scr_image (int x, int y, int w, int h, const char *data, size_t n) {
  Pict *p;
  if (NIMG == MAX_IMG) return;
  p = &IMG[NIMG++];
  p->data = (char *)xmalloc(n + 1);
  memcpy(p->data, data, n);
  p->n = n;
  p->x = x;
  p->y = y;
  p->w = w;
  p->h = h;
}


void (*scr_overlay_hook) (void);	/* drawn over everything, just before it shows (screencast mode) */

void scr_flush (void) {
  Buf o;
  int x, y, st = -1, img_dirty = 0, i;
  uint32_t fg = 0, bg = 0, at = 0, ul = 0;
  size_t row = (size_t)S.cols * sizeof(ECell);
  if (scr_overlay_hook) scr_overlay_hook();
  buf_init(&o);
  buf_puts(&o, "\033[?25l");
  for (y = 0; y < S.rows; y++) {
    ECell *b = &S.back[y * S.cols];
    if (!S.full && memcmp(b, &S.front[y * S.cols], row) == 0) continue;
    for (i = 0; i < NIMG; i++)
      if (y >= IMG[i].y && y < IMG[i].y + IMG[i].h) img_dirty = 1;	/* a picture's cells were written over */
    buf_printf(&o, "\033[%d;1H", y + 1);
    for (x = 0; x < S.cols; x++) {
      char u[4];
      uint32_t ch = b[x].ch;
      if (ch == 0) {
        if (x > 0 && b[x - 1].w == 2) continue;	/* drawn with its left half */
        ch = ' ';
      }
      if (b[x].st == S_RGB) {	/* its own colors: sent when they change */
        const ECell *c = &b[x];
        if (st != S_RGB || c->fg != fg || c->bg != bg || c->at != at || c->ul != ul) {
          buf_printf(&o, "\033[0;%s%s%s%s%s38;2;%u;%u;%u;48;2;%u;%u;%um",
                     (c->at & RGB_BOLD) ? "1;" : "", (c->at & RGB_DIM) ? "2;" : "",
                     (c->at & RGB_ITALIC) ? "3;" : "",
                     (c->at & RGB_UNDER) ? "4;" : "", (c->at & RGB_STRIKE) ? "9;" : "",
                     (unsigned)(c->fg >> 16), (unsigned)((c->fg >> 8) & 255), (unsigned)(c->fg & 255),
                     (unsigned)(c->bg >> 16), (unsigned)((c->bg >> 8) & 255), (unsigned)(c->bg & 255));
          if (c->at & RGB_CURLY)	/* a squiggle: curly, in its own color */
            buf_printf(&o, "\033[4:3m\033[58;2;%u;%u;%um", (unsigned)(c->ul >> 16),
                       (unsigned)((c->ul >> 8) & 255), (unsigned)(c->ul & 255));
          fg = c->fg;
          bg = c->bg;
          at = c->at;
          ul = c->ul;
        }
        st = S_RGB;
      }
      else if (b[x].st != st) {
        st = b[x].st;
        buf_puts(&o, style_sgr(st));
      }
      buf_putn(&o, u, (size_t)utf8_encode(ch, u));
    }
  }
  memcpy(S.front, S.back, row * (size_t)S.rows);
  {	/* the pictures: sent when the terminal has not got these, there */
    int again = S.full || img_dirty || NIMG != NSENT;
    for (i = 0; !again && i < NIMG; i++)
      again = SENT[i].n != IMG[i].n || SENT[i].x != IMG[i].x || SENT[i].y != IMG[i].y ||
              memcmp(SENT[i].data, IMG[i].data, IMG[i].n) != 0;
    if (again) {
      for (i = 0; i < NSENT; i++) free(SENT[i].data);
      for (i = 0; i < NIMG; i++) {
        buf_printf(&o, "\033[%d;%dH", IMG[i].y + 1, IMG[i].x + 1);
        buf_putn(&o, IMG[i].data, IMG[i].n);
        SENT[i] = IMG[i];
        IMG[i].data = NULL;
      }
      NSENT = NIMG;
    }
    for (i = 0; i < NIMG; i++) {	/* the drawing asks for them again every time */
      free(IMG[i].data);
      IMG[i].data = NULL;
    }
    NIMG = 0;
  }
  S.full = 0;
  if (g_ptr != g_ptr_sent) {	/* the hand over buttons, the I beam over text */
    static const char *const name[] = {"default", "text", "pointer"};
    buf_printf(&o, "\033]22;%s\033\\", name[g_ptr]);
    g_ptr_sent = g_ptr;
  }
  if (g_shape != g_shape_sent) {
    buf_printf(&o, "\033[%d q", g_shape);
    g_shape_sent = g_shape;
  }
  if (S.cy >= 0 && S.cy < S.rows && S.cx >= 0 && S.cx < S.cols)
    buf_printf(&o, "\033[%d;%dH\033[?25h", S.cy + 1, S.cx + 1);
  term_write(o.s, o.len);
  buf_free(&o);
}
