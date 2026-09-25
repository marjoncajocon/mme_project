/*
** esdl.c - mme in a window of its own (SDL2), no terminal in between
**
** The editor draws its picture into edraw.c's grid of cells as always;
** here that grid is painted straight into the window - once - instead of
** being sent as escape sequences to a terminal that paints it again. The
** keys, the mouse, the clipboard and the window's size come from SDL too.
** This file stands in for eterm.c and for edraw.c's scr_flush: every other
** file of mme is compiled as it is.
**
** The painting is mmc-term's (tdraw.c: its blending, its box drawing, its
** underlines), the fonts are mmc-term's tfont.c (stb_truetype, and GDI's
** ClearType on Windows), so the window looks like mme in mmc-term.
*/

#define SDL_MAIN_HANDLED	/* mme.c has main(): SDL must not take it */
#include "SDL.h"
#include "SDL_syswm.h"

#define scr_flush scr_flush_vt	/* edraw.c's own: the terminal's escape sequences, not used here */
#include "../edraw.c"
#undef scr_flush

#include "../mterm.h"

#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


Mouse term_mouse;


/*
** {==================================================================
** The window, the font, the picture
** ===================================================================
*/

static struct {
  SDL_Window *win;
  SDL_Renderer *ren;
  SDL_Texture *tex;	/* the picture, as big as the window */
  int tw, th;	/* its size in pixels */
  Frame fr;	/* painted here, then copied into tex */
  int cw, ch, ascent;	/* a cell, in pixels */
  float px, px0;	/* the font's size; the one the settings give (Zoom Reset) */
  float scale;	/* the display's DPI over 96 */
  int ow, oh;	/* the window, in pixels */
  int shown;	/* a picture has been shown */
  int dirty;	/* the window must be shown again (moved, exposed) */
  long long blink_at;	/* when the caret blinked last */
  int blink_on;
  int ccx, ccy, cshape, cvis;	/* the caret drawn last */
  char title[512];
  SDL_Cursor *ptr[3];
  int ptr_now;
} W;


static uint32_t mix (uint32_t bg, uint32_t fg, int a) {
  uint32_t r, g, b;
  if (a <= 0) return bg;
  if (a >= 255) return fg;
  r = (((bg >> 16) & 0xFF) * (uint32_t)(255 - a) + ((fg >> 16) & 0xFF) * (uint32_t)a) / 255;
  g = (((bg >> 8) & 0xFF) * (uint32_t)(255 - a) + ((fg >> 8) & 0xFF) * (uint32_t)a) / 255;
  b = ((bg & 0xFF) * (uint32_t)(255 - a) + (fg & 0xFF) * (uint32_t)a) / 255;
  return (r << 16) | (g << 8) | b;
}


static uint32_t mix3 (uint32_t bg, uint32_t fg, int ar, int ag, int ab) {
  uint32_t r = (((bg >> 16) & 0xFF) * (uint32_t)(255 - ar) + ((fg >> 16) & 0xFF) * (uint32_t)ar) / 255;
  uint32_t g = (((bg >> 8) & 0xFF) * (uint32_t)(255 - ag) + ((fg >> 8) & 0xFF) * (uint32_t)ag) / 255;
  uint32_t b = ((bg & 0xFF) * (uint32_t)(255 - ab) + (fg & 0xFF) * (uint32_t)ab) / 255;
  return (r << 16) | (g << 8) | b;
}


static void fill (Frame *f, int x, int y, int w, int h, uint32_t color) {
  int i, j;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > f->w) w = f->w - x;
  if (y + h > f->h) h = f->h - y;
  for (j = 0; j < h; j++) {
    uint32_t *p = f->px + (size_t)(y + j) * (size_t)f->w + (size_t)x;
    for (i = 0; i < w; i++) p[i] = color;
  }
}


static void fill_alpha (Frame *f, int x, int y, int w, int h, uint32_t color, int a) {
  int i, j;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > f->w) w = f->w - x;
  if (y + h > f->h) h = f->h - y;
  for (j = 0; j < h; j++) {
    uint32_t *p = f->px + (size_t)(y + j) * (size_t)f->w + (size_t)x;
    for (i = 0; i < w; i++) p[i] = mix(p[i], color, a);
  }
}


/* text is thin when light on dark and fat when dark on light: even it out (tdraw.c) */
static unsigned char lut_light[256], lut_dark[256];
static int lut_ready;

static void make_luts (void) {
  int i;
  for (i = 0; i < 256; i++) {
    lut_light[i] = (unsigned char)(pow((double)i / 255.0, 0.78) * 255.0 + 0.5);
    lut_dark[i] = (unsigned char)(pow((double)i / 255.0, 1.10) * 255.0 + 0.5);
  }
  lut_ready = 1;
}


