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
** underlines, its powerline shapes), the fonts are mmc-term's tfont.c
** (stb_truetype, and GDI's ClearType on Windows), so the window looks like
** mme in mmc-term. The image preview's sixel is decoded and painted too.
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

static int g_precise;	/* the wheel's preciseX / preciseY are there: SDL 2.0.18 and later */


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
  int focused;	/* the window has the keyboard: else the caret is a hollow box */
  int ccx, ccy;	/* where the caret was last flush (it moved: shown at once) */
  int car_on, car_x, car_y, car_shape, car_focus;	/* the caret painted into the picture now */
  int up0, up1;	/* the rows of the picture painted since it was last shown */
  int use_surface;	/* shown through SDL's window surface (GDI's own bitmap on Windows), only the rows that
                   changed: no renderer, no texture (macOS: the renderer, for Retina's pixels) */
  SDL_Surface *fsurf;	/* the frame as a surface, its rows copied from */
  SDL_Surface *last;	/* the window surface shown into last (made again when the window's size changes) */
  unsigned char *rowdirty;	/* the rows painted since they were last shown */
  int nrowdirty;
  SDL_Rect *rects;
  char title[512];
  SDL_Cursor *ptr[3];
  int ptr_now;
  char font_fam[128];	/* editor.fontFamily and editor.fontSize, as the font was made from them */
  double font_size;
  long long font_seen;	/* when the settings were looked at last */
  float wheel_x, wheel_y;	/* a touchpad's scrolling, less than a line so far */
  int ime_x, ime_y;	/* where the IME's window was told the caret is */
  int custom;	/* window.titleBarStyle "custom" (VS Code's default): mme's menu bar is the title bar */
  int borderless;	/* no system frame now (custom, and the menu bar is shown) */
  int tb_hover, tb_press;	/* the title bar's button under the mouse, held down; -1 none */
} W;


/* the pictures shown (the image preview's, a notebook's: sixel from eimage.c), decoded, where they are */
typedef struct Pic {
  char *src;	/* the sixel it was decoded from */
  size_t n;
  uint32_t *px;	/* 0xAARRGGBB, alpha 0: not painted */
  int w, h;
  int x, y, cols, rows;	/* in cells */
} Pic;

static Pic IM[MAX_IMG];
static int NIM;


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


