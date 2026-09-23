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
} S;


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


void scr_clear (int st) {
  size_t i, n = (size_t)S.cols * (size_t)S.rows;
  for (i = 0; i < n; i++) {
    S.back[i].ch = ' ';
    S.back[i].st = (uint16_t)st;
    S.back[i].w = 1;
    S.back[i].fg = S.back[i].bg = S.back[i].at = S.back[i].ul = 0;
  }
}


int scr_put (int x, int y, uint32_t ch, int st) {
  ECell *c;
  int w;
  if (x < 0 || y < 0 || x >= S.cols || y >= S.rows) return 0;
  w = uc_width(ch);
  if (w == 0) return 0;	/* a mark that joins the one before: not kept */
  if (w == 2 && x + 1 >= S.cols) {
    ch = ' ';
    w = 1;
  }
  c = &S.back[y * S.cols + x];
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
    ECell *c = &S.back[y * S.cols + x + i];
    c->fg = fg;
    c->bg = bg;
    c->at = (uint32_t)at;
  }
  return w;
}


/* the cell as S_RGB: the colors its style had */
static ECell *own_colors (int x, int y) {
  ECell *c;
  if (x < 0 || y < 0 || x >= S.cols || y >= S.rows) return NULL;
  c = &S.back[y * S.cols + x];
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
  if (x < 0 || y < 0 || x >= S.cols || y >= S.rows) return 0;
  return S.back[y * S.cols + x].ch;
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
    ECell *c;
    if (x < 0 || y < 0 || x >= S.cols || y >= S.rows) continue;
    c = &S.back[y * S.cols + x];
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
  while (i < n && x < S.cols) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    if (x - x0 + uc_width(cp) > w) break;
    x += scr_put(x, y, cp, st);
    i += len;
  }
  return x - x0;
}


int scr_puts (int x, int y, const char *s, int st) {
  return scr_putsw(x, y, S.cols, s, st);
}


void scr_fill (int x, int y, int w, int st) {
  for (; w > 0 && x < S.cols; w--, x++) scr_put(x, y, ' ', st);
}


void scr_box (int x, int y, int w, int h, int st) {
  int i;
  for (i = 0; i < h; i++) scr_fill(x, y + i, w, st);
}


void scr_restyle (int x, int y, int w, int st) {
  if (y < 0 || y >= S.rows) return;
  for (; w > 0 && x < S.cols; w--, x++)
    if (x >= 0) S.back[y * S.cols + x].st = (uint16_t)st;
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


/* the picture waiting to be sent (one is enough: the editor shows one at a time) */
static struct {
  int x, y, w, h;
  char *data;
  size_t n;
} IMG;


static struct { int x, y; char *data; size_t n; } SENT;	/* what the terminal already has */


void scr_image (int x, int y, int w, int h, const char *data, size_t n) {
  free(IMG.data);
  IMG.data = (char *)xmalloc(n + 1);
  memcpy(IMG.data, data, n);
  IMG.n = n;
  IMG.x = x;
  IMG.y = y;
  IMG.w = w;
  IMG.h = h;
}


void scr_flush (void) {
  Buf o;
  int x, y, st = -1, img_dirty = 0;
  uint32_t fg = 0, bg = 0, at = 0, ul = 0;
  size_t row = (size_t)S.cols * sizeof(ECell);
  buf_init(&o);
  buf_puts(&o, "\033[?25l");
  for (y = 0; y < S.rows; y++) {
    ECell *b = &S.back[y * S.cols];
    if (!S.full && memcmp(b, &S.front[y * S.cols], row) == 0) continue;
    if (IMG.data && y >= IMG.y && y < IMG.y + IMG.h) img_dirty = 1;	/* its cells were written over */
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
          buf_printf(&o, "\033[0;%s%s%s%s38;2;%u;%u;%u;48;2;%u;%u;%um",
                     (c->at & RGB_BOLD) ? "1;" : "", (c->at & RGB_ITALIC) ? "3;" : "",
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
  if (IMG.data) {	/* a picture: sent only when the terminal has not got this one */
    int again = S.full || img_dirty || SENT.data == NULL || SENT.n != IMG.n ||
                SENT.x != IMG.x || SENT.y != IMG.y || memcmp(SENT.data, IMG.data, IMG.n) != 0;
    if (again) {
      buf_printf(&o, "\033[%d;%dH", IMG.y + 1, IMG.x + 1);
      buf_putn(&o, IMG.data, IMG.n);
      free(SENT.data);
      SENT.data = IMG.data;
      SENT.n = IMG.n;
      SENT.x = IMG.x;
      SENT.y = IMG.y;
      IMG.data = NULL;
    }
  }
  else if (SENT.data) {	/* nothing is shown now: the next one must be sent again */
    free(SENT.data);
    SENT.data = NULL;
    SENT.n = 0;
  }
  free(IMG.data);	/* the drawing asks for it again every time */
  IMG.data = NULL;
  IMG.n = 0;
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