static int brightness (uint32_t c) {
  return (int)(((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + (c & 0xFF));
}


static void blit_glyph (Frame *f, const Glyph *g, int x, int y, uint32_t fg, uint32_t bg, int clip0, int clip1) {
  const unsigned char *lut;
  int i, j;
  if (g == NULL || g->bm == NULL) return;
  if (!lut_ready) make_luts();
  lut = brightness(fg) > brightness(bg) ? lut_light : lut_dark;
  if (clip0 < 0) clip0 = 0;
  if (clip1 > f->w) clip1 = f->w;
  for (j = 0; j < g->h; j++) {
    int py = y + g->yoff + j;
    if (py < 0 || py >= f->h) continue;
    for (i = 0; i < g->w; i++) {
      int px = x + g->xoff + i;
      size_t at = (size_t)j * (size_t)g->w + (size_t)i;
      uint32_t *p;
      if (px < clip0 || px >= clip1) continue;
      p = &f->px[(size_t)py * (size_t)f->w + (size_t)px];
      if (g->lcd == 3) {	/* a color emoji */
        uint32_t c = ((const uint32_t *)(const void *)g->bm)[at], a = c >> 24;
        if (a != 0) *p = a == 255 ? (c & 0xFFFFFF) : mix(*p, c & 0xFFFFFF, (int)a);
      }
      else if (g->lcd == 1) {	/* ClearType */
        const unsigned char *c = g->bm + at * 3;
        if ((c[0] | c[1] | c[2]) != 0) *p = mix3(*p, fg, c[0], c[1], c[2]);
      }
      else if (g->bm[at] != 0) *p = mix(*p, fg, g->lcd == 2 ? g->bm[at] : lut[g->bm[at]]);
    }
  }
}


/* box drawing (U+2500..257F) as geometry, so lines join whatever the font (tdraw.c) */
static const char *const box_arms[128] = {
  "1100", "2200", "0011", "0022", "1100", "2200", "0011", "0022",
  "1100", "2200", "0011", "0022", "0101", "0201", "0102", "0202",
  "1001", "2001", "1002", "2002", "0110", "0210", "0120", "0220",
  "1010", "2010", "1020", "2020", "0111", "0211", "0121", "0112",
  "0122", "0221", "0212", "0222", "1011", "2011", "1021", "1012",
  "1022", "2021", "2012", "2022", "1101", "2101", "1201", "2201",
  "1102", "2102", "1202", "2202", "1110", "2110", "1210", "2210",
  "1120", "2120", "1220", "2220", "1111", "2111", "1211", "2211",
  "1121", "1112", "1122", "2121", "1221", "2112", "1212", "2221",
  "2212", "2122", "1222", "2222", "1100", "2200", "0011", "0022",
  "3300", "0033", "0301", "0103", "0303", "3001", "1003", "3003",
  "0310", "0130", "0330", "3010", "1030", "3030", "0311", "0133",
  "0333", "3011", "1033", "3033", "3301", "1103", "3303", "3310",
  "1130", "3330", "3311", "1133", "3333", "0101", "1001", "1010",
  "0110", "----", "----", "----", "1000", "0010", "0100", "0001",
  "2000", "0020", "0200", "0002", "1200", "0012", "2100", "0021"
};


static int draw_box (Frame *f, uint32_t cp, int x, int y, int w, int h, uint32_t fg) {
  const char *arms = box_arms[cp - 0x2500];
  int a[4], i, t1 = (w + 4) / 8, t2, d, cx = x + w / 2, cy = y + h / 2, dbl_h, dbl_v, active;
  if (arms[0] == '-') return 0;	/* diagonals: the font's */
  if (t1 < 1) t1 = 1;
  t2 = t1 * 2 + (t1 == 1 ? 1 : 0);
  d = t1 + 1;
  for (i = 0; i < 4; i++) a[i] = arms[i] - '0';
  dbl_h = a[0] == 3 || a[1] == 3;
  dbl_v = a[2] == 3 || a[3] == 3;
  active = dbl_h && dbl_v;
  for (i = 0; i < 4; i++) {
    int t = a[i] == 2 ? t2 : t1, o = t / 2, gap = (a[i] != 3 && (i < 2 ? dbl_v : dbl_h)) ? d : 0;
    if (a[i] == 0) continue;
    if (a[i] != 3) {
      if (i == 0) fill(f, x, cy - o, cx - x - gap + (gap ? 0 : t - o), t, fg);
      else if (i == 1) fill(f, cx - o + gap, cy - o, x + w - (cx - o + gap), t, fg);
      else if (i == 2) fill(f, cx - o, y, t, cy - y - gap + (gap ? 0 : t - o), fg);
      else fill(f, cx - o, cy - o + gap, t, y + h - (cy - o + gap), fg);
    }
    else {
      int e = active ? d : -(t1 - t1 / 2), k;
      for (k = -1; k <= 1; k += 2) {
        int off = k * d - t1 / 2;
        if (i == 0) fill(f, x, cy + off, cx - e - x, t1, fg);
        else if (i == 1) fill(f, cx + e, cy + off, x + w - (cx + e), t1, fg);
        else if (i == 2) fill(f, cx + off, y, t1, cy - e - y, fg);
        else fill(f, cx + off, cy + e, t1, y + h - (cy + e), fg);
      }
    }
  }
  if (active) {
    int lo = -d - t1 / 2, len = 2 * d + t1;
    if (a[0] != 3) fill(f, cx + lo, cy + lo, t1, len, fg);
    if (a[1] != 3) fill(f, cx + d - t1 / 2, cy + lo, t1, len, fg);
    if (a[2] != 3) fill(f, cx + lo, cy + lo, len, t1, fg);
    if (a[3] != 3) fill(f, cx + lo, cy + d - t1 / 2, len, t1, fg);
  }
  return 1;
}


/* block elements (U+2580..259F): the scrollbars' thumbs, the minimap's marks */
static int draw_block (Frame *f, uint32_t cp, int x, int y, int w, int h, uint32_t fg) {
  static const unsigned char quads[] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
  int hw = w / 2, hh = h / 2;
  if (cp == 0x2580) fill(f, x, y, w, hh, fg);
  else if (cp >= 0x2581 && cp <= 0x2588) {
    int n = (int)(cp - 0x2580), part = (h * n + 4) / 8;
    fill(f, x, y + h - part, w, part, fg);
  }
  else if (cp >= 0x2589 && cp <= 0x258F) fill(f, x, y, (w * (int)(0x2590 - cp) + 4) / 8, h, fg);
  else if (cp == 0x2590) fill(f, x + hw, y, w - hw, h, fg);
  else if (cp >= 0x2591 && cp <= 0x2593) fill_alpha(f, x, y, w, h, fg, 64 * (int)(cp - 0x2590));
  else if (cp == 0x2594) fill(f, x, y, w, (h + 4) / 8, fg);
  else if (cp == 0x2595) fill(f, x + w - (w + 4) / 8, y, (w + 4) / 8, h, fg);
  else if (cp >= 0x2596 && cp <= 0x259F) {
    int q = quads[cp - 0x2596];
    if (q & 1) fill(f, x, y, hw, hh, fg);
    if (q & 2) fill(f, x + hw, y, w - hw, hh, fg);
    if (q & 4) fill(f, x, y + hh, hw, h - hh, fg);
    if (q & 8) fill(f, x + hw, y + hh, w - hw, h - hh, fg);
  }
  else return 0;
  return 1;
}


/* a style's colors and attributes, from the theme's escape sequence ("\033[0;1;38;2;r;g;b;48;2;r;g;bm") */
typedef struct SColor {
  uint32_t fg, bg, at;
  int ok;
} SColor;

static SColor g_style[S_N + T_N * B_N];


static void style_of (int st, uint32_t *fg, uint32_t *bg, uint32_t *at) {
  SColor *c;
  if (st < 0 || st >= S_N + T_N * B_N) st = S_TEXT;
  c = &g_style[st];
  if (!c->ok) {
    const char *p = theme_sgr(st);
    int v[32], n = 0, i;
    c->fg = ui_color(C_EDITOR_FG);
    c->bg = ui_color(C_EDITOR_BG);
    c->at = 0;
    if (p[0] == 27 && p[1] == '[') p += 2;
    while (*p && *p != 'm' && n < 32) {
      v[n] = 0;
      while (*p >= '0' && *p <= '9') v[n] = v[n] * 10 + (*p++ - '0');
      n++;
      if (*p == ';') p++;
      else if (*p != 'm') break;
    }
    for (i = 0; i < n; i++) {
      if (v[i] == 1) c->at |= RGB_BOLD;
      else if (v[i] == 2) c->at |= RGB_DIM;
      else if (v[i] == 3) c->at |= RGB_ITALIC;
      else if (v[i] == 4) c->at |= RGB_UNDER;
      else if (v[i] == 9) c->at |= RGB_STRIKE;
      else if ((v[i] == 38 || v[i] == 48) && i + 4 < n && v[i + 1] == 2) {
        uint32_t rgb = ((uint32_t)v[i + 2] << 16) | ((uint32_t)v[i + 3] << 8) | (uint32_t)v[i + 4];
        if (v[i] == 38) c->fg = rgb;
        else c->bg = rgb;
        i += 4;
      }
    }
    c->ok = 1;
  }
  *fg = c->fg;
  *bg = c->bg;
  *at = c->at;
}


/* one cell painted: its background, its character (a glyph, or box drawing), its lines */
static void paint_cell (const ECell *c, int col, int row, int wide) {
  Frame *f = &W.fr;
  int px = col * W.cw, py = row * W.ch, w = W.cw * (wide ? 2 : 1), line = (W.ch + 8) / 16;
  uint32_t fg, bg, at, ch = c->ch, ul;
  if (c->st == S_RGB) {
    fg = c->fg;
    bg = c->bg;
    at = c->at;
  }
  else style_of(c->st, &fg, &bg, &at);
  ul = (at & RGB_CURLY) ? c->ul : fg;
  if (at & RGB_DIM) fg = mix(bg, fg, 150);
  if (line < 1) line = 1;
  fill(f, px, py, w, W.ch, bg);
  if (ch > ' ') {
    int drawn = 0;
    if (ch >= 0x2500 && ch <= 0x257F) drawn = draw_box(f, ch, px, py, w, W.ch, fg);
    else if (ch >= 0x2580 && ch <= 0x259F) drawn = draw_block(f, ch, px, py, w, W.ch, fg);
    if (!drawn)
      blit_glyph(f, font_glyph(ch, (at & RGB_BOLD) != 0, (at & RGB_ITALIC) != 0, brightness(fg) < brightness(bg)),
                 px, py + W.ascent, fg, bg, px, px + w);
  }
  if (at & RGB_CURLY) {	/* a squiggle: a wave from cell to cell (tdraw.c) */
    int amp = line + 1, period = W.cw > 4 ? W.cw : 4, x, y = py + W.ascent + line + 1;
    for (x = 0; x < w; x++) {
      double t = (double)((px + x) % period) / (double)period;
      int dy = (int)((double)amp * (1.0 - cos(t * 6.283185307179586)) / 2.0 + 0.5);
      fill(f, px + x, y - 1 + dy, 1, line, ul);
    }
  }
  else if (at & RGB_UNDER) fill(f, px, py + W.ascent + line + 1, w, line, ul);
  if (at & RGB_STRIKE) fill(f, px, py + (W.ascent * 2) / 3, w, line, fg);
}


/* row y of the grid, into the picture */
static void paint_row (int y) {
  const ECell *b = &S.back[(size_t)y * (size_t)S.cols];
  int x;
  for (x = 0; x < S.cols; x++) {
    if (b[x].ch == 0 && x > 0 && b[x - 1].w == 2) continue;	/* the right half of a wide one: painted with it */
    paint_cell(&b[x], x, y, b[x].w == 2 && x + 1 < S.cols);
  }
}


/* the picture as big as the window: a new texture, everything painted again */
static void picture_size (void) {
  int w = S.cols * W.cw, h = S.rows * W.ch;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (W.tex && w == W.tw && h == W.th) return;
  if (W.tex) SDL_DestroyTexture(W.tex);
  W.tex = SDL_CreateTexture(W.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
  W.tw = w;
  W.th = h;
  free(W.fr.px);
  W.fr.px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
  W.fr.w = w;
  W.fr.h = h;
  S.full = 1;
}


static int caret_color (void) {
  return (int)ui_color(C_MCURSOR);
}


/*
** The picture shown: the rows that changed painted into the frame and
** copied into the texture, the caret over it (it blinks, as the settings
** say), the title, the mouse pointer. Nothing changed: nothing is shown.
*/
void scr_flush (void) {
  int y, changed = 0, y0 = S.rows, y1 = -1, cvis, blink;
  size_t row = (size_t)S.cols * sizeof(ECell);
  long long now = (long long)SDL_GetTicks();
  if (scr_overlay_hook) scr_overlay_hook();
  if (W.ren == NULL || S.back == NULL) return;
  if (S.full) memset(g_style, 0, sizeof(g_style));	/* the theme may have changed */
  picture_size();
  for (y = 0; y < S.rows; y++) {
    ECell *b = &S.back[(size_t)y * (size_t)S.cols];
    if (!S.full && memcmp(b, &S.front[(size_t)y * (size_t)S.cols], row) == 0) continue;
    paint_row(y);
    if (y < y0) y0 = y;
    y1 = y;
  }
  memcpy(S.front, S.back, row * (size_t)S.rows);
  S.full = 0;
  if (y1 >= y0) {
    SDL_Rect r;
    r.x = 0;
    r.y = y0 * W.ch;
    r.w = W.tw;
    r.h = (y1 - y0 + 1) * W.ch;
    SDL_UpdateTexture(W.tex, &r, W.fr.px + (size_t)r.y * (size_t)W.fr.w, W.fr.w * 4);
    changed = 1;
  }
  free(IMG.data);	/* pictures (sixel) are the terminal's: not shown in the window yet */
  IMG.data = NULL;
  IMG.n = 0;
  cvis = S.cy >= 0 && S.cy < S.rows && S.cx >= 0 && S.cx < S.cols;
  blink = (g_shape & 1) != 0;	/* DECSCUSR 1, 3, 5: blinking */
  if (!blink || W.ccx != S.cx || W.ccy != S.cy || changed) {	/* it moved, or typing: shown at once */
    W.blink_on = 1;
    W.blink_at = now;
  }
  else if (now - W.blink_at >= 530) {
    W.blink_on = !W.blink_on;
    W.blink_at = now;
    changed = 1;
  }
  if (cvis != W.cvis || S.cx != W.ccx || S.cy != W.ccy || g_shape != W.cshape) changed = 1;
  W.cvis = cvis;
  W.ccx = S.cx;
  W.ccy = S.cy;
  W.cshape = g_shape;
  if (strcmp(ui_title, W.title) != 0) {	/* the title bar says what mme's does */
    snprintf(W.title, sizeof(W.title), "%s", ui_title);
    SDL_SetWindowTitle(W.win, W.title[0] ? W.title : "mme");
  }
  if (g_ptr != W.ptr_now && g_ptr >= 0 && g_ptr < 3 && W.ptr[g_ptr]) {
    SDL_SetCursor(W.ptr[g_ptr]);
    W.ptr_now = g_ptr;
  }
  if (!changed && !W.dirty && W.shown) return;
  W.dirty = 0;
  W.shown = 1;
  {
    uint32_t bgc = ui_color(C_EDITOR_BG);
    SDL_SetRenderDrawColor(W.ren, (Uint8)(bgc >> 16), (Uint8)(bgc >> 8), (Uint8)bgc, 255);
  }
  SDL_RenderClear(W.ren);	/* the strip past the last cell, when the window is not a whole number of them */
  {
    SDL_Rect d;
    d.x = d.y = 0;
    d.w = W.tw;
    d.h = W.th;
    SDL_RenderCopy(W.ren, W.tex, NULL, &d);
  }
  if (cvis && W.blink_on) {	/* the caret: a bar, a block or a line, like editor.cursorStyle */
    SDL_Rect c;
    int cc = caret_color(), shape = (g_shape + 1) / 2;	/* 1 block, 2 underline, 3 bar */
    c.x = S.cx * W.cw;
    c.y = S.cy * W.ch;
    if (shape == 1) {
      c.w = W.cw;
      c.h = W.ch;
    }
    else if (shape == 2) {
      c.w = W.cw;
      c.h = W.scale >= 1.5f ? 3 : 2;
      c.y += W.ch - c.h;
    }
    else {
      c.w = W.scale >= 1.5f ? 3 : 2;
      c.h = W.ch;
    }
    SDL_SetRenderDrawBlendMode(W.ren, shape == 1 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(W.ren, (Uint8)(cc >> 16), (Uint8)(cc >> 8), (Uint8)cc, shape == 1 ? 140 : 255);
    SDL_RenderFillRect(W.ren, &c);
  }
  SDL_RenderPresent(W.ren);
}

/* }================================================================== */


/*
** {==================================================================
** The font: mmc-term's tfont.c, as the settings say (editor.fontFamily,
** editor.fontSize, VS Code's)
** ===================================================================
*/

static void font_metrics (void) {
  W.cw = font_cell_w();
  W.ch = font_cell_h();
  W.ascent = font_ascent();
  if (W.cw < 1) W.cw = 1;
  if (W.ch < 1) W.ch = 1;
}


/*
** The folder the fonts come with (tfont.c keeps one): fonts\ next to the
** program, else usr\shareonts when it is in an mmc shell's usrin
*/
static void font_dirs (void) {
  char *exe = os_exe_path(NULL), *dir, *up, *share, *fonts;
  OsStat st;
  if (exe == NULL) return;
  dir = path_dirname(exe);
  fonts = path_join(dir, "fonts");
  if (os_stat(fonts, &st) != 0 || !st.exists || !st.is_dir) {
    free(fonts);
    up = path_dirname(dir);
    share = path_join(up, "share");
    fonts = path_join(share, "fonts");
    free(share);
    free(up);
  }
  font_add_dir(fonts);
  free(fonts);
  free(dir);
  free(exe);
}


static int font_setup (void) {
  Config c;
  const char *fam = json_str(settings_get("editor\\.fontFamily"), "");
  double size = json_num(settings_get("editor\\.fontSize"), 14);
  size_t n = 0;
  memset(&c, 0, sizeof(c));
  c.smoothing = SMOOTH_CLEARTYPE;	/* Windows: GDI's ClearType, like every other Windows program */
  c.ligatures = 0;	/* one character a cell: mme places each itself */
  while (*fam == ' ' || *fam == '\'' || *fam == '"') fam++;	/* VS Code's list: its first family */
  while (fam[n] && fam[n] != ',' && fam[n] != '\'' && fam[n] != '"' && n + 1 < sizeof(c.font)) n++;
  memcpy(c.font, fam, n);
  c.font[n] = '\0';
  font_dirs();
  if (font_init(&c) != 0) return -1;
  if (size < 6 || size > 100) size = 14;
  W.px0 = W.px = (float)size * W.scale;	/* VS Code's pixels at 96 DPI, as many more as the screen's DPI */
  font_set_px(W.px);
  font_metrics();
  return 0;
}


/* Zoom In / Out / Reset (VS Code's Ctrl+= Ctrl+- Ctrl+0): the font is ours here */
void term_font (int delta) {
  if (delta > 0) W.px *= 1.1f;
  else if (delta < 0) W.px /= 1.1f;
  else W.px = W.px0;
  if (W.px < 6.0f) W.px = 6.0f;
  if (W.px > 200.0f) W.px = 200.0f;
  font_set_px(W.px);
  font_metrics();
  S.full = 1;
}

/* }================================================================== */


/*
** {==================================================================
** The terminal's calls, answered by the window
** ===================================================================
*/

/* what mme sends a terminal: only the clipboard (OSC 52) means something to a window */
static int b64 (int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}


void term_write (const char *s, size_t n) {
  const char *p = s, *end = s + n;
  if (n > 7 && memcmp(s, "\033]52;", 5) == 0) {	/* ESC ] 52 ; c ; <base64> BEL */
    Buf out;
    unsigned v = 0;
    int bits = 0;
    p = memchr(s + 5, ';', n - 5);
    if (p == NULL) return;
    buf_init(&out);
    for (p++; p < end && *p != 7 && *p != 27; p++) {
      int d = b64((unsigned char)*p);
      if (d < 0) continue;
      v = (v << 6) | (unsigned)d;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        buf_putc(&out, (char)((v >> bits) & 0xFF));
      }
    }
    buf_putc(&out, '\0');
    SDL_SetClipboardText(out.s);
    buf_free(&out);
  }
}


void term_ask_pixels (void) {
}


int term_cell_px (int *w, int *h) {	/* pictures are not shown in the window yet: none */
  (void)w;
  (void)h;
  return 0;
}


void term_size (int *cols, int *rows) {
  int w = 0, h = 0;
  if (W.ren) SDL_GetRendererOutputSize(W.ren, &w, &h);
  *cols = w / (W.cw > 0 ? W.cw : 8);
  *rows = h / (W.ch > 0 ? W.ch : 16);
  if (*cols < 20) *cols = 20;
  if (*rows < 6) *rows = 6;
}


/*
** The icon of a build without mme.rc in it (tcc cannot compile it): mme.ico
** next to the program, given to the window (WM_SETICON) big and small.
*/
static void window_icon (void) {
#ifdef _WIN32
  SDL_SysWMinfo wm;
  HMODULE self = GetModuleHandleW(NULL);
  char *exe, *dir, *ico;
  wchar_t *w;
  int k;
  if (FindResourceW(self, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14)) != NULL) return;	/* RT_GROUP_ICON: it has one */
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(W.win, &wm) || (exe = os_exe_path(NULL)) == NULL) return;
  dir = path_dirname(exe);
  ico = path_join(dir, "mme.ico");
  k = MultiByteToWideChar(CP_UTF8, 0, ico, -1, NULL, 0);
  w = (wchar_t *)xmalloc((size_t)(k > 0 ? k : 1) * sizeof(wchar_t));
  if (k > 0 && MultiByteToWideChar(CP_UTF8, 0, ico, -1, w, k) > 0) {
    HANDLE big = LoadImageW(NULL, w, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
    HANDLE small = LoadImageW(NULL, w, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                              LR_LOADFROMFILE);
    if (big) SendMessageW(wm.info.win.window, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small) SendMessageW(wm.info.win.window, WM_SETICON, ICON_SMALL, (LPARAM)small);
  }
  free(w);
  free(ico);
  free(dir);
  free(exe);
#endif
}


int term_open (void) {
  float ddpi = 96.0f, hdpi = 96.0f, vdpi = 96.0f;
  int w, h;
  SDL_SetMainReady();
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");	/* sharp on a high DPI screen */
  SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
  SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");	/* mme.rc's icon, in the title bar and the taskbar */
  SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return -1;
  if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) != 0 || hdpi < 48.0f) hdpi = 96.0f;
  W.scale = hdpi / 96.0f;
  if (font_setup() != 0) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "mme", "No monospace font was found.", NULL);
    return -1;
  }
  w = 120 * W.cw;	/* 120 x 36 cells to start, as much as the screen has */
  h = 36 * W.ch;
  {
    SDL_Rect u;
    if (SDL_GetDisplayUsableBounds(0, &u) == 0) {
      if (w > u.w * 9 / 10) w = u.w * 9 / 10;
      if (h > u.h * 9 / 10) h = u.h * 9 / 10;
    }
  }
  W.win = SDL_CreateWindow("mme", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (W.win == NULL) return -1;
  W.ren = SDL_CreateRenderer(W.win, -1, SDL_RENDERER_SOFTWARE);	/* the picture is painted here already: no GPU driver to load (tens of MB) */
  if (W.ren == NULL) W.ren = SDL_CreateRenderer(W.win, -1, 0);
  if (W.ren == NULL) return -1;
  window_icon();
  W.ptr[PTR_DEFAULT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  W.ptr[PTR_TEXT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
  W.ptr[PTR_POINTER] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
  W.ptr_now = -1;
  W.ccx = W.ccy = -1;
  SDL_StartTextInput();
  return 0;
}


void term_close (void) {
  int i;
  if (W.win == NULL) return;
  SDL_StopTextInput();
  for (i = 0; i < 3; i++)
    if (W.ptr[i]) SDL_FreeCursor(W.ptr[i]);
  if (W.tex) SDL_DestroyTexture(W.tex);
  if (W.ren) SDL_DestroyRenderer(W.ren);
  SDL_DestroyWindow(W.win);
  W.win = NULL;
  W.ren = NULL;
  W.tex = NULL;
  SDL_Quit();
}

/* }================================================================== */


/*
** {==================================================================
** Keys, the mouse, pastes: SDL's events as mme's keys (the same codes
** eterm.c makes of a terminal's, with the kitty keyboard's modifiers)
** ===================================================================
*/

#define QMAX	64

static int g_q[QMAX], g_qn;	/* keys made ready: a text input can be several */
static Mouse g_qm[QMAX];
static char *g_paste;	/* what K_PASTE hands to term_paste */
static int g_skip_text;	/* the key went as a shortcut: its text (if SDL sends one) does not */
static int g_mcol = -1, g_mrow = -1, g_mbutton = -1;	/* the mouse: its cell, the button held */


static void push (int k) {
  if (g_qn < QMAX) {
    g_qm[g_qn] = term_mouse;
    g_q[g_qn++] = k;
  }
}


static int km_of (Uint16 m) {
  int k = 0;
  if (m & KMOD_SHIFT) k |= KM_SHIFT;
  if (m & KMOD_CTRL) k |= KM_CTRL;
  if (m & KMOD_LALT) k |= KM_ALT;	/* right Alt is AltGr: its characters come as text */
#ifndef _WIN32
  if (m & KMOD_RALT) k |= KM_ALT;
#endif
  return k;
}


/* eterm.c's kitty_key: Ctrl+S stays CTRL('s'), Ctrl+Shift+P is 'p' | KM_CTRL | KM_SHIFT */
static int kitty_key (int code, int k) {
  int ctrl = k & KM_CTRL, shift = k & KM_SHIFT;
  if (code >= 'A' && code <= 'Z') code += 32;
  if (ctrl && !shift && code >= 'a' && code <= 'z' && code != 'm' && code != 'i') return CTRL(code) | (k & KM_ALT);
  if (!ctrl && !(k & KM_ALT)) return (shift && code >= 'a' && code <= 'z') ? code - 32 : code;
  return code | k;
}


static void paste_clipboard (void) {
  char *t = SDL_GetClipboardText();
  free(g_paste);
  g_paste = xstrdup(t ? t : "");
  SDL_free(t);
  push(K_PASTE);
}


static void on_keydown (const SDL_KeyboardEvent *e) {
  SDL_Keycode sym = e->keysym.sym;
  int k = km_of(e->keysym.mod), code = -1;
  g_skip_text = 0;
  switch (sym) {
    case SDLK_UP: case SDLK_KP_8: if (sym == SDLK_UP || !(e->keysym.mod & KMOD_NUM)) code = K_UP; break;
    case SDLK_DOWN: case SDLK_KP_2: if (sym == SDLK_DOWN || !(e->keysym.mod & KMOD_NUM)) code = K_DOWN; break;
    case SDLK_LEFT: case SDLK_KP_4: if (sym == SDLK_LEFT || !(e->keysym.mod & KMOD_NUM)) code = K_LEFT; break;
    case SDLK_RIGHT: case SDLK_KP_6: if (sym == SDLK_RIGHT || !(e->keysym.mod & KMOD_NUM)) code = K_RIGHT; break;
    case SDLK_HOME: case SDLK_KP_7: if (sym == SDLK_HOME || !(e->keysym.mod & KMOD_NUM)) code = K_HOME; break;
    case SDLK_END: case SDLK_KP_1: if (sym == SDLK_END || !(e->keysym.mod & KMOD_NUM)) code = K_END; break;
    case SDLK_PAGEUP: case SDLK_KP_9: if (sym == SDLK_PAGEUP || !(e->keysym.mod & KMOD_NUM)) code = K_PGUP; break;
    case SDLK_PAGEDOWN: case SDLK_KP_3: if (sym == SDLK_PAGEDOWN || !(e->keysym.mod & KMOD_NUM)) code = K_PGDN; break;
    case SDLK_INSERT: case SDLK_KP_0: if (sym == SDLK_INSERT || !(e->keysym.mod & KMOD_NUM)) code = K_INS; break;
    case SDLK_DELETE: case SDLK_KP_PERIOD: if (sym == SDLK_DELETE || !(e->keysym.mod & KMOD_NUM)) code = K_DEL; break;
    case SDLK_F1: code = K_F1; break;
    case SDLK_F2: code = K_F2; break;
    case SDLK_F3: code = K_F3; break;
    case SDLK_F4: code = K_F4; break;
    case SDLK_F5: code = K_F5; break;
    case SDLK_F6: code = K_F6; break;
    case SDLK_F7: code = K_F7; break;
    case SDLK_F8: code = K_F8; break;
    case SDLK_F9: code = K_F9; break;
    case SDLK_F10: code = K_F10; break;
    case SDLK_F11: code = K_F11; break;
    case SDLK_F12: code = K_F12; break;
    case SDLK_RETURN: case SDLK_KP_ENTER: code = K_ENTER; break;
    case SDLK_TAB: code = K_TAB; break;
    case SDLK_BACKSPACE: code = K_BS; break;
    case SDLK_ESCAPE: code = K_ESC; break;
  }
  if (code == K_INS && (k & KM_SHIFT) && !(k & (KM_CTRL | KM_ALT))) {	/* Shift+Insert: paste */
    paste_clipboard();
    g_skip_text = 1;
    return;
  }
  if (code >= 0) {
    push(code | k);
    g_skip_text = 1;
    return;
  }
  if (sym < 32 || sym >= 127 || !(k & (KM_CTRL | KM_ALT))) return;	/* a character: it comes as text */
  if (sym == 'v' && k == KM_CTRL) paste_clipboard();	/* Ctrl+V: the system's clipboard, as a terminal pastes it */
  else push(kitty_key((int)sym, k));
  g_skip_text = 1;
}


static void on_text (const char *s) {
  size_t n = strlen(s), i = 0;
  if (g_skip_text) {	/* Ctrl+Alt+x on some layouts sends both */
    g_skip_text = 0;
    return;
  }
  while (i < n) {
    size_t len;
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    if (len == 0) break;
    if (cp >= 32 && cp != 127) push((int)cp);
    i += len;
  }
}


static void mouse_at (int x, int y) {
  int ww = 0, wh = 0, pw = 0, ph = 0;
  SDL_GetWindowSize(W.win, &ww, &wh);	/* macOS's Retina: the mouse in points, the picture in pixels */
  if (W.ren) SDL_GetRendererOutputSize(W.ren, &pw, &ph);
  if (ww > 0 && pw > 0 && pw != ww) {
    x = x * pw / ww;
    y = y * ph / wh;
  }
  term_mouse.x = x / (W.cw > 0 ? W.cw : 1);
  term_mouse.y = y / (W.ch > 0 ? W.ch : 1);
  if (term_mouse.x >= S.cols) term_mouse.x = S.cols - 1;
  if (term_mouse.y >= S.rows) term_mouse.y = S.rows - 1;
}


/* an SDL event as keys in the queue; 1: the window must be shown again */
static void on_event (const SDL_Event *e) {
  switch (e->type) {
    case SDL_QUIT:
      push(CTRL('q'));	/* the window's x: File > Exit, which asks about unsaved files */
      break;
    case SDL_WINDOWEVENT:
      if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_RESIZED) S.full = 1;
      W.dirty = 1;
      break;
    case SDL_KEYDOWN: on_keydown(&e->key); break;
    case SDL_TEXTINPUT: on_text(e->text.text); break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
      int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_MIDDLE ? 1 :
              e->button.button == SDL_BUTTON_RIGHT ? 2 : -1;
      if (b < 0) break;
      memset(&term_mouse, 0, sizeof(term_mouse));
      mouse_at(e->button.x, e->button.y);
      term_mouse.button = b;
      term_mouse.press = e->type == SDL_MOUSEBUTTONDOWN;
      term_mouse.mods = km_of(SDL_GetModState());
      g_mbutton = term_mouse.press ? b : -1;
      g_mcol = term_mouse.x;
      g_mrow = term_mouse.y;
      push(K_MOUSE);
      break;
    }
    case SDL_MOUSEMOTION:	/* a drag with the button held, else a move (hover): once a cell */
      memset(&term_mouse, 0, sizeof(term_mouse));
      mouse_at(e->motion.x, e->motion.y);
      if (term_mouse.x == g_mcol && term_mouse.y == g_mrow) break;
      g_mcol = term_mouse.x;
      g_mrow = term_mouse.y;
      term_mouse.drag = 1;
      term_mouse.press = 1;
      term_mouse.button = g_mbutton >= 0 ? g_mbutton : 3;
      term_mouse.mods = km_of(SDL_GetModState());
      push(K_MOUSE);
      break;
    case SDL_MOUSEWHEEL: {
      int mx, my;
      if (e->wheel.y == 0) break;
      SDL_GetMouseState(&mx, &my);
      memset(&term_mouse, 0, sizeof(term_mouse));
      mouse_at(mx, my);
      term_mouse.wheel = e->wheel.y > 0 ? -1 : 1;	/* away from you: up */
      term_mouse.button = 3;
      term_mouse.mods = km_of(SDL_GetModState());
      push(K_MOUSE);
      break;
    }
  }
}


