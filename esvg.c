/*
** esvg.c - SVG pictures: a small rasterizer
**
** File icon themes (vscode-icons, Material Icon Theme) draw their icons
** in SVG, and mme-sdl shows them in the explorer, tabs and lists at a
** cell's size. What icons use is read: <path>, <rect>, <circle>,
** <ellipse>, <line>, <polyline>, <polygon>, <g> and <use>; fill and
** stroke (colors, linear and radial gradients), opacity, fill-rule,
** transform, style="" and <style>'s .class rules, clip-path and mask
** (as a clip). Text, images and filters are not drawn (an element with a
** filter, a blurred shadow mostly, is left out).
**
** The shapes are flattened to polygons and filled scanline by scanline,
** four samples down and exact coverage across each pixel.
*/

#include "mmc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void *zalloc (size_t n, size_t size) {
  void *p = xmalloc(n * size);
  memset(p, 0, n * size);
  return p;
}


/*
** {==================================================================
** The source: tags, attributes, numbers, colors
** ===================================================================
*/

typedef struct Tag {
  const char *name, *attrs;	/* in the source */
  size_t nname, nattrs;
  int close, empty;	/* </name>, <name/> */
  const char *end;	/* after the tag */
} Tag;


/* the next tag at or after *p (text, comments, <? ?>, <!...> skipped); 0: none */
static int next_tag (const char **p, const char *end, Tag *t) {
  const char *s = *p;
  for (;;) {
    while (s < end && *s != '<') s++;
    if (s >= end) return 0;
    if (end - s >= 4 && memcmp(s, "<!--", 4) == 0) {
      const char *e = s + 4;
      while (e + 3 <= end && memcmp(e, "-->", 3) != 0) e++;
      s = e + 3;
      continue;
    }
    if (end - s >= 9 && memcmp(s, "<![CDATA[", 9) == 0) {	/* a <style>'s rules may be in one: the tag loop skips it */
      const char *e = s + 9;
      while (e + 3 <= end && memcmp(e, "]]>", 3) != 0) e++;
      s = e + 3;
      continue;
    }
    if (s + 1 < end && (s[1] == '?' || s[1] == '!')) {
      while (s < end && *s != '>') s++;
      continue;
    }
    break;
  }
  memset(t, 0, sizeof(*t));
  s++;
  if (s < end && *s == '/') {
    t->close = 1;
    s++;
  }
  t->name = s;
  while (s < end && *s != '>' && *s != '/' && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') s++;
  t->nname = (size_t)(s - t->name);
  t->attrs = s;
  {
    char q = 0;
    while (s < end && (q || *s != '>')) {
      if (q && *s == q) q = 0;
      else if (!q && (*s == '"' || *s == '\'')) q = *s;
      s++;
    }
  }
  t->nattrs = (size_t)(s - t->attrs);
  if (t->nattrs && t->attrs[t->nattrs - 1] == '/') {
    t->empty = 1;
    t->nattrs--;
  }
  t->end = s < end ? s + 1 : end;
  *p = t->end;
  return 1;
}


static int tag_is (const Tag *t, const char *name) {
  size_t n = strlen(name);
  const char *s = t->name;
  size_t k = t->nname;
  const char *colon = memchr(s, ':', k);	/* svg:path */
  if (colon) {
    k -= (size_t)(colon + 1 - s);
    s = colon + 1;
  }
  return k == n && memcmp(s, name, n) == 0;
}


/* attribute name's value (its quotes off) into out; 0: not there */
static int attr (const Tag *t, const char *name, char *out, size_t n) {
  const char *s = t->attrs, *end = t->attrs + t->nattrs;
  size_t nl = strlen(name);
  while (s < end) {
    const char *k, *v;
    size_t kl, vl;
    char q;
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    k = s;
    while (s < end && *s != '=' && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') s++;
    kl = (size_t)(s - k);
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    if (s >= end || *s != '=') continue;
    s++;
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    if (s >= end) break;
    q = *s;
    if (q != '"' && q != '\'') {
      v = s;
      while (s < end && *s != ' ' && *s != '\t' && *s != '\n') s++;
      vl = (size_t)(s - v);
    }
    else {
      v = ++s;
      while (s < end && *s != q) s++;
      vl = (size_t)(s - v);
      if (s < end) s++;
    }
    if (kl == nl && memcmp(k, name, nl) == 0) {
      if (vl >= n) vl = n - 1;
      memcpy(out, v, vl);
      out[vl] = '\0';
      return 1;
    }
  }
  return 0;
}


/* a CSS declaration list ("fill:#fff;stroke:none") has name: its value into out */
static int decl (const char *s, const char *name, char *out, size_t n) {
  size_t nl = strlen(name);
  while (s && *s) {
    const char *k, *v;
    size_t kl, vl;
    while (*s == ' ' || *s == ';' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    k = s;
    while (*s && *s != ':' && *s != ';') s++;
    kl = (size_t)(s - k);
    while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) kl--;
    if (*s != ':') continue;
    v = ++s;
    while (*v == ' ') v++;
    while (*s && *s != ';') s++;
    vl = (size_t)(s - v);
    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\n' || v[vl - 1] == '\r')) vl--;
    if (kl == nl && memcmp(k, name, nl) == 0) {
      if (vl >= n) vl = n - 1;
      memcpy(out, v, vl);
      out[vl] = '\0';
      if (vl >= 10 && strcmp(out + vl - 10, "!important") == 0) {
        out[vl - 10] = '\0';
        while (vl > 10 && out[vl - 11] == ' ') out[--vl - 10] = '\0';
      }
      return 1;
    }
  }
  return 0;
}


static const char *skip_sep (const char *s) {
  while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r') s++;
  return s;
}


/* a number at *s (SVG's: "-.5.5" is two, "1e-3"); 0: none there */
static int num (const char **s, double *v) {
  const char *p = skip_sep(*s), *q = p;
  int dot = 0, digits = 0;
  if (*q == '+' || *q == '-') q++;
  while ((*q >= '0' && *q <= '9') || (*q == '.' && !dot)) {
    if (*q == '.') dot = 1;
    else digits = 1;
    q++;
  }
  if (!digits) return 0;
  if ((*q == 'e' || *q == 'E') && (q[1] == '-' || q[1] == '+' || (q[1] >= '0' && q[1] <= '9'))) {
    q += 2;
    while (*q >= '0' && *q <= '9') q++;
  }
  *v = strtod(p, NULL);
  if (!isfinite(*v)) *v = *v < 0 ? -1e30 : 1e30;	/* "1e999": inf - inf would be NaN */
  *s = q;
  return 1;
}


/* a length ("12", "12px", "50%" of whole); def when there is none */
static double length (const char *s, double whole, double def) {
  const char *p = s;
  double v;
  if (s == NULL || !num(&p, &v)) return def;
  if (*p == '%') return v * whole / 100.0;
  return v;
}


static int hexv (int c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}


/* a CSS color as 0xAARRGGBB; 0: not one ("none", "url(...)") */
static int color (const char *s, uint32_t *out) {
  static const struct {
    const char *name;
    uint32_t rgb;
  } named[] = {
    {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000}, {"blue", 0x0000FF},
    {"yellow", 0xFFFF00}, {"orange", 0xFFA500}, {"purple", 0x800080}, {"gray", 0x808080}, {"grey", 0x808080},
    {"silver", 0xC0C0C0}, {"navy", 0x000080}, {"teal", 0x008080}, {"maroon", 0x800000}, {"lime", 0x00FF00},
    {"aqua", 0x00FFFF}, {"cyan", 0x00FFFF}, {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF}, {"olive", 0x808000},
    {"brown", 0xA52A2A}, {"pink", 0xFFC0CB}, {"gold", 0xFFD700}, {"darkgray", 0xA9A9A9}, {"lightgray", 0xD3D3D3},
    {"darkblue", 0x00008B}, {"darkgreen", 0x006400}, {"darkred", 0x8B0000}, {"whitesmoke", 0xF5F5F5},
    {"currentColor", 0xC5C5C5}	/* the icons' "text" color: a light gray reads on dark and light */
  };
  size_t i, n;
  while (*s == ' ') s++;
  n = strlen(s);
  if (s[0] == '#') {
    int d[8], k;
    for (k = 0; k < 8 && hexv(s[1 + k]) >= 0; k++) d[k] = hexv(s[1 + k]);
    if (k == 3 || k == 4) {
      *out = (uint32_t)((k == 4 ? d[3] * 17 : 255) << 24 | (d[0] * 17) << 16 | (d[1] * 17) << 8 | d[2] * 17);
      return 1;
    }
    if (k == 6 || k == 8) {
      *out = (uint32_t)((k == 8 ? d[6] * 16 + d[7] : 255) << 24 | (d[0] * 16 + d[1]) << 16 | (d[2] * 16 + d[3]) << 8 |
                        (d[4] * 16 + d[5]));
      return 1;
    }
    return 0;
  }
  if (strncmp(s, "rgb", 3) == 0) {
    const char *p = strchr(s, '(');
    double c[4] = {0, 0, 0, 1};
    int k;
    if (p == NULL) return 0;
    p++;
    for (k = 0; k < 4; k++) {
      p = skip_sep(p);
      if (*p == '/') p++;
      if (!num(&p, &c[k])) break;
      if (*p == '%') {
        c[k] = k < 3 ? c[k] * 2.55 : c[k] / 100.0;
        p++;
      }
    }
    if (k < 3) return 0;
    for (k = 0; k < 3; k++) c[k] = c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k];
    c[3] = c[3] < 0 ? 0 : c[3] > 1 ? 1 : c[3];
    *out = (uint32_t)((int)(c[3] * 255 + 0.5) << 24 | (int)c[0] << 16 | (int)c[1] << 8 | (int)c[2]);
    return 1;
  }
  for (i = 0; i < sizeof(named) / sizeof(named[0]); i++)
    if (strlen(named[i].name) == n && m_strnicmp(s, named[i].name, n) == 0) {
      *out = 0xFF000000u | named[i].rgb;
      return 1;
    }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Transforms
** ===================================================================
*/

typedef struct Mat {
  double a, b, c, d, e, f;	/* x' = a x + c y + e, y' = b x + d y + f */
} Mat;

static const Mat IDENT = {1, 0, 0, 1, 0, 0};


static Mat mul (Mat m, Mat n) {	/* m after n: m * n */
  Mat r;
  r.a = m.a * n.a + m.c * n.b;
  r.b = m.b * n.a + m.d * n.b;
  r.c = m.a * n.c + m.c * n.d;
  r.d = m.b * n.c + m.d * n.d;
  r.e = m.a * n.e + m.c * n.f + m.e;
  r.f = m.b * n.e + m.d * n.f + m.f;
  return r;
}


static Mat inverse (Mat m) {
  double det = m.a * m.d - m.b * m.c;
  Mat r;
  if (fabs(det) < 1e-12) return IDENT;
  r.a = m.d / det;
  r.b = -m.b / det;
  r.c = -m.c / det;
  r.d = m.a / det;
  r.e = (m.c * m.f - m.d * m.e) / det;
  r.f = (m.b * m.e - m.a * m.f) / det;
  return r;
}


/* "translate(2 4.5)scale(.2) rotate(45 16 16) matrix(...)" */
static Mat parse_transform (const char *s) {
  Mat m = IDENT;
  while (s && *s) {
    const char *name = skip_sep(s), *p;
    double v[6] = {0, 0, 0, 0, 0, 0};
    int k = 0;
    Mat t = IDENT;
    size_t nl;
    p = name;
    while (*p >= 'a' && *p <= 'z') p++;
    nl = (size_t)(p - name);
    while (*p == ' ') p++;
    if (nl == 0 || *p != '(') break;
    p++;
    while (k < 6 && num(&p, &v[k])) k++;
    p = skip_sep(p);
    if (*p == ')') p++;
    s = p;
    if (nl == 6 && memcmp(name, "matrix", 6) == 0 && k == 6) {
      t.a = v[0];
      t.b = v[1];
      t.c = v[2];
      t.d = v[3];
      t.e = v[4];
      t.f = v[5];
    }
    else if (nl == 9 && memcmp(name, "translate", 9) == 0) {
      t.e = v[0];
      t.f = k > 1 ? v[1] : 0;
    }
    else if (nl == 5 && memcmp(name, "scale", 5) == 0) {
      t.a = v[0];
      t.d = k > 1 ? v[1] : v[0];
    }
    else if (nl == 6 && memcmp(name, "rotate", 6) == 0) {
      double r = v[0] * 3.14159265358979323846 / 180.0;
      Mat rot = {cos(r), sin(r), -sin(r), cos(r), 0, 0};
      if (k == 3) {
        Mat a = {1, 0, 0, 1, v[1], v[2]}, b = {1, 0, 0, 1, -v[1], -v[2]};
        t = mul(a, mul(rot, b));
      }
      else t = rot;
    }
    else if (nl == 5 && memcmp(name, "skewX", 5) == 0) t.c = tan(v[0] * 3.14159265358979323846 / 180.0);
    else if (nl == 5 && memcmp(name, "skewY", 5) == 0) t.b = tan(v[0] * 3.14159265358979323846 / 180.0);
    m = mul(m, t);
  }
  return m;
}

/* }================================================================== */


/*
** {==================================================================
** Polygons: a shape flattened, in its own coordinates
** ===================================================================
*/

typedef struct Poly {
  double *pt;	/* x, y pairs */
  size_t n, cap;
  size_t *start;	/* where each contour starts */
  int *closed;
  size_t nc, capc;
} Poly;


static void poly_free (Poly *p) {
  free(p->pt);
  free(p->start);
  free(p->closed);
  memset(p, 0, sizeof(*p));
}


static void poly_move (Poly *p) {
  if (p->nc && p->start[p->nc - 1] == p->n) return;	/* an empty one: reused */
  if (p->nc == p->capc) {
    p->capc = p->capc ? p->capc * 2 : 8;
    p->start = (size_t *)xrealloc(p->start, p->capc * sizeof(size_t));
    p->closed = (int *)xrealloc(p->closed, p->capc * sizeof(int));
  }
  p->start[p->nc] = p->n;
  p->closed[p->nc++] = 0;
}


static void poly_add (Poly *p, double x, double y) {
  if (p->nc == 0) poly_move(p);
  if (p->n == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 64;
    p->pt = (double *)xrealloc(p->pt, p->cap * 2 * sizeof(double));
  }
  p->pt[p->n * 2] = x;
  p->pt[p->n * 2 + 1] = y;
  p->n++;
}


static void poly_close (Poly *p) {
  if (p->nc) p->closed[p->nc - 1] = 1;
}


static size_t contour_end (const Poly *p, size_t c) {
  return c + 1 < p->nc ? p->start[c + 1] : p->n;
}


/* a cubic Bézier from (x0, y0), in steps fine enough at scale */
static void cubic (Poly *p, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, double scale) {
  double len = (hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) + hypot(x3 - x2, y3 - y2)) * scale;
  int n = len < 93 ? (int)(len / 1.5) + 2 : 64, i;	/* NaN too: (int) of it is undefined */
  if (n > 64) n = 64;
  for (i = 1; i <= n; i++) {
    double t = (double)i / n, u = 1 - t;
    poly_add(p, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
             u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
  }
}


static void quad (Poly *p, double x0, double y0, double x1, double y1, double x2, double y2, double scale) {
  cubic(p, x0, y0, x0 + 2.0 / 3.0 * (x1 - x0), y0 + 2.0 / 3.0 * (y1 - y0), x2 + 2.0 / 3.0 * (x1 - x2), y2 + 2.0 / 3.0 * (y1 - y2), x2,
        y2, scale);
}


/* an elliptical arc (SVG's endpoint form) from (x0, y0) to (x, y) */
static void arc (Poly *p, double x0, double y0, double rx, double ry, double rot, int large, int sweep, double x, double y,
                 double scale) {
  const double PI = 3.14159265358979323846;
  double phi = rot * PI / 180.0, cp = cos(phi), sp = sin(phi);
  double dx = (x0 - x) / 2, dy = (y0 - y) / 2, x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
  double lam, co, cx1, cy1, cx, cy, t1, dt;
  int n, i;
  rx = fabs(rx);
  ry = fabs(ry);
  if (rx < 1e-9 || ry < 1e-9) {
    poly_add(p, x, y);
    return;
  }
  lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
  if (lam > 1) {
    rx *= sqrt(lam);
    ry *= sqrt(lam);
  }
  {
    double num_ = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    co = den > 0 && num_ > 0 ? sqrt(num_ / den) : 0;
  }
  if (large == sweep) co = -co;
  cx1 = co * rx * y1 / ry;
  cy1 = -co * ry * x1 / rx;
  cx = cp * cx1 - sp * cy1 + (x0 + x) / 2;
  cy = sp * cx1 + cp * cy1 + (y0 + y) / 2;
  t1 = atan2((y1 - cy1) / ry, (x1 - cx1) / rx);
  dt = atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx) - t1;
  if (sweep && dt < 0) dt += 2 * PI;
  else if (!sweep && dt > 0) dt -= 2 * PI;
  lam = fabs(dt) * (rx > ry ? rx : ry) * scale / 1.5;
  n = lam < 88 ? (int)lam + 2 : 90;	/* NaN too */
  if (n > 90) n = 90;
  for (i = 1; i <= n; i++) {
    double t = t1 + dt * i / n, ex = rx * cos(t), ey = ry * sin(t);
    poly_add(p, cp * ex - sp * ey + cx, sp * ex + cp * ey + cy);
  }
}


/* a path's d */
static void path_d (Poly *p, const char *s, double scale) {
  double cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0;	/* current, subpath start, last control */
  char cmd = 0, last = 0;
  for (;;) {
    double v[7];
    int rel, k, need;
    char c;
    s = skip_sep(s);
    if (*s == '\0') break;
    if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) cmd = *s++;
    else if (cmd == 0) break;
    rel = cmd >= 'a';
    c = (char)(rel ? cmd - 32 : cmd);
    need = c == 'M' || c == 'L' || c == 'T' ? 2 : c == 'H' || c == 'V' ? 1 : c == 'C' ? 6 : c == 'S' || c == 'Q' ? 4 : c == 'A' ? 7 : 0;
    if (c == 'Z') {
      poly_close(p);
      cx = sx;
      cy = sy;
      last = 'Z';
      poly_move(p);
      continue;
    }
    for (k = 0; k < need; k++) {
      if (c == 'A' && (k == 3 || k == 4)) {	/* the flags: one digit each, maybe with nothing between */
        s = skip_sep(s);
        if (*s != '0' && *s != '1') break;
        v[k] = *s++ - '0';
      }
      else if (!num(&s, &v[k])) break;
    }
    if (k < need) break;
    switch (c) {
      case 'M':
        cx = rel ? cx + v[0] : v[0];
        cy = rel ? cy + v[1] : v[1];
        poly_move(p);
        poly_add(p, cx, cy);
        sx = cx;
        sy = cy;
        cmd = rel ? 'l' : 'L';	/* more pairs after it: lines */
        break;
      case 'L':
        cx = rel ? cx + v[0] : v[0];
        cy = rel ? cy + v[1] : v[1];
        poly_add(p, cx, cy);
        break;
      case 'H':
        cx = rel ? cx + v[0] : v[0];
        poly_add(p, cx, cy);
        break;
      case 'V':
        cy = rel ? cy + v[0] : v[0];
        poly_add(p, cx, cy);
        break;
      case 'C': {
        double x1 = rel ? cx + v[0] : v[0], y1 = rel ? cy + v[1] : v[1], x2 = rel ? cx + v[2] : v[2], y2 = rel ? cy + v[3] : v[3];
        double x = rel ? cx + v[4] : v[4], y = rel ? cy + v[5] : v[5];
        cubic(p, cx, cy, x1, y1, x2, y2, x, y, scale);
        lx = x2;
        ly = y2;
        cx = x;
        cy = y;
        break;
      }
      case 'S': {
        double x1 = last == 'C' || last == 'S' ? 2 * cx - lx : cx, y1 = last == 'C' || last == 'S' ? 2 * cy - ly : cy;
        double x2 = rel ? cx + v[0] : v[0], y2 = rel ? cy + v[1] : v[1], x = rel ? cx + v[2] : v[2], y = rel ? cy + v[3] : v[3];
        cubic(p, cx, cy, x1, y1, x2, y2, x, y, scale);
        lx = x2;
        ly = y2;
        cx = x;
        cy = y;
        break;
      }
      case 'Q': {
        double x1 = rel ? cx + v[0] : v[0], y1 = rel ? cy + v[1] : v[1], x = rel ? cx + v[2] : v[2], y = rel ? cy + v[3] : v[3];
        quad(p, cx, cy, x1, y1, x, y, scale);
        lx = x1;
        ly = y1;
        cx = x;
        cy = y;
        break;
      }
      case 'T': {
        double x1 = last == 'Q' || last == 'T' ? 2 * cx - lx : cx, y1 = last == 'Q' || last == 'T' ? 2 * cy - ly : cy;
        double x = rel ? cx + v[0] : v[0], y = rel ? cy + v[1] : v[1];
        quad(p, cx, cy, x1, y1, x, y, scale);
        lx = x1;
        ly = y1;
        cx = x;
        cy = y;
        break;
      }
      case 'A': {
        double x = rel ? cx + v[5] : v[5], y = rel ? cy + v[6] : v[6];
        arc(p, cx, cy, v[0], v[1], v[2], v[3] != 0, v[4] != 0, x, y, scale);
        cx = x;
        cy = y;
        break;
      }
      default:
        return;
    }
    last = c;
  }
}