/* a glyph at pen (x, y), only its pixels inside clip0 .. clip1 across and top .. bottom down */
static void blit_glyph (Frame *f, const Glyph *g, int x, int y, uint32_t fg, uint32_t bg, int clip0, int clip1,
                        int top, int bottom) {
  const unsigned char *lut;
  int i, j;
  if (g == NULL || g->bm == NULL) return;
  if (!lut_ready) make_luts();
  lut = brightness(fg) > brightness(bg) ? lut_light : lut_dark;
  if (clip0 < 0) clip0 = 0;
  if (clip1 > f->w) clip1 = f->w;
  if (top < 0) top = 0;
  if (bottom > f->h) bottom = f->h;
  for (j = 0; j < g->h; j++) {
    int py = y + g->yoff + j;
    if (py < top || py >= bottom) continue;
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


/*
** The round corners (U+256D..2570, the Chat box's, the git graph's): a
** quarter of a circle from the middle of one side to the middle of the
** other, its edge smoothed, and the straight rest of the two arms
*/
static void draw_arc (Frame *f, uint32_t cp, int x, int y, int w, int h, int t, uint32_t fg) {
  int sx = cp == 0x256D || cp == 0x2570 ? 1 : -1;	/* the arms go right (1) or left */
  int sy = cp == 0x256D || cp == 0x256E ? 1 : -1;	/* down (1) or up */
  int o = t / 2, i, j;
  double lx = x + w / 2 - o + t / 2.0, ly = y + h / 2 - o + t / 2.0;	/* the middle of the lines */
  double r = (w < h ? w : h) / 2.0, ax = lx + sx * r, ay = ly + sy * r;	/* the circle's middle */
  for (j = 0; j < h; j++)
    for (i = 0; i < w; i++) {
      double px = x + i + 0.5, py = y + j + 0.5, d, a;
      if ((px - ax) * sx > 0.5 || (py - ay) * sy > 0.5) continue;	/* the quarter toward the corner */
      d = sqrt((px - ax) * (px - ax) + (py - ay) * (py - ay));
      a = t / 2.0 + 0.5 - fabs(d - r);
      if (a <= 0.0 || x + i < 0 || x + i >= f->w || y + j < 0 || y + j >= f->h) continue;
      {
        uint32_t *p = &f->px[(size_t)(y + j) * (size_t)f->w + (size_t)(x + i)];
        *p = mix(*p, fg, a >= 1.0 ? 255 : (int)(a * 255.0));
      }
    }
  {	/* the straight parts, from where the circle ends to the cell's edge */
    int ex = (int)(ax + 0.5), ey = (int)(ay + 0.5);
    if (sx > 0) fill(f, ex, y + h / 2 - o, x + w - ex, t, fg);
    else fill(f, x, y + h / 2 - o, ex - x, t, fg);
    if (sy > 0) fill(f, x + w / 2 - o, ey, t, y + h - ey, fg);
    else fill(f, x + w / 2 - o, y, t, ey - y, fg);
  }
}


static int draw_box (Frame *f, uint32_t cp, int x, int y, int w, int h, uint32_t fg) {
  const char *arms = box_arms[cp - 0x2500];
  int a[4], i, t1 = (w + 4) / 8, t2, d, cx = x + w / 2, cy = y + h / 2, dbl_h, dbl_v, active;
  if (arms[0] == '-') return 0;	/* diagonals: the font's */
  if (t1 < 1) t1 = 1;
  if (cp >= 0x256D && cp <= 0x2570) {
    draw_arc(f, cp, x, y, w, h, t1, fg);
    return 1;
  }
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


/*
** Powerline separators (U+E0B0..E0BF), a shell prompt's, as geometry: they
** fill their cell exactly, so the colored parts join without seams (tdraw.c)
*/
static double seg_dist (double px, double py, double ax, double ay, double bx, double by) {
  double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
  double t = len2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;	/* a point (a circle's middle): 0 */
  if (t < 0.0) t = 0.0;
  else if (t > 1.0) t = 1.0;
  dx = ax + t * dx - px;
  dy = ay + t * dy - py;
  return sqrt(dx * dx + dy * dy);
}


static int in_ellipse (double x, double y, double cx, double m, double rx, double ry) {
  double a, b;
  if (rx <= 0.0 || ry <= 0.0) return 0;
  a = (x - cx) / rx;
  b = (y - m) / ry;
  return a * a + b * b <= 1.0;
}


static int pl_inside (uint32_t cp, double x, double y, double w, double h, double t) {
  double m = h / 2.0, u = x / w, v = y / h, r = t / 2.0;
  switch (cp) {
    case 0xE0B0: return u <= 1.0 - fabs(2.0 * v - 1.0);
    case 0xE0B2: return u >= fabs(2.0 * v - 1.0);
    case 0xE0B1: return seg_dist(x, y, 0, 0, w, m) <= r || seg_dist(x, y, w, m, 0, h) <= r;
    case 0xE0B3: return seg_dist(x, y, w, 0, 0, m) <= r || seg_dist(x, y, 0, m, w, h) <= r;
    case 0xE0B4: return in_ellipse(x, y, 0, m, w, m);
    case 0xE0B6: return in_ellipse(x, y, w, m, w, m);
    case 0xE0B5: return in_ellipse(x, y, 0, m, w, m) && !in_ellipse(x, y, 0, m, w - t, m - t);
    case 0xE0B7: return in_ellipse(x, y, w, m, w, m) && !in_ellipse(x, y, w, m, w - t, m - t);
    case 0xE0B8: return u <= v;
    case 0xE0BA: return u >= 1.0 - v;
    case 0xE0BC: return u <= 1.0 - v;
    case 0xE0BE: return u >= v;
    case 0xE0B9: case 0xE0BF: return seg_dist(x, y, 0, 0, w, h) <= r;
    case 0xE0BB: case 0xE0BD: return seg_dist(x, y, w, 0, 0, h) <= r;
  }
  return 0;
}


static int draw_powerline (Frame *f, uint32_t cp, int x, int y, int w, int h, uint32_t fg) {
  double t = (double)h / 14.0;
  int i, j, si, sj;
  if (t < 1.0) t = 1.0;
  for (j = 0; j < h; j++) {
    if (y + j < 0 || y + j >= f->h) continue;
    for (i = 0; i < w; i++) {
      int n = 0;
      uint32_t *p;
      if (x + i < 0 || x + i >= f->w) continue;
      for (sj = 0; sj < 4; sj++)	/* 4 x 4 samples a pixel: smooth edges */
        for (si = 0; si < 4; si++)
          n += pl_inside(cp, (double)i + (si + 0.5) / 4.0, (double)j + (sj + 0.5) / 4.0, (double)w, (double)h, t);
      if (n == 0) continue;
      p = &f->px[(size_t)(y + j) * (size_t)f->w + (size_t)(x + i)];
      *p = mix(*p, fg, n * 255 / 16);
    }
  }
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


/* a cell's colors and attributes */
static void cell_colors (const ECell *c, uint32_t *fg, uint32_t *bg, uint32_t *at, uint32_t *ul) {
  if (c->st == S_RGB) {
    *fg = c->fg;
    *bg = c->bg;
    *at = c->at;
  }
  else style_of(c->st, fg, bg, at);
  *ul = (*at & RGB_CURLY) ? c->ul : *fg;
  if (*at & RGB_DIM) *fg = mix(*bg, *fg, 150);
}


/*
** A cell's character (a glyph, box drawing, a block, a powerline shape)
** and its lines, over the background already there. A glyph may reach
** half a cell past its own on either side (italics, ClearType's edges),
** as in a terminal (over: 1), but never above or below its row.
*/
static void fill_row (Frame *f, int x, int y, int w, int h, uint32_t color, int top) {	/* inside the row at top */
  if (y + h > top + W.ch) h = top + W.ch - y;
  if (y < top) {
    h -= top - y;
    y = top;
  }
  if (h > 0) fill(f, x, y, w, h, color);
}


static void paint_fg (const ECell *c, int col, int row, int wide, uint32_t fg, uint32_t bg, uint32_t at, uint32_t ul,
                      int over) {
  Frame *f = &W.fr;
  int px = col * W.cw, py = row * W.ch, w = W.cw * (wide ? 2 : 1), line = (W.ch + 8) / 16, o = over ? W.cw / 2 : 0;
  uint32_t ch = c->ch;
  if (line < 1) line = 1;
  if (ch > ' ') {
    int drawn = 0;
    if (ch >= 0x2500 && ch <= 0x257F) drawn = draw_box(f, ch, px, py, w, W.ch, fg);
    else if (ch >= 0x2580 && ch <= 0x259F) drawn = draw_block(f, ch, px, py, w, W.ch, fg);
    else if (ch >= 0xE0B0 && ch <= 0xE0BF) drawn = draw_powerline(f, ch, px, py, w, W.ch, fg);
    if (!drawn)
      blit_glyph(f, font_glyph(ch, (at & RGB_BOLD) != 0, (at & RGB_ITALIC) != 0, brightness(fg) < brightness(bg)),
                 px, py + W.ascent, fg, bg, px - o, px + w + o, py, py + W.ch);
  }
  if (at & RGB_CURLY) {	/* a squiggle: a wave from cell to cell (tdraw.c) */
    int amp = line + 1, period = W.cw > 4 ? W.cw : 4, x, y = py + W.ascent + line + 1;
    for (x = 0; x < w; x++) {
      double t = (double)((px + x) % period) / (double)period;
      int dy = (int)((double)amp * (1.0 - cos(t * 6.283185307179586)) / 2.0 + 0.5);
      fill_row(f, px + x, y - 1 + dy, 1, line, ul, py);
    }
  }
  else if (at & RGB_UNDER) fill_row(f, px, py + W.ascent + line + 1, w, line, ul, py);
  if (at & RGB_STRIKE) fill_row(f, px, py + (W.ascent * 2) / 3, w, line, fg, py);
}


static void mark (int y) {
  if (y < W.up0) W.up0 = y;
  if (y > W.up1) W.up1 = y;
  if (y >= 0 && y < W.nrowdirty) W.rowdirty[y] = 1;
}


/* the window's size in pixels: the renderer's (macOS's Retina has more than its points), else the window's */
static void out_size (int *w, int *h) {
  *w = *h = 0;
  if (W.ren) SDL_GetRendererOutputSize(W.ren, w, h);
  else if (W.win) SDL_GetWindowSize(W.win, w, h);
}


/*
** {==================================================================
** The title bar (window.titleBarStyle "custom", VS Code's default on
** Windows): no system frame; mme's menu bar is the title bar - its empty
** parts move the window (a double-click maximizes it, dragged to the
** screen's top it snaps), the window's edges resize it, and its minimize,
** maximize and close buttons are drawn here at its right end
** ===================================================================
*/

static int tb_width (void) {	/* a button: VS Code's 46 pixels (at 96 DPI, zoomed), as high as a row */
  int w = (int)(46.0f * W.scale * (W.px0 > 0.0f ? W.px / W.px0 : 1.0f) + 0.5f);
  return w > W.ch ? w : W.ch;
}


static int tb_left (void) {
  return W.fr.w - 3 * tb_width();
}


/* the button at pixel (x, y) of the picture: 0 minimize, 1 maximize, 2 close; -1 none */
static int tb_button (int x, int y) {
  if (!W.borderless || y < 0 || y >= W.ch || x < tb_left() || x >= W.fr.w) return -1;
  x = (x - tb_left()) / tb_width();
  return x > 2 ? 2 : x;
}


static double seg_dist (double px, double py, double ax, double ay, double bx, double by);


static void aa_line (Frame *f, double x0, double y0, double x1, double y1, double t, uint32_t c) {
  int i, j, l = (int)floor((x0 < x1 ? x0 : x1) - t), r = (int)ceil((x0 > x1 ? x0 : x1) + t);
  int top = (int)floor((y0 < y1 ? y0 : y1) - t), bot = (int)ceil((y0 > y1 ? y0 : y1) + t);
  for (j = top; j <= bot; j++)
    for (i = l; i <= r; i++) {
      double a = t / 2.0 + 0.5 - seg_dist(i + 0.5, j + 0.5, x0, y0, x1, y1);
      uint32_t *p;
      if (a <= 0.0 || i < 0 || j < 0 || i >= f->w || j >= f->h) continue;
      p = &f->px[(size_t)j * (size_t)f->w + (size_t)i];
      *p = mix(*p, c, a >= 1.0 ? 255 : (int)(a * 255.0));
    }
}


static void box_outline (Frame *f, int x, int y, int w, int h, int t, uint32_t c) {
  fill(f, x, y, w, t, c);
  fill(f, x, y + h - t, w, t, c);
  fill(f, x, y, t, h, c);
  fill(f, x + w - t, y, t, h, c);
}


/* the three buttons, in the colors of the row's right end (the menu bar's); row0: the grid's row 0 */
static void tb_paint (const ECell *row0) {
  Frame *f = &W.fr;
  int bw = tb_width(), x0 = tb_left(), i, h = W.ch / 3, th = (int)(W.scale + 0.5f);
  int maxed = (SDL_GetWindowFlags(W.win) & SDL_WINDOW_MAXIMIZED) != 0;
  uint32_t fg, bg, at, ul;
  cell_colors(&row0[S.cols - 1], &fg, &bg, &at, &ul);
  if (h < 4) h = 4;
  h /= 2;
  if (th < 1) th = 1;
  for (i = 0; i < 3; i++) {
    int bx = x0 + i * bw, cx = bx + bw / 2, cy = W.ch / 2;
    uint32_t b = bg, ic = fg;
    if (W.tb_hover == i && i == 2) {	/* close: red, as Windows' own */
      b = W.tb_press == i ? 0xF1707Au : 0xE81123u;
      ic = 0xFFFFFFu;
    }
    else if (W.tb_hover == i) b = mix(bg, fg, W.tb_press == i ? 60 : 32);
    fill(f, bx, 0, i == 2 ? f->w - bx : bw, W.ch, b);
    if (i == 0) fill(f, cx - h, cy, 2 * h + 1, th, ic);	/* _ */
    else if (i == 1 && !maxed) box_outline(f, cx - h, cy - h, 2 * h + 1, 2 * h + 1, th, ic);	/* a square */
    else if (i == 1) {	/* restore: two squares, one behind the other */
      int d = h / 2 > 1 ? h / 2 : 2;
      box_outline(f, cx - h, cy - h + d, 2 * h + 1 - d, 2 * h + 1 - d, th, ic);
      fill(f, cx - h + d, cy - h, 2 * h + 1 - d, th, ic);
      fill(f, cx + h + 1 - th, cy - h, th, 2 * h + 1 - d, ic);
    }
    else {	/* x */
      aa_line(f, cx - h, cy - h, cx + h + 1, cy + h + 1, th, ic);
      aa_line(f, cx + h + 1, cy - h, cx - h, cy + h + 1, th, ic);
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** Badges (RGB_BADGE: the activity bar's counts): a terminal can only give
** them whole cells; here they are VS Code's - a small round pill under the
** icon's corner, its number smaller than the text, not bold
** ===================================================================
*/

static int is_badge (const ECell *c) {
  return c->st == S_RGB && (c->at & RGB_BADGE) != 0;
}


/* a glyph drawn smaller (s < 1), 3 x 3 samples a pixel; (x, y): its pen, on the baseline */
static void blit_glyph_small (Frame *f, const Glyph *g, double x, double y, double s, uint32_t fg) {
  int i, j, x0, x1, y0, y1;
  if (g == NULL || g->bm == NULL || g->lcd == 3) return;
  x0 = (int)floor(x + g->xoff * s);
  x1 = (int)ceil(x + (g->xoff + g->w) * s);
  y0 = (int)floor(y + g->yoff * s);
  y1 = (int)ceil(y + (g->yoff + g->h) * s);
  for (j = y0; j < y1; j++)
    for (i = x0; i < x1; i++) {
      int u, v, sum = 0;
      if (i < 0 || j < 0 || i >= f->w || j >= f->h) continue;
      for (v = 0; v < 3; v++)
        for (u = 0; u < 3; u++) {
          int sx = (int)floor((i + (u + 0.5) / 3.0 - x) / s) - g->xoff;
          int sy = (int)floor((j + (v + 0.5) / 3.0 - y) / s) - g->yoff;
          size_t at;
          if (sx < 0 || sy < 0 || sx >= g->w || sy >= g->h) continue;
          at = (size_t)sy * (size_t)g->w + (size_t)sx;
          sum += g->lcd == 1 ? (g->bm[at * 3] + g->bm[at * 3 + 1] + g->bm[at * 3 + 2]) / 3 : g->bm[at];
        }
      if (sum > 0) {
        uint32_t *p = &f->px[(size_t)j * (size_t)f->w + (size_t)i];
        *p = mix(*p, fg, sum / 9);
      }
    }
}


/* the badge of cells x .. e-1 of row y: the pill, its number in it */
static void paint_badge (const ECell *b, int x, int e, int y) {
  Frame *f = &W.fr;
  double sc = 0.72, adv = W.cw * sc, ph = W.ch * 0.78, r = ph / 2.0, tw = adv * (e - x), pw = tw + ph * 0.6;
  double left = x * W.cw - W.cw * 0.5, top = y * W.ch + 1, cy = top + r, base;
  uint32_t fg, bg, at, ul;
  int i, j, k;
  const Glyph *g0;
  cell_colors(&b[x], &fg, &bg, &at, &ul);
  if (pw < ph) pw = ph;	/* one digit: a circle */
  for (j = (int)top; j <= (int)(top + ph) + 1; j++)	/* the pill, its ends round and smooth */
    for (i = (int)left - 1; i <= (int)(left + pw) + 1; i++) {
      double d = seg_dist(i + 0.5, j + 0.5, left + r, cy, left + pw - r, cy), a = r + 0.5 - d;
      uint32_t *p;
      if (a <= 0.0 || i < 0 || j < 0 || i >= f->w || j >= f->h) continue;
      p = &f->px[(size_t)j * (size_t)f->w + (size_t)i];
      *p = mix(*p, bg, a >= 1.0 ? 255 : (int)(a * 255.0));
    }
  g0 = font_glyph('0', 0, 0, 0);	/* the digits' height: the number in the pill's middle */
  base = g0 && g0->bm ? cy - (g0->yoff + g0->h / 2.0) * sc : cy + W.ascent * sc / 2.0;
  for (k = x; k < e; k++)
    blit_glyph_small(f, font_glyph(b[k].ch, 0, 0, 0), left + (pw - tw) / 2.0 + (k - x) * adv, base, sc, fg);
}

/* }================================================================== */


/* row y of a grid (S.back, or S.front: what is shown) into the picture: every background, then the characters */
static void paint_row (int y, const ECell *grid) {
  const ECell *b = &grid[(size_t)y * (size_t)S.cols];
  int x, pass;
  for (pass = 0; pass < 2; pass++)
    for (x = 0; x < S.cols; x++) {
      uint32_t fg, bg, at, ul;
      int wide = b[x].w == 2 && x + 1 < S.cols;
      if (b[x].ch == 0 && x > 0 && b[x - 1].w == 2) continue;	/* the right half of a wide one: painted with it */
      if (is_badge(&b[x])) {	/* under the badge: the color beside it; the badge itself after */
        int k = x;
        while (k > 0 && is_badge(&b[k])) k--;
        cell_colors(&b[k], &fg, &bg, &at, &ul);
        if (pass == 0) fill(&W.fr, x * W.cw, y * W.ch, W.cw, W.ch, bg);
        continue;
      }
      cell_colors(&b[x], &fg, &bg, &at, &ul);
      if (pass == 0) fill(&W.fr, x * W.cw, y * W.ch, W.cw * (wide ? 2 : 1), W.ch, bg);
      else paint_fg(&b[x], x, y, wide, fg, bg, at, ul, 1);
    }
  for (x = 0; x < S.cols; x++)
    if (is_badge(&b[x])) {
      int e = x;
      while (e < S.cols && is_badge(&b[e])) e++;
      paint_badge(b, x, e, y);
      x = e;
    }
  {	/* the window is not a whole number of cells: the row's colors go on to its edges */
    int gx = S.cols * W.cw, gy = S.rows * W.ch;
    uint32_t fg, bg, at, ul;
    cell_colors(&b[S.cols - 1], &fg, &bg, &at, &ul);
    fill(&W.fr, gx, y * W.ch, W.fr.w - gx, W.ch, bg);
    if (y == S.rows - 1 && W.fr.h > gy) {	/* under the last row (the status bar) */
      fill(&W.fr, gx, gy, W.fr.w - gx, W.fr.h - gy, bg);
      for (x = 0; x < S.cols; x++) {
        cell_colors(&b[x], &fg, &bg, &at, &ul);
        fill(&W.fr, x * W.cw, gy, W.cw, W.fr.h - gy, bg);
      }
    }
  }
  if (y == 0 && W.borderless) tb_paint(b);	/* the title bar's buttons, over its right end */
  if (W.car_on && W.car_y == y) W.car_on = 0;	/* painted over */
  mark(y);
}


/* the picture as big as the window: a new frame (and texture), everything painted again */
static void picture_size (void) {
  int w = 0, h = 0;
  out_size(&w, &h);	/* the window, cells and the bit past them */
  if (w < S.cols * W.cw) w = S.cols * W.cw;
  if (h < S.rows * W.ch) h = S.rows * W.ch;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (W.fr.px && w == W.tw && h == W.th && W.nrowdirty == S.rows && (W.use_surface || W.tex)) return;
  if (W.tex) SDL_DestroyTexture(W.tex);
  W.tex = W.use_surface ? NULL : SDL_CreateTexture(W.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
  W.tw = w;
  W.th = h;
  if (W.fsurf) SDL_FreeSurface(W.fsurf);
  free(W.fr.px);
  W.fr.px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
  W.fr.w = w;
  W.fr.h = h;
  W.fsurf = W.use_surface ? SDL_CreateRGBSurfaceWithFormatFrom(W.fr.px, w, h, 32, w * 4, SDL_PIXELFORMAT_RGB888) : NULL;
  free(W.rowdirty);
  free(W.rects);
  W.nrowdirty = S.rows;
  W.rowdirty = (unsigned char *)calloc((size_t)S.rows + 1, 1);
  W.rects = (SDL_Rect *)calloc((size_t)S.rows + 1, sizeof(SDL_Rect));
  S.full = 1;
  W.dirty = 1;
}


/*
** {==================================================================
** The image preview's picture: eimage.c sends it as sixel, as it would to
** a terminal; here it is decoded once and painted over its cells
** ===================================================================
*/

static int sixel_num (const char **p, const char *end) {
  int v = 0;
  while (*p < end && **p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
  return v;
}


/* a sixel string as pixels (its size from the raster attributes, "1;1;w;h); NULL: not one */
static uint32_t *sixel_decode (const char *s, size_t n, int *ow, int *oh) {
  const char *p = s, *end = s + n;
  uint32_t pal[256], *px = NULL, color = 0xFF000000u;
  int w = 0, h = 0, x = 0, band = 0, i;
  for (i = 0; i < 256; i++) pal[i] = 0xFF000000u | (uint32_t)(i * 0x010101);
  while (p < end && *p != 'q') p++;	/* ESC P ... q */
  if (p >= end) return NULL;
  p++;
  while (p < end) {
    int c = (unsigned char)*p;
    if (c == '"') {	/* "pan;pad;w;h */
      int v[4] = {0, 0, 0, 0}, k = 0;
      p++;
      while (k < 4) {
        v[k++] = sixel_num(&p, end);
        if (p < end && *p == ';') p++;
        else break;
      }
      if (px == NULL && v[2] > 0 && v[3] > 0 && v[2] <= 16384 && v[3] <= 16384) {
        w = v[2];
        h = v[3];
        px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
      }
    }
    else if (c == '#') {	/* #n selects a color, #n;2;r;g;b (0 .. 100) sets it */
      int v[5] = {0, 0, 0, 0, 0}, k = 0;
      p++;
      while (k < 5) {
        v[k++] = sixel_num(&p, end);
        if (p < end && *p == ';') p++;
        else break;
      }
      v[0] &= 255;
      if (k == 5 && v[1] == 2)
        pal[v[0]] = 0xFF000000u | ((uint32_t)(v[2] * 255 / 100) << 16) | ((uint32_t)(v[3] * 255 / 100) << 8) |
                    (uint32_t)(v[4] * 255 / 100);
      color = pal[v[0]];
    }
    else if (c == '$') {
      x = 0;
      p++;
    }
    else if (c == '-') {
      x = 0;
      band += 6;
      p++;
    }
    else if (c == '!' || (c >= '?' && c <= '~')) {	/* !count<sixel>, or one sixel */
      int count = 1, bits, r;
      if (c == '!') {
        p++;
        count = sixel_num(&p, end);
        if (p >= end) break;
        c = (unsigned char)*p;
        if (c < '?' || c > '~') continue;
      }
      p++;
      bits = c - '?';
      if (px == NULL) return NULL;	/* no size given: not eimage.c's */
      for (; count > 0 && x < w; count--, x++)
        for (r = 0; r < 6; r++)
          if ((bits & (1 << r)) && band + r < h) px[(size_t)(band + r) * (size_t)w + (size_t)x] = color;
    }
    else if (c == 27) break;	/* ESC \ */
    else p++;
  }
  *ow = w;
  *oh = h;
  return px;
}


static void caret_off (void);


/* picture p, over rows y0 .. y1 of the screen (only those it covers) */
static void image_paint (const Pic *p, int y0, int y1) {
  int top = p->y * W.ch, left = p->x * W.cw, maxw = p->cols * W.cw, maxh = p->rows * W.ch, i, j;
  int from, to;
  if (p->px == NULL) return;
  if (y0 < p->y) y0 = p->y;
  if (y1 > p->y + p->rows - 1) y1 = p->y + p->rows - 1;
  if (y1 >= S.rows) y1 = S.rows - 1;
  if (y0 > y1) return;
  from = y0 * W.ch;
  to = (y1 + 1) * W.ch;
  if (W.car_on && W.car_y >= y0 && W.car_y <= y1) caret_off();	/* painted again after it, where it is then */
  for (j = 0; j < p->h && j < maxh; j++) {
    int py = top + j;
    if (py < from || py >= to || py < 0 || py >= W.fr.h) continue;
    for (i = 0; i < p->w && i < maxw; i++) {
      uint32_t c = p->px[(size_t)j * (size_t)p->w + (size_t)i];
      int x = left + i;
      if ((c >> 24) == 0 || x < 0 || x >= W.fr.w) continue;
      W.fr.px[(size_t)py * (size_t)W.fr.w + (size_t)x] = c & 0xFFFFFF;
    }
  }
  for (j = y0; j <= y1; j++) mark(j);
}


static void images_row (int y) {	/* the pictures over row y again (it was painted from its cells) */
  int k;
  for (k = 0; k < NIM; k++) image_paint(&IM[k], y, y);
}


/*
** The pictures of this flush (IMG[], from scr_image): the ones decoded
** before are kept (a sixel is decoded once), the rows of the ones that
** went or moved painted again from their cells, the new ones painted
*/
static void image_flush (int y_first, int y_last) {
  Pic now[MAX_IMG];
  int k, i, same = NIMG == NIM;
  memset(now, 0, sizeof(now));
  for (i = 0; i < NIMG; i++) {
    Pic *p = &now[i];
    for (k = 0; k < NIM; k++)	/* decoded already */
      if (IM[k].src && IM[k].n == IMG[i].n && memcmp(IM[k].src, IMG[i].data, IMG[i].n) == 0) {
        *p = IM[k];
        IM[k].src = NULL;
        IM[k].px = NULL;
        break;
      }
    if (p->src == NULL) {
      p->src = (char *)xmalloc(IMG[i].n + 1);
      memcpy(p->src, IMG[i].data, IMG[i].n);
      p->n = IMG[i].n;
      p->px = sixel_decode(IMG[i].data, IMG[i].n, &p->w, &p->h);
      same = 0;
    }
    if (i < NIM && (IM[i].x != IMG[i].x || IM[i].y != IMG[i].y || IM[i].cols != IMG[i].w || IM[i].rows != IMG[i].h)) same = 0;
    p->x = IMG[i].x;
    p->y = IMG[i].y;
    p->cols = IMG[i].w;
    p->rows = IMG[i].h;
  }
  if (!same) {	/* the old ones' rows from their cells; everything of the new ones */
    for (k = 0; k < NIM; k++) {
      int r;
      for (r = IM[k].y; r < IM[k].y + IM[k].rows && r < S.rows; r++) paint_row(r, S.front);
    }
    y_first = 0;
    y_last = S.rows - 1;
  }
  for (k = 0; k < NIM; k++) {	/* the ones not shown any more */
    free(IM[k].src);
    free(IM[k].px);
  }
  memcpy(IM, now, sizeof(now));
  NIM = NIMG;
  for (k = 0; k < NIM; k++)
    if (y_first <= y_last) image_paint(&IM[k], y_first, y_last);	/* over the rows painted again */
  for (i = 0; i < NIMG; i++) {
    free(IMG[i].data);
    IMG[i].data = NULL;
  }
  NIMG = 0;
}

/* }================================================================== */


/*
** {==================================================================
** The caret, painted into the picture over its cell (a block shows the
** character under it in the background's color), taken away by painting
** its row again from what is shown (S.front)
** ===================================================================
*/

static int caret_shape (void) {	/* 1 block, 2 underline, 3 bar (DECSCUSR 0 .. 6) */
  int s = (g_shape + 1) / 2;
  return s < 1 || s > 3 ? 1 : s;
}


static void caret_off (void) {
  if (!W.car_on) return;
  W.car_on = 0;
  if (W.car_y < 0 || W.car_y >= S.rows) return;
  paint_row(W.car_y, S.front);
  images_row(W.car_y);
}


static void caret_on (void) {
  const ECell *row = &S.front[(size_t)S.cy * (size_t)S.cols];
  int x = S.cx, wide, px, py, w, thin;
  uint32_t cc = ui_color(C_MCURSOR), fg, bg, at, ul;
  Frame *f = &W.fr;
  if (row[x].ch == 0 && x > 0 && row[x - 1].w == 2) x--;	/* on the right half of a wide one */
  wide = row[x].w == 2 && x + 1 < S.cols;
  px = x * W.cw;
  py = S.cy * W.ch;
  w = W.cw * (wide ? 2 : 1);
  thin = (int)(2.0f * W.scale * (W.px0 > 0 ? W.px / W.px0 : 1.0f) + 0.5f);	/* VS Code's 2 pixels, zoomed */
  if (thin < 1) thin = 1;
  if (!W.focused) {	/* a hollow box, not blinking: the window has not got the keys */
    fill(f, px, py, w, 1, cc);
    fill(f, px, py + W.ch - 1, w, 1, cc);
    fill(f, px, py, 1, W.ch, cc);
    fill(f, px + w - 1, py, 1, W.ch, cc);
  }
  else if (caret_shape() == 2) fill(f, px, py + W.ch - thin, w, thin, cc);
  else if (caret_shape() == 3) fill(f, px, py, thin, W.ch, cc);
  else {	/* a block: the character in it in the background's color */
    cell_colors(&row[x], &fg, &bg, &at, &ul);
    fill(f, px, py, w, W.ch, cc);
    paint_fg(&row[x], x, S.cy, wide, bg, cc, at & ~(uint32_t)(RGB_UNDER | RGB_CURLY | RGB_STRIKE), bg, 0);
  }
  W.car_on = 1;
  W.car_x = S.cx;
  W.car_y = S.cy;
  W.car_shape = caret_shape();
  W.car_focus = W.focused;
  mark(S.cy);
}


/* the caret as it should be now: where, what shape, on or off in its blink */
static void caret_sync (void) {
  int want = S.cy >= 0 && S.cy < S.rows && S.cx >= 0 && S.cx < S.cols && (W.blink_on || !W.focused);
  if (W.car_on && (!want || W.car_x != S.cx || W.car_y != S.cy || W.car_shape != caret_shape() ||
                   W.car_focus != W.focused)) caret_off();
  if (want && !W.car_on) caret_on();
  if (want && (W.ime_x != S.cx || W.ime_y != S.cy)) {	/* the IME's window goes under the caret */
    SDL_Rect r;
    int ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(W.win, &ww, &wh);
    out_size(&pw, &ph);
    r.x = S.cx * W.cw;
    r.y = S.cy * W.ch;
    r.w = W.cw;
    r.h = W.ch;
    if (pw > 0 && ph > 0 && pw != ww) {	/* macOS's Retina: in points */
      r.x = r.x * ww / pw;
      r.y = r.y * wh / ph;
      r.w = r.w * ww / pw;
      r.h = r.h * wh / ph;
    }
    SDL_SetTextInputRect(&r);
    W.ime_x = S.cx;
    W.ime_y = S.cy;
  }
}


/*
** The window surface: the rows painted since last time copied into it and
** only they shown (a key typed: a row or two, the status bar), everything
** when the window was uncovered or resized
*/
static void present_surface (void) {
  SDL_Surface *s = SDL_GetWindowSurface(W.win);
  int y, n = 0;
  if (s == NULL || W.fsurf == NULL) return;
  if (W.dirty || s != W.last) {
    uint32_t bgc = ui_color(C_EDITOR_BG);
    SDL_Rect r;
    SDL_FillRect(s, NULL, SDL_MapRGB(s->format, (Uint8)(bgc >> 16), (Uint8)(bgc >> 8), (Uint8)bgc));	/* the strip past the last cell */
    r.x = r.y = 0;
    r.w = W.tw;
    r.h = W.th;
    SDL_BlitSurface(W.fsurf, NULL, s, &r);
    SDL_UpdateWindowSurface(W.win);
  }
  else {
    for (y = 0; y < W.nrowdirty; y++) {
      SDL_Rect r, d;
      int y0 = y;
      if (!W.rowdirty[y]) continue;
      while (y + 1 < W.nrowdirty && W.rowdirty[y + 1]) y++;
      r.x = 0;
      r.y = y0 * W.ch;
      r.w = W.tw;
      r.h = y == W.nrowdirty - 1 ? W.th - r.y : (y - y0 + 1) * W.ch;	/* the last row: the strip under it too */
      d = r;
      SDL_BlitSurface(W.fsurf, &r, s, &d);
      W.rects[n++] = r;
    }
    if (n > 0) SDL_UpdateWindowSurfaceRects(W.win, W.rects, n);
  }
  W.last = s;
}


/* the rows painted since last time into the texture, and the window shown */
static void present (void) {
  if (W.use_surface) {
    present_surface();
    if (W.rowdirty) memset(W.rowdirty, 0, (size_t)W.nrowdirty);
    W.up0 = S.rows;
    W.up1 = -1;
    W.dirty = 0;
    W.shown = 1;
    return;
  }
  if (W.up1 >= W.up0) {
    SDL_Rect r;
    r.x = 0;
    r.y = W.up0 * W.ch;
    r.w = W.tw;
    r.h = W.up1 >= S.rows - 1 ? W.th - r.y : (W.up1 - W.up0 + 1) * W.ch;
    if (r.y + r.h > W.th) r.h = W.th - r.y;
    if (r.h > 0) SDL_UpdateTexture(W.tex, &r, W.fr.px + (size_t)r.y * (size_t)W.fr.w, W.fr.w * 4);
  }
  W.up0 = S.rows;
  W.up1 = -1;
  W.dirty = 0;
  W.shown = 1;
  {
    uint32_t bgc = ui_color(C_EDITOR_BG);
    SDL_Rect d;
    SDL_SetRenderDrawColor(W.ren, (Uint8)(bgc >> 16), (Uint8)(bgc >> 8), (Uint8)bgc, 255);
    SDL_RenderClear(W.ren);	/* the strip past the last cell, when the window is not a whole number of them */
    d.x = d.y = 0;
    d.w = W.tw;
    d.h = W.th;
    SDL_RenderCopy(W.ren, W.tex, NULL, &d);
  }
  SDL_RenderPresent(W.ren);
}


static int caret_blinks (void) {
  return W.focused && (g_shape == 0 || (g_shape & 1) != 0) && S.cy >= 0 && S.cy < S.rows && S.cx >= 0 && S.cx < S.cols;
}


/* while no key comes: the caret blinks on its own (term_key waits no longer than this) */
static int blink_wait (void) {
  long long left;
  if (!W.shown || !caret_blinks()) return -1;
  left = W.blink_at + 530 - (long long)SDL_GetTicks();
  return left < 0 ? 0 : (int)left;
}


static void blink_tick (void) {
  long long now = (long long)SDL_GetTicks();
  if (!W.shown || S.full || !caret_blinks() || now - W.blink_at < 530) return;	/* S.full: new metrics, not painted yet */
  W.blink_on = !W.blink_on;
  W.blink_at = now;
  caret_sync();
  present();
}


static void font_settings (void);


/*
** The system frame goes when the menu bar is the title bar (custom), and
** comes back when it is not (window.menuBarVisibility hidden, Zen mode, or
** "native"): the window must still be moved and closed
*/
static void tb_update (void) {
  int menu = S.cols > 2 && S.front[1].ch == 0xF121;	/* the app's icon at row 0: mme's menu bar */
  int want = W.custom && menu && !remote_mode;	/* a remote window: the remote mme's menus are not ours to drag by */
  if (want == W.borderless) return;
  W.borderless = want;
  W.tb_hover = W.tb_press = -1;
  SDL_SetWindowBordered(W.win, want ? SDL_FALSE : SDL_TRUE);
  S.full = 1;	/* another size: drawn again */
}


/* the buttons' look changed (the mouse over one, one held): row 0 shown again */
static void tb_refresh (void) {
  if (!W.shown || S.full || S.front == NULL || W.fr.px == NULL) return;
  paint_row(0, S.front);
  caret_sync();
  present();
}


static void quit_request (void);


static void tb_action (int b) {
  if (b == 0) SDL_MinimizeWindow(W.win);
  else if (b == 1 && (SDL_GetWindowFlags(W.win) & SDL_WINDOW_MAXIMIZED)) SDL_RestoreWindow(W.win);
  else if (b == 1) SDL_MaximizeWindow(W.win);
  else if (b == 2) quit_request();
}


/* the mouse in window points, as picture pixels (macOS's Retina has more) */
static void to_pixels (int *x, int *y) {
  int ww = 0, wh = 0, pw = 0, ph = 0;
  SDL_GetWindowSize(W.win, &ww, &wh);
  out_size(&pw, &ph);
  if (ww > 0 && wh > 0 && pw > 0 && pw != ww) {
    *x = *x * pw / ww;
    *y = *y * ph / wh;
  }
}


/*
** The system asks what is under the mouse (Windows' WM_NCHITTEST): the
** edges resize the window, the empty parts of the title bar move it; the
** menus, the command center, the buttons are mme's
*/
static SDL_HitTestResult SDLCALL hit_test (SDL_Window *win, const SDL_Point *pt, void *data) {
  int ww = 0, wh = 0, x = pt->x, y = pt->y, e = (int)(5.0f * W.scale + 0.5f), col;
  (void)data;
  if (!W.borderless || W.fr.px == NULL) return SDL_HITTEST_NORMAL;
  SDL_GetWindowSize(win, &ww, &wh);
  if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)) {
    int l = x < e, r = x >= ww - e, t = y < e, b = y >= wh - e;
    if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (t) return SDL_HITTEST_RESIZE_TOP;
    if (b) return SDL_HITTEST_RESIZE_BOTTOM;
    if (l) return SDL_HITTEST_RESIZE_LEFT;
    if (r) return SDL_HITTEST_RESIZE_RIGHT;
  }
  to_pixels(&x, &y);
  if (y >= W.ch || x >= tb_left()) return SDL_HITTEST_NORMAL;
  col = x / W.cw;
  if (col < S.cols && (menubar_hit(col) >= 0 || menubar_cc_hit(col) != 0)) return SDL_HITTEST_NORMAL;
  return SDL_HITTEST_DRAGGABLE;
}


/* the mouse over the buttons: theirs, not mme's (1) */
static int tb_mouse (int x, int y, int type) {
  int b;
  if (!W.borderless) return 0;
  to_pixels(&x, &y);
  b = tb_button(x, y);
  if (type == SDL_MOUSEMOTION) {
    if (b != W.tb_hover) {
      W.tb_hover = b;
      tb_refresh();
    }
    return b >= 0 || W.tb_press >= 0;
  }
  if (type == SDL_MOUSEBUTTONDOWN) {
    if (b < 0) return 0;
    W.tb_press = b;
    W.tb_hover = b;
    tb_refresh();
    return 1;
  }
  if (W.tb_press < 0) return 0;	/* up */
  {
    int was = W.tb_press;
    W.tb_press = -1;
    tb_refresh();
    if (b == was) tb_action(b);
  }
  return 1;
}


/*
** The picture shown: the rows that changed painted into the frame, the
** image preview's picture over its cells, the caret (it blinks, as the
** settings say), then the rows painted copied into the texture; the
** title, the mouse pointer. Nothing changed: nothing is shown.
*/
void scr_flush (void) {
  int y, painted0 = S.rows, painted1 = -1, caret_row = 0;
  size_t row = (size_t)S.cols * sizeof(ECell);
  long long now = (long long)SDL_GetTicks();
  if (scr_overlay_hook) scr_overlay_hook();
  if (W.win == NULL || S.back == NULL) return;
  if (S.full || now - W.font_seen >= 1000) font_settings();	/* editor.fontSize changed: the font too */
  if (S.full) memset(g_style, 0, sizeof(g_style));	/* the theme may have changed */
  picture_size();
  if (W.up0 > S.rows) W.up0 = S.rows;
  for (y = 0; y < S.rows; y++) {
    ECell *b = &S.back[(size_t)y * (size_t)S.cols];
    if (!S.full && memcmp(b, &S.front[(size_t)y * (size_t)S.cols], row) == 0) continue;
    paint_row(y, S.back);
    if (y == S.cy) caret_row = 1;
    if (y < painted0) painted0 = y;
    painted1 = y;
  }
  memcpy(S.front, S.back, row * (size_t)S.rows);
  S.full = 0;
  tb_update();
  image_flush(painted0, painted1);
  if (!caret_blinks() || W.ccx != S.cx || W.ccy != S.cy || caret_row) {	/* it moved, or typing: shown at once */
    W.blink_on = 1;
    W.blink_at = now;
  }
  else if (now - W.blink_at >= 530) {
    W.blink_on = !W.blink_on;
    W.blink_at = now;
  }
  W.ccx = S.cx;
  W.ccy = S.cy;
  caret_sync();
  if (strcmp(ui_title, W.title) != 0) {	/* the title bar says what mme's does */
    snprintf(W.title, sizeof(W.title), "%s", ui_title);
    SDL_SetWindowTitle(W.win, W.title[0] ? W.title : "mme");
  }
  if (g_ptr != W.ptr_now && g_ptr >= 0 && g_ptr < 3 && W.ptr[g_ptr]) {
    SDL_SetCursor(W.ptr[g_ptr]);
    W.ptr_now = g_ptr;
  }
  if (W.up1 < W.up0 && !W.dirty && W.shown) return;
  present();
}

/* }================================================================== */


/*
** {==================================================================
** The font: mmc-term's tfont.c, as the settings say (editor.fontFamily,
** editor.fontSize, VS Code's)
** ===================================================================
*/

static void font_metrics (void) {
  W.ime_x = -1;	/* the IME's window is placed again */
  W.cw = font_cell_w();
  W.ch = font_cell_h();
  W.ascent = font_ascent();
  if (W.cw < 1) W.cw = 1;
  if (W.ch < 1) W.ch = 1;
}


/*
** window.titleBarStyle: "custom" is VS Code's default, but on macOS, whose
** window without a title bar is not minimized (nor made full screen)
*/
static int title_custom (void) {
#ifdef __APPLE__
  return strcmp(json_str(settings_get("window\\.titleBarStyle"), "native"), "custom") == 0;
#else
  return strcmp(json_str(settings_get("window\\.titleBarStyle"), "custom"), "native") != 0;
#endif
}


/*
** The folder the fonts come with (tfont.c keeps one): mme-fonts next to
** the program, else share/mme-fonts beside its bin (make install's), else
** usr/share/fonts when it is in an mmc shell's usr/bin
*/
static void font_dirs (void) {
  char *exe = os_exe_path(NULL), *dir, *up, *share, *fonts;
  OsStat st;
  if (exe == NULL) return;
  dir = path_dirname(exe);
  fonts = path_join(dir, "mme-fonts");
  if (os_stat(fonts, &st) != 0 || !st.exists || !st.is_dir) {
    free(fonts);
    up = path_dirname(dir);
    share = path_join(up, "share");
    fonts = path_join(share, "mme-fonts");
    if (os_stat(fonts, &st) != 0 || !st.exists || !st.is_dir) {
      free(fonts);
      fonts = path_join(share, "fonts");
    }
    free(share);
    free(up);
  }
  font_add_dir(fonts);
  free(fonts);
  free(dir);
  free(exe);
}


static int font_make (const char *fam, double size) {
  Config c;
  size_t n = 0;
  memset(&c, 0, sizeof(c));
  c.smoothing = SMOOTH_CLEARTYPE;	/* Windows: GDI's ClearType, like every other Windows program */
  c.ligatures = 0;	/* one character a cell: mme places each itself */
  snprintf(W.font_fam, sizeof(W.font_fam), "%s", fam);
  W.font_size = size;
  while (*fam == ' ' || *fam == '\'' || *fam == '"') fam++;	/* VS Code's list: its first family */
  while (fam[n] && fam[n] != ',' && fam[n] != '\'' && fam[n] != '"' && n + 1 < sizeof(c.font)) n++;
  memcpy(c.font, fam, n);
  c.font[n] = '\0';
  if (font_init(&c) != 0) return -1;
  if (size < 6 || size > 100) size = 14;
  W.px0 = W.px = (float)size * W.scale;	/* VS Code's pixels at 96 DPI, as many more as the screen's DPI */
  font_set_px(W.px);
  font_metrics();
  return 0;
}


static int font_setup (void) {
  font_dirs();
  return font_make(json_str(settings_get("editor\\.fontFamily"), ""), json_num(settings_get("editor\\.fontSize"), 14));
}


/* editor.fontFamily or editor.fontSize changed in the settings: the font is made again (the zoom goes) */
static void font_settings (void) {
  const char *fam = json_str(settings_get("editor\\.fontFamily"), "");
  W.custom = title_custom();
  double size = json_num(settings_get("editor\\.fontSize"), 14);
  W.font_seen = (long long)SDL_GetTicks();
  if (size == W.font_size && strcmp(fam, W.font_fam) == 0) return;
  if (font_make(fam, size) != 0) return;
  S.full = 1;
}


/* the window went to a screen with another DPI: the font keeps its size to the eye */
static void font_dpi (void) {
  float ddpi, hdpi, vdpi, s;
  int i = SDL_GetWindowDisplayIndex(W.win);
  if (i < 0 || SDL_GetDisplayDPI(i, &ddpi, &hdpi, &vdpi) != 0 || hdpi < 48.0f) return;
  s = hdpi / 96.0f;
  if (s - W.scale < 0.01f && W.scale - s < 0.01f) return;
  W.px *= s / W.scale;
  W.px0 *= s / W.scale;
  W.scale = s;
  font_set_px(W.px);
  font_metrics();
  S.full = 1;
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


static char *g_clip_ours;	/* what mme put on the system's clipboard last, LF ends */
static int g_clip_quiet;	/* clip_sync is giving mme the system's: not put back there */


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
    free(g_clip_ours);
    g_clip_ours = xstrdup(out.s);
    if (!g_clip_quiet) SDL_SetClipboardText(out.s);
    buf_free(&out);
  }
}


/*
** mme's clipboard is the system's: text copied in another program is what
** Ctrl+V, the terminal's paste, vim's "+ and the Chat box paste. It is
** taken when the clipboard changes, the window gets the keys again, or a
** paste key comes.
*/
static void clip_sync (void) {
  char *t, *r, *w;
  if (!SDL_HasClipboardText()) return;
  t = SDL_GetClipboardText();
  if (t == NULL) return;
  for (r = w = t; *r; r++)	/* CR LF: one newline, as mme keeps text */
    if (!(*r == '\r' && r[1] == '\n')) *w++ = *r;
  *w = '\0';
  if (*t && (g_clip_ours == NULL || strcmp(t, g_clip_ours) != 0)) {
    g_clip_quiet = 1;	/* the system has it: only mme's copy changes (its other formats stay) */
    clip_set(t, strlen(t));
    g_clip_quiet = 0;
  }
  SDL_free(t);
}


void term_ask_pixels (void) {
}


int term_cell_px (int *w, int *h) {	/* the image preview's pictures: a cell's pixels */
  if (W.cw < 2 || W.ch < 2) return 0;
  *w = W.cw;
  *h = W.ch;
  return 1;
}


int term_can_raise (void) {
  return 1;
}


void term_raise (void) {	/* another window asked (the folder is open here): this one to the front */
  if (W.win == NULL) return;
  if (SDL_GetWindowFlags(W.win) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(W.win);
  SDL_RaiseWindow(W.win);
}


/*
** An accessibility signal's sound where Windows' system sounds are not
** (eaccess.c): a short tone of its own pitch, from SDL's audio
*/
int term_tone (int sig) {
  static const int hz[SIG_N] = {220, 330, 523, 392, 660, 196, 587, 784};
  static SDL_AudioDeviceID dev;
  static int tried;
  static float buf[48000 / 10];
  int i, n = 48000 * 80 / 1000;	/* 80 ms */
  if (sig < 0 || sig >= SIG_N) return -1;
  if (!tried) {
    SDL_AudioSpec want, have;
    tried = 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
      memset(&want, 0, sizeof(want));
      want.freq = 48000;
      want.format = AUDIO_F32SYS;
      want.channels = 1;
      want.samples = 512;
      dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
      if (dev) SDL_PauseAudioDevice(dev, 0);
    }
  }
  if (!dev) return -1;
  for (i = 0; i < n; i++) {	/* faded in and out: no click */
    double env = i < 480 ? i / 480.0 : n - i < 960 ? (n - i) / 960.0 : 1.0;
    buf[i] = (float)(0.2 * env * sin(2.0 * M_PI * hz[sig] * i / 48000.0));
  }
  SDL_ClearQueuedAudio(dev);
  SDL_QueueAudio(dev, buf, (Uint32)((size_t)n * sizeof(float)));
  return 0;
}


/*
** New Window: mme-sdl again, a window (a process) of its own, as VS Code's
** windows. A tab dropped out of the window: the new one opens there
** (MME_WINDOW_AT, the screen's point, read by its term_open).
*/
int term_new_window (const char *arg) {
  char *exe = os_exe_path(NULL), *argv[3];
  int r, at = term_mouse.out;
  if (exe == NULL) return -1;
  argv[0] = exe;
  argv[1] = (char *)arg;
  argv[2] = NULL;
  if (at) {
    char pos[32];
    int gx = 0, gy = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    snprintf(pos, sizeof(pos), "%d,%d", gx, gy);
    os_setenv("MME_WINDOW_AT", pos);
  }
  r = spawn_detached(argv);
  if (at) os_setenv("MME_WINDOW_AT", NULL);
  free(exe);
  return r;
}


void term_size (int *cols, int *rows) {
  int w = 0, h = 0;
  out_size(&w, &h);
  *cols = w / (W.cw > 0 ? W.cw : 8);
  *rows = h / (W.ch > 0 ? W.ch : 16);
  if (*cols < 20) *cols = 20;
  if (*rows < 6) *rows = 6;
}


/*
** Keys sent by another program (SendInput with only the virtual key: an
** automation tool, an on-screen keyboard, a remote desktop) come without the
** keyboard's scan code, and SDL 2 drops a key it cannot place by scan code:
** Enter, Ctrl+S never arrive. The window's messages go through key_proc
** first, which gives such a key the scan code its virtual key has (and the
** extended bit a real keyboard sets for the arrows, Home, Insert ...). It
** also keeps a maximized window without a frame on the screen (the custom
** title bar).
*/
#ifdef _WIN32
static WNDPROC g_sdl_proc;

static LRESULT CALLBACK key_proc (HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCALCSIZE && w == TRUE && W.borderless && IsZoomed(h)) {
    /*
    ** Maximized without a frame: Windows puts the window its frame's width
    ** past the screen's edges, which would hide the title bar's top and the
    ** edges; the picture is the monitor's work area (the taskbar stays)
    */
    NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)l;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
      p->rgrc[0] = mi.rcWork;
      return 0;
    }
  }
  if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && ((l >> 16) & 0xFF) == 0) {
    UINT sc = MapVirtualKeyW((UINT)w, 0) & 0xFF;	/* MAPVK_VK_TO_VSC */
    switch (w) {
      case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_HOME: case VK_END: case VK_PRIOR:
      case VK_NEXT: case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE:
      case VK_NUMLOCK:
        l |= (LPARAM)1 << 24;	/* extended */
        break;
    }
    l |= (LPARAM)sc << 16;
  }
  return CallWindowProcW(g_sdl_proc, h, m, w, l);
}
#endif


static void key_filter (void) {
#ifdef _WIN32
  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(W.win, &wm)) return;
  g_sdl_proc = (WNDPROC)SetWindowLongPtrW(wm.info.win.window, GWLP_WNDPROC, (LONG_PTR)key_proc);
#endif
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
  int w, h, wx, wy;
  SDL_SetMainReady();
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");	/* sharp on a high DPI screen */
  SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");	/* the click that activates the window does its work too (a button, the caret) */
  SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");	/* without a frame still maximized, resized by its edges */
  SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");	/* and snapped, minimized with Windows' animation */
  SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");	/* mme.rc's icon, in the title bar and the taskbar */
  SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return -1;
  {
    SDL_version v;
    SDL_GetVersion(&v);	/* the SDL2 it runs with (a Linux system's own can be older than the headers) */
    g_precise = SDL_VERSIONNUM(v.major, v.minor, v.patch) >= SDL_VERSIONNUM(2, 0, 18);
  }
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
  W.custom = title_custom();
  W.borderless = W.custom;	/* the menu bar is shown, mostly: tb_update says when not */
  W.tb_hover = W.tb_press = -1;
  {	/* a tab dropped out of another window: this one where it was dropped */
    char *at = os_getenv("MME_WINDOW_AT");
    int ax, ay;
    wx = wy = SDL_WINDOWPOS_CENTERED;
    if (at && sscanf(at, "%d,%d", &ax, &ay) == 2) {
      SDL_Rect u = {0, 0, 0, 0}, b;
      int i, n = SDL_GetNumVideoDisplays();
      for (i = 0; i < n; i++)	/* the screen it was dropped on: all of the window on it */
        if (SDL_GetDisplayBounds(i, &b) == 0 && ax >= b.x && ax < b.x + b.w && ay >= b.y && ay < b.y + b.h) {
          if (SDL_GetDisplayUsableBounds(i, &u) != 0) u = b;
          break;
        }
      wx = ax - w / 4;
      wy = ay - W.ch;
      if (u.w > 0) {
        if (wx + w > u.x + u.w) wx = u.x + u.w - w;
        if (wy + h > u.y + u.h) wy = u.y + u.h - h;
        if (wx < u.x) wx = u.x;
        if (wy < u.y) wy = u.y;
      }
    }
    free(at);
    os_setenv("MME_WINDOW_AT", NULL);	/* not for its own windows */
  }
  W.win = SDL_CreateWindow("mme", wx, wy, w, h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (W.custom ? SDL_WINDOW_BORDERLESS : 0));
  if (W.win == NULL) return -1;
  SDL_SetWindowHitTest(W.win, hit_test, NULL);
  SDL_SetWindowMinimumSize(W.win, 40 * W.cw, 10 * W.ch);
#ifndef __APPLE__
  SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");	/* the window's own bitmap (GDI, X11's), not a texture */
  W.use_surface = SDL_GetWindowSurface(W.win) != NULL;
#endif
  if (!W.use_surface) {	/* the picture is painted here already: no GPU driver to load (tens of MB) */
    W.ren = SDL_CreateRenderer(W.win, -1, SDL_RENDERER_SOFTWARE);
    if (W.ren == NULL) W.ren = SDL_CreateRenderer(W.win, -1, 0);
    if (W.ren == NULL) return -1;
  }
  window_icon();
  key_filter();
  W.ptr[PTR_DEFAULT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  W.ptr[PTR_TEXT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
  W.ptr[PTR_POINTER] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
  W.ptr_now = -1;
  W.ccx = W.ccy = -1;
  W.ime_x = W.ime_y = -1;
  W.focused = 1;
  W.up0 = 1 << 30;
  W.up1 = -1;
  SDL_EventState(SDL_DROPFILE, SDL_ENABLE);	/* a file dropped on the window */
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
  if (W.fsurf) SDL_FreeSurface(W.fsurf);
  W.fsurf = NULL;
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
  if (m & KMOD_ALT) k |= KM_ALT;	/* right Alt too: AltGr's characters are told apart by their text (on_keydown) */
#ifdef __APPLE__
  if (m & KMOD_GUI) k |= KM_CTRL;	/* Cmd: Cmd+S saves, Cmd+C copies, as VS Code's keys on a Mac */
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


static void paste_text (const char *t) {
  free(g_paste);
  g_paste = xstrdup(t ? t : "");
  push(K_PASTE);
}


static void paste_clipboard (void) {
  char *t = SDL_GetClipboardText();
  paste_text(t);
  SDL_free(t);
}


/*
** The key typed a character though Ctrl or Alt is down: AltGr (Windows
** sends a left Ctrl with it) on a German keyboard, Option on a Mac. SDL has
** its text waiting behind the key: the character goes, not the shortcut.
*/
static int text_queued (void) {
  SDL_Event e;
  return SDL_PeepEvents(&e, 1, SDL_PEEKEVENT, SDL_TEXTINPUT, SDL_TEXTINPUT) > 0 && (unsigned char)e.text.text[0] >= 32;
}


static int text_follows (int k) {
  if (text_queued()) return 1;
  if ((k & (KM_CTRL | KM_ALT)) != (KM_CTRL | KM_ALT)) return 0;
  SDL_PumpEvents();	/* AltGr: its character may still be in the window's queue */
  return text_queued();
}


/* the keypad's character with Ctrl or Alt (without them it comes as text); 0: none */
static int keypad_char (SDL_Keycode sym, Uint16 mod) {
  int num = (mod & KMOD_NUM) != 0;
  switch (sym) {
    case SDLK_KP_DIVIDE: return '/';
    case SDLK_KP_MULTIPLY: return '*';
    case SDLK_KP_MINUS: return '-';
    case SDLK_KP_PLUS: return '+';
    case SDLK_KP_EQUALS: return '=';
    case SDLK_KP_PERIOD: return num ? '.' : 0;
    case SDLK_KP_0: return num ? '0' : 0;
    case SDLK_KP_5: return num ? '5' : 0;
    default:
      if (sym >= SDLK_KP_1 && sym <= SDLK_KP_9 && num) return '1' + (int)(sym - SDLK_KP_1);
      return 0;
  }
}


/*
** VS Code's zoom keys: Ctrl+= and Ctrl+- (Shift or not, the keypad's too),
** Ctrl+NumPad0. A terminal zooms by itself; the window is the font's
** here. 2: not one (or keybindings.json gives the key to something else).
*/
static int zoom_of (SDL_Keycode sym, int key, int k) {
  int z = 2, c;
  if ((k & ~KM_SHIFT) != KM_CTRL) return 2;
  if (sym == '=' || sym == '+' || sym == SDLK_KP_PLUS) z = 1;
  else if (sym == '-' || sym == '_' || sym == SDLK_KP_MINUS) z = -1;
  else if (sym == SDLK_KP_0 && !(k & KM_SHIFT)) z = 0;
  if (z == 2) return 2;
  for (c = keys_count() - 1; c >= 0; c--) {	/* keybindings.json has the key: it is the user's (keys_find would drop a running command's args) */
    int cmd, k1, k2;
    const char *when;
    keys_entry(c, &cmd, &k1, &k2, &when);
    if (k1 == key && k2 == 0) return 2;
  }
  return z;
}


static void on_keydown (const SDL_KeyboardEvent *e) {
  SDL_Keycode sym = e->keysym.sym;
  int k = km_of(e->keysym.mod), code = -1, ch, key;
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
  if (code == K_INS && (k & KM_SHIFT) && !(k & (KM_CTRL | KM_ALT))) {	/* Shift+Insert: paste, as a terminal does */
    clip_sync();
    paste_clipboard();
    g_skip_text = 1;
    return;
  }
  if (code >= 0) {
    push(code | k);
    g_skip_text = 1;
    return;
  }
  if (!(k & (KM_CTRL | KM_ALT))) return;	/* a character: it comes as text */
  if (sym >= 32 && sym < 127) ch = (int)sym;
  else if (e->keysym.scancode >= SDL_SCANCODE_A && e->keysym.scancode <= SDL_SCANCODE_Z)	/* Ctrl+C on a Russian keyboard */
    ch = 'a' + (int)(e->keysym.scancode - SDL_SCANCODE_A);
  else if (e->keysym.scancode >= SDL_SCANCODE_1 && e->keysym.scancode <= SDL_SCANCODE_0)
    ch = e->keysym.scancode == SDL_SCANCODE_0 ? '0' : '1' + (int)(e->keysym.scancode - SDL_SCANCODE_1);
  else ch = (k & KM_CTRL) ? keypad_char(sym, e->keysym.mod) : 0;	/* Alt+NumPad digits: Windows' Alt codes (Alt+0169) */
  if (ch == 0) return;
  if (text_follows(k)) return;	/* AltGr+Q: @ */
  key = kitty_key(ch, k);
  switch (zoom_of(sym, key, k)) {
    case 1: term_font(1); g_skip_text = 1; return;
    case -1: term_font(-1); g_skip_text = 1; return;
    case 0: term_font(0); g_skip_text = 1; return;
  }
  if (ch == 'v' && (k & KM_CTRL)) clip_sync();	/* Ctrl+V, Ctrl+Shift+V: what was copied anywhere */
  push(key);
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
  out_size(&pw, &ph);
  if (ww > 0 && pw > 0 && pw != ww) {
    x = x * pw / ww;
    y = y * ph / wh;
  }
  term_mouse.out = x < 0 || y < 0 || (pw > 0 && x >= pw) || (ph > 0 && y >= ph);	/* a tab dragged out */
  if (x < 0) x = 0;	/* dragged out of the window: its edge, as a terminal says */
  if (y < 0) y = 0;
  term_mouse.x = x / (W.cw > 0 ? W.cw : 1);
  term_mouse.y = y / (W.ch > 0 ? W.ch : 1);
  if (term_mouse.x >= S.cols) term_mouse.x = S.cols - 1;
  if (term_mouse.y >= S.rows) term_mouse.y = S.rows - 1;
}


/* n wheel steps where the mouse is; mods: Shift for across */
static void wheel_push (int d, int n, int mods) {
  int mx, my;
  if (n > 20) n = 20;
  SDL_GetMouseState(&mx, &my);
  while (n-- > 0) {
    memset(&term_mouse, 0, sizeof(term_mouse));
    mouse_at(mx, my);
    term_mouse.wheel = d;
    term_mouse.button = 3;
    term_mouse.mods = mods;
    push(K_MOUSE);
  }
}


static int g_drops;	/* the files of this drop pasted so far */


/* the window's x (the system's, or the title bar's): Close Window, which asks about unsaved files */
static void quit_request (void) {
  if (remote_mode && ++remote_close >= 2) return;	/* a remote window: the second x ends it (remote_main) */
  if (remote_mode) {	/* the first: the remote mme is asked to close (its unsaved files) */
    push('w' | KM_CTRL | KM_SHIFT);
    return;
  }
  if (when_ctx("terminalFocus") == NULL) push(K_ESC);	/* a question, a list open: it goes first (the shell keeps its line) */
  push('w' | KM_CTRL | KM_SHIFT);	/* Ctrl+Shift+W: this window only (Ctrl+Q, Exit, closes them all) */
}


/* an SDL event as keys in the queue */
static void on_event (const SDL_Event *e) {
  switch (e->type) {
    case SDL_QUIT: quit_request(); break;
    case SDL_WINDOWEVENT:
      switch (e->window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED: case SDL_WINDOWEVENT_RESIZED: case SDL_WINDOWEVENT_DISPLAY_CHANGED:
          font_dpi();	/* another screen may have another DPI */
          S.full = 1;
          break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
          W.focused = 1;
          W.blink_on = 1;
          W.blink_at = (long long)SDL_GetTicks();
          clip_sync();
          break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
          W.focused = 0;
          break;
        case SDL_WINDOWEVENT_LEAVE:	/* no button is under the mouse */
          if (W.tb_hover >= 0 && W.tb_press < 0) {
            W.tb_hover = -1;
            tb_refresh();
          }
          break;
      }
      W.dirty = 1;
      break;
    case SDL_CLIPBOARDUPDATE: clip_sync(); break;
    case SDL_RENDER_TARGETS_RESET: case SDL_RENDER_DEVICE_RESET:	/* the texture's pixels are gone: made again */
      W.tw = -1;
      S.full = 1;
      break;
    case SDL_KEYDOWN: on_keydown(&e->key); break;
    case SDL_KEYUP: g_skip_text = 0; break;	/* the text a shortcut key made comes before its release */
    case SDL_TEXTINPUT: on_text(e->text.text); break;
    case SDL_DROPBEGIN: g_drops = 0; break;
    case SDL_DROPFILE: {	/* its path pasted, quoted when it has a space, as a terminal does */
      const char *f = e->drop.file ? e->drop.file : "";
      Buf b;
      buf_init(&b);
      if (g_drops++ > 0) buf_putc(&b, ' ');
      if (strchr(f, ' ')) buf_printf(&b, "\"%s\"", f);
      else buf_puts(&b, f);
      buf_putc(&b, '\0');
      paste_text(b.s);
      buf_free(&b);
      SDL_free(e->drop.file);
      break;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
      int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_MIDDLE ? 1 :
              e->button.button == SDL_BUTTON_RIGHT ? 2 : -1;
      if (b < 0) break;
      if (b == 0 && tb_mouse(e->button.x, e->button.y, (int)e->type)) break;	/* the title bar's buttons */
      if (b == 2 && e->type == SDL_MOUSEBUTTONDOWN) clip_sync();	/* a right-click menu may paste */
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
      if (tb_mouse(e->motion.x, e->motion.y, SDL_MOUSEMOTION)) break;
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
    case SDL_MOUSEWHEEL: {	/* a notch is a step; a touchpad's bits add up to steps; across: Shift+wheel */
      float dx = g_precise ? e->wheel.preciseX : (float)e->wheel.x;
      float dy = g_precise ? e->wheel.preciseY : (float)e->wheel.y;
      int mods = km_of(SDL_GetModState());
      if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        dx = -dx;
        dy = -dy;
      }
      W.wheel_y += dy;
      W.wheel_x += dx;
      if (W.wheel_y >= 1.0f || W.wheel_y <= -1.0f) {
        int n = (int)(W.wheel_y > 0 ? W.wheel_y : -W.wheel_y);
        wheel_push(W.wheel_y > 0 ? -1 : 1, n, mods);	/* away from you: up */
        W.wheel_y += W.wheel_y > 0 ? -(float)n : (float)n;
      }
      if (W.wheel_x >= 1.0f || W.wheel_x <= -1.0f) {
        int n = (int)(W.wheel_x > 0 ? W.wheel_x : -W.wheel_x);
        wheel_push(W.wheel_x > 0 ? 1 : -1, n, mods | KM_SHIFT);	/* to the right */
        W.wheel_x += W.wheel_x > 0 ? -(float)n : (float)n;
      }
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
    {	/* the caret blinks while nothing comes, whoever waits (a dialog, the Chat box) */
      int b = blink_wait();
      if (b >= 0 && (wait < 0 || b < wait)) wait = b;
    }
    got = wait < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, wait);
    if (!got) {
      blink_tick();
      continue;
    }
    on_event(&e);
    if (S.full) {	/* resized, zoomed: the editor draws again before the next key */
      if (g_qn == 0) return K_NONE;
    }
    else if (W.dirty && W.shown) {	/* uncovered, the keys came or went (the caret's shape): shown again */
      caret_sync();
      present();
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