void (*term_key_hook) (int k);	/* every key read, whoever reads it (screencast mode) */

/* the next key, waiting ms for it (-1: as long as it takes); K_NONE: none came */
int term_key (int ms) {
  Uint32 start = SDL_GetTicks();
  int k;
  for (;;) {
    SDL_Event e;
    int wait, got;
    if (g_qn > 0) {
      k = g_q[0];
      term_mouse = g_qm[0];
      memmove(g_q, g_q + 1, (size_t)(g_qn - 1) * sizeof(int));
      memmove(g_qm, g_qm + 1, (size_t)(g_qn - 1) * sizeof(Mouse));
      g_qn--;
      if (term_key_hook) term_key_hook(k);
      return k;
    }
    if (ms < 0) wait = -1;
    else {
      Uint32 gone = SDL_GetTicks() - start;
      if ((int)gone >= ms) return K_NONE;
      wait = ms - (int)gone;
    }
    got = wait < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, wait);
    if (!got) {
      if (ms >= 0) return K_NONE;
      continue;
    }
    on_event(&e);
    if (W.dirty || S.full) {	/* resized, uncovered: the editor draws again before the next key */
      if (g_qn == 0) return K_NONE;
    }
  }
}


/* after K_PASTE: the text, CR LF as one newline */
void term_paste (Buf *b) {
  const char *p = g_paste ? g_paste : "";
  for (; *p; p++) {
    if (*p == '\r') {
      buf_putc(b, '\n');
      if (p[1] == '\n') p++;
    }
    else buf_putc(b, *p);
  }
  free(g_paste);
  g_paste = NULL;
}

/* }================================================================== */


#ifdef _WIN32
/* a window program (no console behind it) starts here; mme.c's main reads its own arguments (os_args) */
int main (int argc, char **argv);

int WINAPI WinMain (HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  (void)inst;
  (void)prev;
  (void)cmd;
  (void)show;
  return main(__argc, __argv);
}
#endif