/* an ellipse (a circle when rx == ry) */
static void ellipse (Poly *p, double cx, double cy, double rx, double ry, double scale) {
  double v = (rx > ry ? rx : ry) * scale * 1.2;
  int n = v < 108 ? (v > -12 ? (int)v + 12 : 0) : 120, i;	/* NaN too */
  if (n > 120) n = 120;
  poly_move(p);
  for (i = 0; i < n; i++) {
    double t = 6.283185307179586 * i / n;
    poly_add(p, cx + rx * cos(t), cy + ry * sin(t));
  }
  poly_close(p);
}


static void rect (Poly *p, double x, double y, double w, double h, double rx, double ry, double scale) {
  if (rx > w / 2) rx = w / 2;
  if (ry > h / 2) ry = h / 2;
  poly_move(p);
  if (rx <= 0 || ry <= 0) {
    poly_add(p, x, y);
    poly_add(p, x + w, y);
    poly_add(p, x + w, y + h);
    poly_add(p, x, y + h);
  }
  else {	/* the corners: quarter ellipses */
    poly_add(p, x + rx, y);
    poly_add(p, x + w - rx, y);
    arc(p, x + w - rx, y, rx, ry, 0, 0, 1, x + w, y + ry, scale);
    poly_add(p, x + w, y + h - ry);
    arc(p, x + w, y + h - ry, rx, ry, 0, 0, 1, x + w - rx, y + h, scale);
    poly_add(p, x + rx, y + h);
    arc(p, x + rx, y + h, rx, ry, 0, 0, 1, x, y + h - ry, scale);
    poly_add(p, x, y + ry);
    arc(p, x, y + ry, rx, ry, 0, 0, 1, x + rx, y, scale);
  }
  poly_close(p);
}


/* a stroke as polygons (one each segment, a disc at each joint, all turning the same way: nonzero joins them) */
static void stroke_of (const Poly *in, Poly *out, double width, double scale) {
  double r = width / 2;
  size_t c;
  if (r <= 0) return;
  for (c = 0; c < in->nc; c++) {
    size_t s = in->start[c], e = contour_end(in, c), i, n = e - s, segs;
    if (n < 2) continue;
    segs = in->closed[c] ? n : n - 1;
    for (i = 0; i < segs; i++) {
      const double *a = &in->pt[(s + i) * 2], *b = &in->pt[(s + (i + 1) % n) * 2];
      double dx = b[0] - a[0], dy = b[1] - a[1], l = hypot(dx, dy), nx, ny;
      if (l < 1e-9) continue;
      nx = -dy / l * r;
      ny = dx / l * r;
      poly_move(out);
      poly_add(out, a[0] + nx, a[1] + ny);
      poly_add(out, a[0] - nx, a[1] - ny);
      poly_add(out, b[0] - nx, b[1] - ny);
      poly_add(out, b[0] + nx, b[1] + ny);
      poly_close(out);
    }
    for (i = 0; i < n; i++) {	/* round joints (and caps) */
      if (!in->closed[c] && (i == 0 || i == n - 1) && r * scale < 0.75) continue;
      ellipse(out, in->pt[(s + i) * 2], in->pt[(s + i) * 2 + 1], r, r, scale);
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** Filling: coverage of a polygon in the picture
** ===================================================================
*/

#define SUB 4	/* samples down a pixel */

typedef struct Edge {
  double x0, y0, x1, y1;
  int dir;
} Edge;


/* the polygon's coverage (0 .. 1 each pixel) into cov (w * h), by m; evenodd or nonzero */
static void fill_poly (const Poly *p, Mat m, int evenodd, int w, int h, float *cov) {
  Edge *e;
  size_t ne = 0, c, i;
  double ymin = 1e30, ymax = -1e30;
  float *row;
  int y;
  if (p->n < 3) return;
  e = (Edge *)xmalloc((p->n + p->nc) * sizeof(Edge));
  for (c = 0; c < p->nc; c++) {
    size_t s = p->start[c], en = contour_end(p, c), n = en - s;
    if (n < 3) continue;
    for (i = 0; i < n; i++) {	/* every contour is closed for filling */
      const double *a = &p->pt[(s + i) * 2], *b = &p->pt[(s + (i + 1) % n) * 2];
      double ax = m.a * a[0] + m.c * a[1] + m.e, ay = m.b * a[0] + m.d * a[1] + m.f;
      double bx = m.a * b[0] + m.c * b[1] + m.e, by = m.b * b[0] + m.d * b[1] + m.f;
      if (ay == by || !isfinite(ax) || !isfinite(ay) || !isfinite(bx) || !isfinite(by)) continue;
      if (ay < by) {
        e[ne].x0 = ax, e[ne].y0 = ay, e[ne].x1 = bx, e[ne].y1 = by, e[ne].dir = 1;
      }
      else {
        e[ne].x0 = bx, e[ne].y0 = by, e[ne].x1 = ax, e[ne].y1 = ay, e[ne].dir = -1;
      }
      if (e[ne].y0 < ymin) ymin = e[ne].y0;
      if (e[ne].y1 > ymax) ymax = e[ne].y1;
      ne++;
    }
  }
  if (ne == 0 || ymin >= h || ymax <= 0) {	/* nothing in the picture ((int) of a huge ymin is undefined) */
    free(e);
    return;
  }
  row = (float *)xmalloc((size_t)(w + 2) * sizeof(float));
  {
    double *xs = (double *)xmalloc((ne + 1) * sizeof(double));
    int *ds = (int *)xmalloc((ne + 1) * sizeof(int));
    int y0 = ymin < 0 ? 0 : (int)ymin, y1 = ymax > h ? h : (int)ceil(ymax);
    for (y = y0; y < y1; y++) {
      int sub, any = 0, x;
      memset(row, 0, (size_t)(w + 2) * sizeof(float));
      for (sub = 0; sub < SUB; sub++) {
        double sy = y + (sub + 0.5) / SUB;
        size_t nx = 0, a, b;
        int wind = 0;
        for (i = 0; i < ne; i++)
          if (sy >= e[i].y0 && sy < e[i].y1) {
            double t = (sy - e[i].y0) / (e[i].y1 - e[i].y0);
            xs[nx] = e[i].x0 + t * (e[i].x1 - e[i].x0);
            if (xs[nx] != xs[nx]) continue;	/* NaN (inf - inf): no crossing */
            ds[nx++] = e[i].dir;
          }
        for (a = 1; a < nx; a++)	/* few: insertion sort */
          for (b = a; b > 0 && xs[b - 1] > xs[b]; b--) {
            double tx = xs[b];
            int td = ds[b];
            xs[b] = xs[b - 1];
            ds[b] = ds[b - 1];
            xs[b - 1] = tx;
            ds[b - 1] = td;
          }
        for (a = 0; a + 1 <= nx; a++) {
          int in;
          wind += evenodd ? 1 : ds[a];
          in = evenodd ? (wind & 1) : wind != 0;
          if (in && a + 1 < nx) {	/* xs[a] .. xs[a + 1] is inside: its coverage, exact at the ends */
            double l = xs[a] < 0 ? 0 : xs[a], r = xs[a + 1] > w ? w : xs[a + 1];
            int li, ri;
            if (!(r > l)) continue;	/* NaN too */
            li = (int)l;	/* 0 <= l < r <= w */
            ri = (int)r;
            any = 1;
            if (li == ri) row[li] += (float)((r - l) / SUB);
            else {
              row[li] += (float)((li + 1 - l) / SUB);
              for (x = li + 1; x < ri; x++) row[x] += 1.0f / SUB;
              if (ri < w) row[ri] += (float)((r - ri) / SUB);
            }
          }
        }
      }
      if (any)
        for (x = 0; x < w; x++) {
          float v = row[x] > 1 ? 1 : row[x];
          if (v > 0) {	/* shapes of one fill: their union */
            float *o = &cov[(size_t)y * (size_t)w + (size_t)x];
            *o = *o + v - *o * v;
          }
        }
    }
    free(xs);
    free(ds);
  }
  free(row);
  free(e);
}

/* }================================================================== */


/*
** {==================================================================
** The document: styles, gradients, ids; drawing it
** ===================================================================
*/

typedef struct Stop {
  double off;
  uint32_t argb;
} Stop;

typedef struct Grad {
  char id[64];
  int radial, user;	/* userSpaceOnUse (else the shape's box) */
  double v[5];	/* x1 y1 x2 y2, or cx cy r (fx fy not used) */
  int has[5];
  Mat tr;
  Stop *stop;
  int nstop;
  char href[64];
} Grad;

typedef struct Rule {
  char sel[64];	/* ".cls-1" or "path" */
  char *body;
} Rule;

typedef struct Doc {
  const char *src, *end;
  Grad *grad;
  int ngrad;
  Rule *rule;
  int nrule;
  struct {
    char id[64];
    const char *at;	/* its tag in the source */
  } *ids;
  int nids;
  int w, h;
  float *px;	/* premultiplied r g b a, w * h * 4 */
  float *cov;	/* a shape's coverage, w * h */
  int depth;	/* <use> in <use>: kept from going round */
  long left;	/* the elements still to be drawn: <use>s of <use>s multiply them */
  int clipping;	/* a clip's shapes are drawn: their geometry only, filled white */
} Doc;

typedef struct Style {
  char fill[96], stroke[96];
  double fill_op, stroke_op, op, stroke_w;
  int evenodd, hidden;
  Mat m;
  float *clip;	/* NULL: none; else w * h, 0 .. 1 */
} Style;


/* the property of the element: its style="", its classes' rules, its attribute; the inherited value else */
static int prop (const Doc *d, const Tag *t, const char *name, char *out, size_t n) {
  char buf[1024], cls[256];
  int k;
  if (attr(t, "style", buf, sizeof(buf)) && decl(buf, name, out, n)) return 1;
  if (attr(t, "class", cls, sizeof(cls))) {
    const char *s = cls;
    while (*s) {
      const char *c;
      size_t cl;
      while (*s == ' ') s++;
      c = s;
      while (*s && *s != ' ') s++;
      cl = (size_t)(s - c);
      for (k = d->nrule - 1; cl && k >= 0; k--)
        if (d->rule[k].sel[0] == '.' && strlen(d->rule[k].sel + 1) == cl && memcmp(d->rule[k].sel + 1, c, cl) == 0 &&
            decl(d->rule[k].body, name, out, n))
          return 1;
    }
  }
  if (attr(t, name, out, n)) return 1;
  for (k = d->nrule - 1; k >= 0; k--)	/* "path{fill:...}" */
    if (d->rule[k].sel[0] != '.' && strlen(d->rule[k].sel) == t->nname && memcmp(d->rule[k].sel, t->name, t->nname) == 0 &&
        decl(d->rule[k].body, name, out, n))
      return 1;
  return 0;
}


static double opacity_of (const char *s) {
  const char *p = s;
  double v;
  if (!num(&p, &v)) return 1;
  if (*p == '%') v /= 100;
  return v < 0 ? 0 : v > 1 ? 1 : v;
}


/* the style of element t over the inherited one */
static void style_of (const Doc *d, const Tag *t, const Style *up, Style *s) {
  char v[128];
  *s = *up;
  s->op = 1;	/* not inherited: the group's is applied to what it has, as a factor */
  if (prop(d, t, "fill", v, sizeof(v))) snprintf(s->fill, sizeof(s->fill), "%s", v);
  if (prop(d, t, "stroke", v, sizeof(v))) snprintf(s->stroke, sizeof(s->stroke), "%s", v);
  if (prop(d, t, "fill-opacity", v, sizeof(v))) s->fill_op = opacity_of(v);
  if (prop(d, t, "stroke-opacity", v, sizeof(v))) s->stroke_op = opacity_of(v);
  if (prop(d, t, "stroke-width", v, sizeof(v))) s->stroke_w = length(v, 1, 1);
  if (prop(d, t, "fill-rule", v, sizeof(v))) s->evenodd = strcmp(v, "evenodd") == 0;
  if (prop(d, t, "opacity", v, sizeof(v))) s->op = opacity_of(v);
  s->op *= up->op;
  if ((prop(d, t, "display", v, sizeof(v)) && strcmp(v, "none") == 0) ||
      (prop(d, t, "visibility", v, sizeof(v)) && strcmp(v, "hidden") == 0))
    s->hidden = 1;
  {
    char big[1024];
    if (attr(t, "transform", big, sizeof(big))) s->m = mul(up->m, parse_transform(big));
  }
}


static const Grad *grad_by (const Doc *d, const char *id) {
  int i;
  for (i = 0; i < d->ngrad; i++)
    if (strcmp(d->grad[i].id, id) == 0) return &d->grad[i];
  return NULL;
}


static const char *id_at (const Doc *d, const char *id) {
  int i;
  for (i = 0; i < d->nids; i++)
    if (strcmp(d->ids[i].id, id) == 0) return d->ids[i].at;
  return NULL;
}


/* "url(#a)" or "#a": "a" into out */
static int url_id (const char *s, char *out, size_t n) {
  const char *h = strchr(s, '#'), *e;
  if (h == NULL) return 0;
  h++;
  for (e = h; *e && *e != ')' && *e != '\'' && *e != '"' && *e != ' '; e++) {}
  if ((size_t)(e - h) >= n) return 0;
  memcpy(out, h, (size_t)(e - h));
  out[e - h] = '\0';
  return 1;
}


/* the color of a gradient at t (0 .. 1) */
static uint32_t grad_at (const Grad *g, const Stop *st, int n, double t) {
  int i;
  (void)g;
  if (n == 0) return 0;
  if (t <= st[0].off) return st[0].argb;
  for (i = 1; i < n; i++)
    if (t <= st[i].off) {
      double span = st[i].off - st[i - 1].off, f = span > 1e-9 ? (t - st[i - 1].off) / span : 1;
      uint32_t a = st[i - 1].argb, b = st[i].argb, r = 0;
      int sh;
      for (sh = 0; sh < 32; sh += 8) {
        double ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        r |= (uint32_t)(int)(ca + (cb - ca) * f + 0.5) << sh;
      }
      return r;
    }
  return st[n - 1].argb;
}


/* paints the coverage in d->cov with paint (a color or url(#gradient)) at alpha op; box: the shape's box in its
** coordinates (for objectBoundingBox gradients), m: its transform */
static void paint (Doc *d, const char *pnt, double op, const double box[4], Mat m, const float *clip) {
  uint32_t argb = 0;
  const Grad *g = NULL;
  const Stop *st = NULL;
  int nst = 0, x, y;
  Mat inv = IDENT;
  char id[64];
  if (strncmp(pnt, "url(", 4) == 0) {
    if (!url_id(pnt, id, sizeof(id)) || (g = grad_by(d, id)) == NULL) {	/* "url(#x) red": the fallback */
      const char *fb = strchr(pnt, ')');
      if (fb == NULL || !color(fb + 1, &argb)) return;
      g = NULL;
    }
    else {
      const Grad *sg = g;
      int hops = 0;
      while (sg && sg->nstop == 0 && sg->href[0] && hops++ < 8) sg = grad_by(d, sg->href);	/* its stops from another */
      if (sg == NULL || sg->nstop == 0) return;
      st = sg->stop;
      nst = sg->nstop;
      {	/* device -> the gradient's space */
        Mat to = m;
        if (!g->user) {
          Mat bb = {box[2] - box[0], 0, 0, box[3] - box[1], box[0], box[1]};
          to = mul(m, bb);
        }
        inv = inverse(mul(to, g->tr));
      }
    }
  }
  else if (!color(pnt, &argb)) return;
  for (y = 0; y < d->h; y++)
    for (x = 0; x < d->w; x++) {
      size_t k = (size_t)y * (size_t)d->w + (size_t)x;
      float c = d->cov[k];
      double a;
      uint32_t col = argb;
      float *o;
      if (c <= 0) continue;
      if (clip) c *= clip[k];
      if (g) {
        double px = x + 0.5, py = y + 0.5, gx = inv.a * px + inv.c * py + inv.e, gy = inv.b * px + inv.d * py + inv.f, t;
        if (g->radial) {
          double cx = g->has[0] ? g->v[0] : 0.5, cy = g->has[1] ? g->v[1] : 0.5, r = g->has[2] ? g->v[2] : 0.5;
          t = r > 1e-9 ? hypot(gx - cx, gy - cy) / r : 1;
        }
        else {
          double x1 = g->has[0] ? g->v[0] : 0, y1 = g->has[1] ? g->v[1] : 0, x2 = g->has[2] ? g->v[2] : 1,
                 y2 = g->has[3] ? g->v[3] : 0, dx = x2 - x1, dy = y2 - y1, l2 = dx * dx + dy * dy;
          t = l2 > 1e-12 ? ((gx - x1) * dx + (gy - y1) * dy) / l2 : 0;
        }
        col = grad_at(g, st, nst, t < 0 ? 0 : t > 1 ? 1 : t);
      }
      a = c * op * ((col >> 24) & 255) / 255.0;
      if (a <= 0) continue;
      o = &d->px[k * 4];	/* source over, premultiplied */
      o[0] = (float)(((col >> 16) & 255) / 255.0 * a + o[0] * (1 - a));
      o[1] = (float)(((col >> 8) & 255) / 255.0 * a + o[1] * (1 - a));
      o[2] = (float)((col & 255) / 255.0 * a + o[2] * (1 - a));
      o[3] = (float)(a + o[3] * (1 - a));
    }
}


static double scale_of (Mat m) {
  return sqrt(fabs(m.a * m.d - m.b * m.c));
}


/* the geometry of a shape element into p; 0: not a shape */
static int shape (const Tag *t, Poly *p, double scale) {
  char v[64];
  double x, y, w, h;
  if (tag_is(t, "path")) {
    size_t cap = t->nattrs + 1;
    char *dd = (char *)xmalloc(cap);
    if (attr(t, "d", dd, cap)) path_d(p, dd, scale);
    free(dd);
    return 1;
  }
  if (tag_is(t, "rect")) {
    double rx = -1, ry = -1;
    x = attr(t, "x", v, sizeof(v)) ? length(v, 0, 0) : 0;
    y = attr(t, "y", v, sizeof(v)) ? length(v, 0, 0) : 0;
    w = attr(t, "width", v, sizeof(v)) ? length(v, 0, 0) : 0;
    h = attr(t, "height", v, sizeof(v)) ? length(v, 0, 0) : 0;
    if (attr(t, "rx", v, sizeof(v))) rx = length(v, 0, 0);
    if (attr(t, "ry", v, sizeof(v))) ry = length(v, 0, 0);
    if (rx < 0) rx = ry;
    if (ry < 0) ry = rx;
    if (w > 0 && h > 0) rect(p, x, y, w, h, rx < 0 ? 0 : rx, ry < 0 ? 0 : ry, scale);
    return 1;
  }
  if (tag_is(t, "circle") || tag_is(t, "ellipse")) {
    double rx, ry;
    x = attr(t, "cx", v, sizeof(v)) ? length(v, 0, 0) : 0;
    y = attr(t, "cy", v, sizeof(v)) ? length(v, 0, 0) : 0;
    if (tag_is(t, "circle")) rx = ry = attr(t, "r", v, sizeof(v)) ? length(v, 0, 0) : 0;
    else {
      rx = attr(t, "rx", v, sizeof(v)) ? length(v, 0, 0) : 0;
      ry = attr(t, "ry", v, sizeof(v)) ? length(v, 0, 0) : 0;
    }
    if (rx > 0 && ry > 0) ellipse(p, x, y, rx, ry, scale);
    return 1;
  }
  if (tag_is(t, "line")) {
    double x2, y2;
    x = attr(t, "x1", v, sizeof(v)) ? length(v, 0, 0) : 0;
    y = attr(t, "y1", v, sizeof(v)) ? length(v, 0, 0) : 0;
    x2 = attr(t, "x2", v, sizeof(v)) ? length(v, 0, 0) : 0;
    y2 = attr(t, "y2", v, sizeof(v)) ? length(v, 0, 0) : 0;
    poly_move(p);
    poly_add(p, x, y);
    poly_add(p, x2, y2);
    return 1;
  }
  if (tag_is(t, "polygon") || tag_is(t, "polyline")) {
    size_t cap = t->nattrs + 1;
    char *pts = (char *)xmalloc(cap);
    if (attr(t, "points", pts, cap)) {
      const char *s = pts;
      double a, b;
      poly_move(p);
      while (num(&s, &a) && num(&s, &b)) poly_add(p, a, b);
      if (tag_is(t, "polygon")) poly_close(p);
    }
    free(pts);
    return 1;
  }
  return 0;
}


static void draw_from (Doc *d, const char *at, const Style *up, int one);


/* a clip-path (or a mask, taken as one) of the element: the coverage of its shapes, times the one above */
static float *clip_of (Doc *d, const Tag *t, const Style *s) {
  char v[128], id[64];
  const char *at;
  Tag ct;
  float *clip, *keep_cov;
  float *keep_px;
  Style cs;
  size_t n = (size_t)d->w * (size_t)d->h, i;
  if (!(prop(d, t, "clip-path", v, sizeof(v)) || prop(d, t, "mask", v, sizeof(v))) || !url_id(v, id, sizeof(id)) ||
      (at = id_at(d, id)) == NULL || d->depth > 6)
    return NULL;
  {
    const char *p = at;
    if (!next_tag(&p, d->end, &ct) || ct.empty) return NULL;
  }
  clip = (float *)zalloc(n, sizeof(float));
  keep_cov = d->cov;	/* its shapes drawn white into a picture of their own: its alpha is the clip */
  keep_px = d->px;
  d->px = (float *)zalloc(n * 4, sizeof(float));
  d->cov = (float *)zalloc(n, sizeof(float));
  cs = *s;
  cs.clip = NULL;
  cs.op = 1;
  snprintf(cs.fill, sizeof(cs.fill), "#fff");
  cs.stroke[0] = '\0';
  cs.fill_op = 1;
  d->depth++;
  d->clipping++;
  draw_from(d, at, &cs, 1);	/* the <clipPath> as a group: its transform, its children */
  d->clipping--;
  d->depth--;
  for (i = 0; i < n; i++) clip[i] = d->px[i * 4 + 3] * (s->clip ? s->clip[i] : 1.0f);
  free(d->px);
  free(d->cov);
  d->px = keep_px;
  d->cov = keep_cov;
  return clip;
}


/* one shape, filled and stroked */
static void draw_shape (Doc *d, const Tag *t, const Style *s) {
  Poly p;
  double box[4] = {1e30, 1e30, -1e30, -1e30}, sc = scale_of(s->m);
  size_t i, n = (size_t)d->w * (size_t)d->h;
  memset(&p, 0, sizeof(p));
  shape(t, &p, sc);
  if (p.n == 0) {
    poly_free(&p);
    return;
  }
  if (d->clipping) {
    memset(d->cov, 0, n * sizeof(float));
    fill_poly(&p, s->m, s->evenodd, d->w, d->h, d->cov);
    paint(d, "#fff", 1, box, s->m, NULL);
    poly_free(&p);
    return;
  }
  for (i = 0; i < p.n; i++) {
    double x = p.pt[i * 2], y = p.pt[i * 2 + 1];
    if (x < box[0]) box[0] = x;
    if (y < box[1]) box[1] = y;
    if (x > box[2]) box[2] = x;
    if (y > box[3]) box[3] = y;
  }
  if (strcmp(s->fill, "none") != 0 && !tag_is(t, "line") && !tag_is(t, "polyline")) {
    memset(d->cov, 0, n * sizeof(float));
    fill_poly(&p, s->m, s->evenodd, d->w, d->h, d->cov);
    paint(d, s->fill, s->op * s->fill_op, box, s->m, s->clip);
  }
  else if (tag_is(t, "polyline") && strcmp(s->fill, "none") != 0) {	/* a polyline's fill: as if closed */
    memset(d->cov, 0, n * sizeof(float));
    fill_poly(&p, s->m, s->evenodd, d->w, d->h, d->cov);
    paint(d, s->fill, s->op * s->fill_op, box, s->m, s->clip);
  }
  if (s->stroke[0] && strcmp(s->stroke, "none") != 0 && s->stroke_w > 0) {
    Poly sp;
    double w = s->stroke_w;
    if (w * sc < 0.6) w = 0.6 / sc;	/* a hairline still shows */
    memset(&sp, 0, sizeof(sp));
    stroke_of(&p, &sp, w, sc);
    memset(d->cov, 0, n * sizeof(float));
    fill_poly(&sp, s->m, 0, d->w, d->h, d->cov);
    paint(d, s->stroke, s->op * s->stroke_op, box, s->m, s->clip);
    poly_free(&sp);
  }
  poly_free(&p);
}


/* skips the element whose start tag was t (its children too) */
static void skip_elem (const Doc *d, const char **p, const Tag *t) {
  int depth = 1;
  Tag x;
  if (t->empty || t->close) return;
  while (depth > 0 && next_tag(p, d->end, &x)) {
    if (x.close) depth--;
    else if (!x.empty) depth++;
  }
}


/* the elements from at: the one there (one), or it and its children as a group drawn with up */
static void draw_from (Doc *d, const char *at, const Style *up, int one) {
  const char *p = at;
  Tag t;
  Style st[48];
  int sp = 0;
  st[0] = *up;
  while (next_tag(&p, d->end, &t)) {
    Style *s = &st[sp], ns;
    char v[256];
    if (d->left <= 0) break;	/* too many: the rest is left out, not drawn for minutes */
    d->left--;
    if (t.close) {
      if (sp > 0 && st[sp].clip && st[sp].clip != st[sp - 1].clip) free(st[sp].clip);
      if (--sp < 0) return;
      if (one && sp == 0) return;
      continue;
    }
    if (tag_is(&t, "defs") || tag_is(&t, "clipPath") || tag_is(&t, "mask") || tag_is(&t, "linearGradient") ||
        tag_is(&t, "radialGradient") || tag_is(&t, "symbol") || tag_is(&t, "pattern") || tag_is(&t, "filter") ||
        tag_is(&t, "style") || tag_is(&t, "title") || tag_is(&t, "desc") || tag_is(&t, "metadata") || tag_is(&t, "text") ||
        tag_is(&t, "image") || tag_is(&t, "script") || tag_is(&t, "foreignObject") || tag_is(&t, "switch") ||
        (prop(d, &t, "filter", v, sizeof(v)) && strncmp(v, "url(", 4) == 0)) {
      if (d->clipping && sp == 0 && at == t.name - 1 && (tag_is(&t, "clipPath") || tag_is(&t, "mask"))) {
        /* the clip itself (clip_of): its children are drawn */
      }
      else {
        skip_elem(d, &p, &t);
        if (one && sp == 0) return;
        continue;
      }
    }
    style_of(d, &t, s, &ns);
    if (!ns.hidden && !d->clipping) {
      float *c = clip_of(d, &t, &ns);
      if (c) ns.clip = c;
    }
    if (tag_is(&t, "use") && !ns.hidden) {
      char id[64];
      const char *ref;
      if ((attr(&t, "href", v, sizeof(v)) || attr(&t, "xlink:href", v, sizeof(v))) && url_id(v, id, sizeof(id)) &&
          (ref = id_at(d, id)) != NULL && d->depth < 6) {
        Style us = ns;
        Mat tr = IDENT;
        tr.e = attr(&t, "x", v, sizeof(v)) ? length(v, 0, 0) : 0;
        tr.f = attr(&t, "y", v, sizeof(v)) ? length(v, 0, 0) : 0;
        us.m = mul(ns.m, tr);
        d->depth++;
        draw_from(d, ref, &us, 1);
        d->depth--;
      }
    }
    else if (!ns.hidden) {
      if (tag_is(&t, "path") || tag_is(&t, "rect") || tag_is(&t, "circle") || tag_is(&t, "ellipse") || tag_is(&t, "line") ||
          tag_is(&t, "polygon") || tag_is(&t, "polyline"))
        draw_shape(d, &t, &ns);
    }
    if (t.empty || ns.hidden) {
      if (ns.clip && ns.clip != s->clip) free(ns.clip);
      if (!t.empty) skip_elem(d, &p, &t);
      if (one && sp == 0) return;
      continue;
    }
    if (sp + 1 >= (int)(sizeof(st) / sizeof(st[0]))) {	/* too deep: left out */
      if (ns.clip && ns.clip != s->clip) free(ns.clip);
      skip_elem(d, &p, &t);
      continue;
    }
    st[++sp] = ns;	/* a group (or a shape with children): what is inside takes its style */
  }
  for (; sp > 0; sp--)	/* stopped inside groups: their clips */
    if (st[sp].clip && st[sp].clip != st[sp - 1].clip) free(st[sp].clip);
}


/* a <style>'s rules from s up to </style>: ".cls-1{fill:#fff}.cls-2,.cls-3{...}" (CDATA's marks taken out) */
static void style_rules (Doc *d, const char *s) {
  const char *e = s;
  char *css, *q;
  while (e < d->end && !(e[0] == '<' && e + 1 < d->end && e[1] == '/')) {
    if (d->end - e >= 9 && memcmp(e, "<![CDATA[", 9) == 0) e += 9;
    else e++;
  }
  css = (char *)xmalloc((size_t)(e - s) + 1);
  memcpy(css, s, (size_t)(e - s));
  css[e - s] = '\0';
  while ((q = strstr(css, "<![CDATA[")) != NULL) memset(q, ' ', 9);
  while ((q = strstr(css, "]]>")) != NULL) memset(q, ' ', 3);
  q = css;
  while (*q) {
    char *sel = q, *ob, *cb, *x;
    while (*q && *q != '{') q++;
    if (*q == '\0') break;
    ob = q;
    while (*q && *q != '}') q++;
    cb = q;
    if (*q) q++;
    *ob = '\0';
    *cb = '\0';
    for (x = sel; *x;) {	/* each selector of "a, b" */
      char *xs;
      while (*x == ' ' || *x == ',' || *x == '\n' || *x == '\r' || *x == '\t') x++;
      xs = x;
      while (*x && *x != ',' && *x != ' ' && *x != '\n' && *x != '\r' && *x != '\t') x++;
      if (x > xs && (size_t)(x - xs) < sizeof(d->rule[0].sel)) {
        Rule *r;
        d->rule = (Rule *)xrealloc(d->rule, (size_t)(d->nrule + 1) * sizeof(Rule));
        r = &d->rule[d->nrule++];
        memcpy(r->sel, xs, (size_t)(x - xs));
        r->sel[x - xs] = '\0';
        r->body = xstrdup(ob + 1);
      }
    }
  }
  free(css);
}


/* the gradients, the ids, the <style> rules: before drawing, since they may come after they are used */
static void read_defs (Doc *d) {
  const char *p = d->src;
  Tag t;
  Grad *cur = NULL;
  while (next_tag(&p, d->end, &t)) {
    char v[256];
    if (t.close) {
      if (tag_is(&t, "linearGradient") || tag_is(&t, "radialGradient")) cur = NULL;
      continue;
    }
    if (attr(&t, "id", v, sizeof(v)) && strlen(v) < 64) {
      d->ids = xrealloc(d->ids, (size_t)(d->nids + 1) * sizeof(*d->ids));
      snprintf(d->ids[d->nids].id, sizeof(d->ids[d->nids].id), "%s", v);
      d->ids[d->nids++].at = t.name - 1;
    }
    if (tag_is(&t, "linearGradient") || tag_is(&t, "radialGradient")) {
      static const char *const lin[] = {"x1", "y1", "x2", "y2"}, *const rad[] = {"cx", "cy", "r", "fx", "fy"};
      int k, radial = tag_is(&t, "radialGradient");
      Grad *g;
      d->grad = (Grad *)xrealloc(d->grad, (size_t)(d->ngrad + 1) * sizeof(Grad));
      g = &d->grad[d->ngrad++];
      memset(g, 0, sizeof(*g));
      g->radial = radial;
      g->tr = IDENT;
      if (attr(&t, "id", v, sizeof(v))) snprintf(g->id, sizeof(g->id), "%s", v);
      g->user = attr(&t, "gradientUnits", v, sizeof(v)) && strcmp(v, "userSpaceOnUse") == 0;
      for (k = 0; k < (radial ? 5 : 4); k++)
        if (attr(&t, radial ? rad[k] : lin[k], v, sizeof(v))) {
          g->has[k] = 1;
          g->v[k] = length(v, 1, 0);	/* "50%": 0.5 (of the box, or of 1 in user space: rare) */
        }
      if (attr(&t, "gradientTransform", v, sizeof(v))) g->tr = parse_transform(v);
      if ((attr(&t, "href", v, sizeof(v)) || attr(&t, "xlink:href", v, sizeof(v))) && url_id(v, g->href, sizeof(g->href))) {
        const Grad *from = grad_by(d, g->href);	/* a gradient of one before: its geometry when this has none */
        if (from) {
          for (k = 0; k < 5; k++)
            if (!g->has[k] && from->has[k]) {
              g->has[k] = 1;
              g->v[k] = from->v[k];
            }
          if (!attr(&t, "gradientUnits", v, sizeof(v))) g->user = from->user;
          if (!attr(&t, "gradientTransform", v, sizeof(v))) g->tr = from->tr;
        }
      }
      cur = t.empty ? NULL : g;
      continue;
    }
    if (tag_is(&t, "stop") && cur) {
      char c[96] = "black", o[32] = "1", so[32] = "1", sty[256];
      uint32_t argb = 0xFF000000u;
      Stop *s;
      attr(&t, "offset", o, sizeof(o));
      attr(&t, "stop-color", c, sizeof(c));
      attr(&t, "stop-opacity", so, sizeof(so));
      if (attr(&t, "style", sty, sizeof(sty))) {
        decl(sty, "stop-color", c, sizeof(c));
        decl(sty, "stop-opacity", so, sizeof(so));
      }
      color(c, &argb);
      argb = (argb & 0xFFFFFFu) | (uint32_t)(int)(((argb >> 24) & 255) * opacity_of(so) + 0.5) << 24;
      cur->stop = (Stop *)xrealloc(cur->stop, (size_t)(cur->nstop + 1) * sizeof(Stop));
      s = &cur->stop[cur->nstop++];
      s->off = opacity_of(o);	/* 0 .. 1, or a percent */
      if (cur->nstop > 1 && s->off < cur->stop[cur->nstop - 2].off) s->off = cur->stop[cur->nstop - 2].off;
      s->argb = argb;
      continue;
    }
    if (tag_is(&t, "style") && !t.empty) style_rules(d, t.end);
  }
}


/*
** An SVG as w by h pixels, 0xAARRGGBB (not premultiplied), fitted in the
** middle keeping its shape (preserveAspectRatio's default); NULL when it
** is not SVG. The caller frees it.
*/
uint32_t *svg_render (const char *src, size_t n, int w, int h) {
  Doc d;
  const char *p = src, *svg_at;
  Tag t;
  Style s;
  double vb[4] = {0, 0, 0, 0};
  char v[128];
  uint32_t *out;
  size_t i, np;
  int k;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
  memset(&d, 0, sizeof(d));
  d.src = src;
  d.end = src + n;
  d.left = 20000;
  for (;;) {
    if (!next_tag(&p, d.end, &t)) return NULL;
    if (!t.close && tag_is(&t, "svg")) break;
  }
  svg_at = t.name - 1;
  {
    double sw = attr(&t, "width", v, sizeof(v)) ? length(v, 100, 0) : 0, sh = attr(&t, "height", v, sizeof(v)) ? length(v, 100, 0) : 0;
    if (attr(&t, "viewBox", v, sizeof(v))) {
      const char *q = v;
      for (k = 0; k < 4 && num(&q, &vb[k]); k++) {}
      if (k < 4) vb[2] = vb[3] = 0;
    }
    if (vb[2] <= 0 || vb[3] <= 0) {
      vb[0] = vb[1] = 0;
      vb[2] = sw > 0 ? sw : 16;
      vb[3] = sh > 0 ? sh : 16;
    }
  }
  d.w = w;
  d.h = h;
  np = (size_t)w * (size_t)h;
  d.px = (float *)zalloc(np * 4, sizeof(float));
  d.cov = (float *)zalloc(np, sizeof(float));
  read_defs(&d);
  memset(&s, 0, sizeof(s));
  snprintf(s.fill, sizeof(s.fill), "black");
  s.fill_op = s.stroke_op = s.op = 1;
  s.stroke_w = 1;
  {	/* the viewBox into the picture, in its middle */
    double sc = (double)w / vb[2] < (double)h / vb[3] ? (double)w / vb[2] : (double)h / vb[3];
    s.m.a = s.m.d = sc;
    s.m.b = s.m.c = 0;
    s.m.e = (w - vb[2] * sc) / 2 - vb[0] * sc;
    s.m.f = (h - vb[3] * sc) / 2 - vb[1] * sc;
  }
  draw_from(&d, svg_at, &s, 1);
  out = (uint32_t *)xmalloc(np * sizeof(uint32_t));
  for (i = 0; i < np; i++) {
    float a = d.px[i * 4 + 3];
    uint32_t A = (uint32_t)(a * 255 + 0.5f), c[3];
    for (k = 0; k < 3; k++) {
      float v2 = a > 0 ? d.px[i * 4 + k] / a : 0;
      c[k] = (uint32_t)((v2 > 1 ? 1 : v2) * 255 + 0.5f);
    }
    out[i] = A << 24 | c[0] << 16 | c[1] << 8 | c[2];
  }
  for (k = 0; k < d.ngrad; k++) free(d.grad[k].stop);
  free(d.grad);
  for (k = 0; k < d.nrule; k++) free(d.rule[k].body);
  free(d.rule);
  free(d.ids);
  free(d.px);
  free(d.cov);
  return out;
}

/* }================================================================== */
