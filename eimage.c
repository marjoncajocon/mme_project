/*
** eimage.c - the picture preview, like VS Code's image editor
**
** A picture opens in a tab of its own: its size, kind and bytes on a line
** under it, and the picture itself when the terminal draws them (sixel,
** which mmc-term does). PNG, BMP, GIF and ICO are read here - inflate for
** PNG, LZW for GIF - and turned into pixels; a terminal without pictures,
** or a kind mme cannot read (JPEG, WEBP), gets a page of Braille dots
** instead, which is enough to see what it is.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** inflate (RFC 1951): PNG's pixels come zlib-deflated
** ===================================================================
*/

typedef struct Bits {
  const unsigned char *s;
  size_t n, at;	/* the byte */
  unsigned acc;	/* bits not used yet */
  int nbit;
  int bad;
} Bits;


static int bits_get (Bits *b, int n) {
  while (b->nbit < n) {
    if (b->at >= b->n) {
      b->bad = 1;
      return 0;
    }
    b->acc |= (unsigned)b->s[b->at++] << b->nbit;
    b->nbit += 8;
  }
  {
    int v = (int)(b->acc & ((1u << n) - 1));
    b->acc >>= n;
    b->nbit -= n;
    return v;
  }
}


typedef struct Huff {
  int count[16];	/* codes of each length */
  int sym[288];
} Huff;


static void huff_build (Huff *h, const unsigned char *len, int n) {
  int i, offs[16], total = 0;
  memset(h->count, 0, sizeof(h->count));
  for (i = 0; i < n; i++) h->count[len[i]]++;
  h->count[0] = 0;
  for (i = 1; i < 16; i++) {
    offs[i] = total;
    total += h->count[i];
  }
  for (i = 0; i < n; i++)
    if (len[i]) h->sym[offs[len[i]]++] = i;
}


static int huff_sym (Bits *b, const Huff *h) {
  int code = 0, first = 0, index = 0, len;
  for (len = 1; len < 16; len++) {
    code |= bits_get(b, 1);
    if (b->bad) return -1;
    if (code - first < h->count[len]) return h->sym[index + (code - first)];
    index += h->count[len];
    first = (first + h->count[len]) << 1;
    code <<= 1;
  }
  b->bad = 1;
  return -1;
}


/* the deflate stream s (n bytes) unpacked; *out_n its size, NULL: broken */
static unsigned char *inflate_raw (const unsigned char *s, size_t n, size_t *out_n) {
  static const unsigned short lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                         35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const unsigned char lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const unsigned short dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                         257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                         8193, 12289, 16385, 24577};
  static const unsigned char dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  Bits b;
  unsigned char *out = NULL;
  size_t cap = 0, len = 0;
  int last = 0;
  memset(&b, 0, sizeof(b));
  b.s = s;
  b.n = n;
  while (!last && !b.bad) {
    int type;
    Huff lit, dist;
    last = bits_get(&b, 1);
    type = bits_get(&b, 2);
    if (type == 0) {	/* stored */
      unsigned l;
      b.acc = 0;
      b.nbit = 0;
      if (b.at + 4 > b.n) break;
      l = (unsigned)b.s[b.at] | ((unsigned)b.s[b.at + 1] << 8);
      b.at += 4;
      if (b.at + l > b.n) break;
      if (len + l + 1 > cap) {
        cap = (len + l + 1) * 2;
        out = (unsigned char *)xrealloc(out, cap);
      }
      memcpy(out + len, b.s + b.at, l);
      len += l;
      b.at += l;
      continue;
    }
    if (type == 1) {	/* the fixed trees */
      unsigned char l[288], d[30];
      int i;
      for (i = 0; i < 288; i++) l[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
      for (i = 0; i < 30; i++) d[i] = 5;
      huff_build(&lit, l, 288);
      huff_build(&dist, d, 30);
    }
    else if (type == 2) {	/* the trees of this block */
      static const unsigned char ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      unsigned char cl[19], lens[320];
      Huff clh;
      int hlit = bits_get(&b, 5) + 257, hdist = bits_get(&b, 5) + 1, hclen = bits_get(&b, 4) + 4, i = 0;
      memset(cl, 0, sizeof(cl));
      for (i = 0; i < hclen; i++) cl[ord[i]] = (unsigned char)bits_get(&b, 3);
      huff_build(&clh, cl, 19);
      memset(lens, 0, sizeof(lens));
      for (i = 0; i < hlit + hdist && !b.bad;) {
        int sym = huff_sym(&b, &clh), rep, v = 0;
        if (sym < 0) break;
        if (sym < 16) {
          lens[i++] = (unsigned char)sym;
          continue;
        }
        if (sym == 16) {
          v = i ? lens[i - 1] : 0;
          rep = 3 + bits_get(&b, 2);
        }
        else if (sym == 17) rep = 3 + bits_get(&b, 3);
        else rep = 11 + bits_get(&b, 7);
        while (rep-- > 0 && i < hlit + hdist) lens[i++] = (unsigned char)v;
      }
      huff_build(&lit, lens, hlit);
      huff_build(&dist, lens + hlit, hdist);
    }
    else break;	/* type 3: broken */
    for (;;) {
      int sym = huff_sym(&b, &lit), l, dsym;
      size_t d;
      if (sym < 0 || b.bad) {
        b.bad = 1;
        break;
      }
      if (sym == 256) break;
      if (len + 1 > cap) {
        cap = cap ? cap * 2 : 65536;
        out = (unsigned char *)xrealloc(out, cap);
      }
      if (sym < 256) {
        out[len++] = (unsigned char)sym;
        continue;
      }
      sym -= 257;
      if (sym >= 29) {
        b.bad = 1;
        break;
      }
      l = lbase[sym] + bits_get(&b, lext[sym]);
      dsym = huff_sym(&b, &dist);
      if (dsym < 0 || dsym >= 30) {
        b.bad = 1;
        break;
      }
      d = (size_t)(dbase[dsym] + bits_get(&b, dext[dsym]));
      if (d > len) {
        b.bad = 1;
        break;
      }
      if (len + (size_t)l + 1 > cap) {
        cap = (len + (size_t)l + 1) * 2;
        out = (unsigned char *)xrealloc(out, cap);
      }
      while (l-- > 0) {
        out[len] = out[len - d];
        len++;
      }
    }
  }
  if (b.bad && len == 0) {
    free(out);
    return NULL;
  }
  *out_n = len;
  return out;
}

/* }================================================================== */


/*
** {==================================================================
** The pictures: what kind, how big, and their pixels
** ===================================================================
*/

typedef struct Pix {	/* 0xRRGGBB a pixel, with alpha put on a checkered ground */
  uint32_t *px;
  int w, h;
} Pix;


static uint32_t be32 (const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}


static uint32_t le32 (const unsigned char *p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}


static int le16 (const unsigned char *p) {
  return p[0] | (p[1] << 8);
}


/* what the file is by its first bytes: "PNG", "JPEG" ...; NULL: not a picture */
static const char *kind_of (const unsigned char *b, size_t n) {
  if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return "PNG";
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "JPEG";
  if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0)) return "GIF";
  if (n >= 2 && b[0] == 'B' && b[1] == 'M') return "BMP";
  if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) return "WEBP";
  if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0) return "ICO";
  if (n >= 5 && (memcmp(b, "<?xml", 5) == 0 || memcmp(b, "<svg", 4) == 0)) return "SVG";
  return NULL;
}


/* the size in the file's header; 0: it does not say */
static int size_of (const unsigned char *b, size_t n, const char *kind, int *w, int *h) {
  *w = *h = 0;
  if (strcmp(kind, "PNG") == 0 && n >= 24) {
    *w = (int)be32(b + 16);
    *h = (int)be32(b + 20);
  }
  else if (strcmp(kind, "GIF") == 0 && n >= 10) {
    *w = le16(b + 6);
    *h = le16(b + 8);
  }
  else if (strcmp(kind, "BMP") == 0 && n >= 26) {
    *w = (int)le32(b + 18);
    *h = (int)le32(b + 22);
    if (*h < 0) *h = -*h;
  }
  else if (strcmp(kind, "ICO") == 0 && n >= 8) {
    *w = b[6] ? b[6] : 256;
    *h = b[7] ? b[7] : 256;
  }
  else if (strcmp(kind, "WEBP") == 0 && n >= 30) {
    if (memcmp(b + 12, "VP8X", 4) == 0) {
      *w = 1 + (b[24] | (b[25] << 8) | (b[26] << 16));
      *h = 1 + (b[27] | (b[28] << 8) | (b[29] << 16));
    }
    else if (memcmp(b + 12, "VP8L", 4) == 0) {
      unsigned v = (unsigned)b[21] | ((unsigned)b[22] << 8) | ((unsigned)b[23] << 16) | ((unsigned)b[24] << 24);
      *w = (int)(v & 0x3FFF) + 1;
      *h = (int)((v >> 14) & 0x3FFF) + 1;
    }
    else if (memcmp(b + 12, "VP8 ", 4) == 0 && n >= 30) {
      *w = le16(b + 26) & 0x3FFF;
      *h = le16(b + 28) & 0x3FFF;
    }
  }
  else if (strcmp(kind, "JPEG") == 0) {	/* the first SOFn frame says it */
    size_t i = 2;
    while (i + 9 < n) {
      unsigned char m;
      size_t seg;
      if (b[i] != 0xFF) {
        i++;
        continue;
      }
      m = b[i + 1];
      if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
        i += 2;
        continue;
      }
      seg = (size_t)((b[i + 2] << 8) | b[i + 3]);
      if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) ||
          (m >= 0xCD && m <= 0xCF)) {
        *h = (b[i + 5] << 8) | b[i + 6];
        *w = (b[i + 7] << 8) | b[i + 8];
        break;
      }
      i += 2 + seg;
    }
  }
  else if (strcmp(kind, "SVG") == 0) {	/* width="120" height="80", or the viewBox */
    const char *s = (const char *)b, *a;
    size_t look = n < 4000 ? n : 4000;
    char tmp[4001];
    memcpy(tmp, s, look);
    tmp[look] = '\0';
    /* the quote after the '=' is skipped, but only when the text really goes that far */
    if ((a = strstr(tmp, "width=")) != NULL && a[6] != '\0') *w = atoi(a + 7);
    if ((a = strstr(tmp, "height=")) != NULL && a[7] != '\0') *h = atoi(a + 8);
    if ((*w == 0 || *h == 0) && (a = strstr(tmp, "viewBox=")) != NULL && a[8] != '\0') {
      double v[4] = {0, 0, 0, 0};
      if (sscanf(a + 9, "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]) == 4) {
        *w = (int)v[2];
        *h = (int)v[3];
      }
    }
  }
  return *w > 0 && *h > 0;
}


/* alpha over VS Code's checkered ground */
static uint32_t blend (uint32_t r, uint32_t g, uint32_t bl, uint32_t a, int x, int y) {
  uint32_t back = ((x / 8 + y / 8) & 1) ? 0x808080u : 0x606060u;
  uint32_t br = (back >> 16) & 255, bg = (back >> 8) & 255, bb = back & 255;
  r = (r * a + br * (255 - a)) / 255;
  g = (g * a + bg * (255 - a)) / 255;
  bl = (bl * a + bb * (255 - a)) / 255;
  return (r << 16) | (g << 8) | bl;
}


/* --- PNG --- */

static int png_bits (const unsigned char *row, int i, int depth) {
  int per = 8 / depth, shift = (per - 1 - (i % per)) * depth;
  return (row[i / per] >> shift) & ((1 << depth) - 1);
}


static int png_decode (const unsigned char *b, size_t n, Pix *out) {
  unsigned char *idat = NULL, *raw = NULL, *pal = NULL, *trns = NULL;
  size_t nidat = 0, nraw = 0, i = 8, npal = 0, ntrns = 0;
  int w = 0, h = 0, depth = 0, ctype = 0, inter = 0, ok = 0, chan, x, y;
  size_t stride, rb;
  uint32_t *px = NULL;
  while (i + 8 <= n) {	/* the chunks */
    uint32_t len = be32(b + i);
    const char *type = (const char *)b + i + 4;
    const unsigned char *d = b + i + 8;
    if (len > n || i + 12 + len > n) break;
    if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
      w = (int)be32(d);
      h = (int)be32(d + 4);
      depth = d[8];
      ctype = d[9];
      inter = d[12];
    }
    else if (memcmp(type, "PLTE", 4) == 0) {
      pal = (unsigned char *)xmalloc(len + 1);
      memcpy(pal, d, len);
      npal = len / 3;
    }
    else if (memcmp(type, "tRNS", 4) == 0) {
      trns = (unsigned char *)xmalloc(len + 1);
      memcpy(trns, d, len);
      ntrns = len;
    }
    else if (memcmp(type, "IDAT", 4) == 0) {
      idat = (unsigned char *)xrealloc(idat, nidat + len);
      memcpy(idat + nidat, d, len);
      nidat += len;
    }
    else if (memcmp(type, "IEND", 4) == 0) break;
    i += 12 + len;
  }
  if (w <= 0 || h <= 0 || inter != 0 || nidat < 3 || (size_t)w * (size_t)h > (1u << 26)) goto done;
  if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) goto done;
  raw = inflate_raw(idat + 2, nidat - 2, &nraw);	/* past zlib's two bytes */
  if (raw == NULL) goto done;
  chan = ctype == 2 ? 3 : ctype == 4 ? 2 : ctype == 6 ? 4 : 1;
  rb = ((size_t)w * (size_t)chan * (size_t)depth + 7) / 8;
  stride = rb + 1;
  if (nraw < stride * (size_t)h) goto done;
  px = (uint32_t *)xmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
  for (y = 0; y < h; y++) {	/* the filter of each row, then its pixels */
    unsigned char *r = raw + (size_t)y * stride + 1, *prev = y ? raw + (size_t)(y - 1) * stride + 1 : NULL;
    int f = raw[(size_t)y * stride], bpp = (chan * depth + 7) / 8;
    size_t k;
    for (k = 0; k < rb; k++) {
      int a = k >= (size_t)bpp ? r[k - bpp] : 0, bb = prev ? prev[k] : 0;
      int c = (prev && k >= (size_t)bpp) ? prev[k - bpp] : 0;
      switch (f) {
        case 1: r[k] = (unsigned char)(r[k] + a); break;
        case 2: r[k] = (unsigned char)(r[k] + bb); break;
        case 3: r[k] = (unsigned char)(r[k] + (a + bb) / 2); break;
        case 4: {
          int p = a + bb - c, pa = p > a ? p - a : a - p, pb = p > bb ? p - bb : bb - p;
          int pc = p > c ? p - c : c - p;
          r[k] = (unsigned char)(r[k] + (pa <= pb && pa <= pc ? a : pb <= pc ? bb : c));
          break;
        }
        default: break;
      }
    }
    for (x = 0; x < w; x++) {
      uint32_t cr, cg, cb, ca = 255;
      if (depth == 16) {
        const unsigned char *p = r + (size_t)x * (size_t)chan * 2;
        cr = p[0];
        cg = chan >= 3 ? p[2] : p[0];
        cb = chan >= 3 ? p[4] : p[0];
        if (ctype == 4) ca = p[2];
        else if (ctype == 6) ca = p[6];
      }
      else if (depth == 8) {
        const unsigned char *p = r + (size_t)x * (size_t)chan;
        if (ctype == 3) {
          int idx = p[0];
          cr = (uint32_t)(idx < (int)npal ? pal[idx * 3] : 0);
          cg = (uint32_t)(idx < (int)npal ? pal[idx * 3 + 1] : 0);
          cb = (uint32_t)(idx < (int)npal ? pal[idx * 3 + 2] : 0);
          if (trns && idx < (int)ntrns) ca = trns[idx];
        }
        else {
          cr = p[0];
          cg = chan >= 3 ? p[1] : p[0];
          cb = chan >= 3 ? p[2] : p[0];
          if (ctype == 4) ca = p[1];
          else if (ctype == 6) ca = p[3];
        }
      }
      else {	/* 1, 2 or 4 bits: gray or a palette */
        int v = png_bits(r, x, depth), max = (1 << depth) - 1;
        if (ctype == 3) {
          cr = (uint32_t)(v < (int)npal ? pal[v * 3] : 0);
          cg = (uint32_t)(v < (int)npal ? pal[v * 3 + 1] : 0);
          cb = (uint32_t)(v < (int)npal ? pal[v * 3 + 2] : 0);
          if (trns && v < (int)ntrns) ca = trns[v];
        }
        else cr = cg = cb = (uint32_t)(v * 255 / max);
      }
      px[(size_t)y * (size_t)w + (size_t)x] = ca == 255 ? ((cr << 16) | (cg << 8) | cb)
                                                        : blend(cr, cg, cb, ca, x, y);
    }
  }
  out->px = px;
  out->w = w;
  out->h = h;
  px = NULL;
  ok = 1;
done:
  free(idat);
  free(raw);
  free(pal);
  free(trns);
  free(px);
  return ok;
}


/* --- BMP --- */

static int bmp_decode (const unsigned char *b, size_t n, Pix *out) {
  uint32_t off, hdr;
  int w, h, bpp, flip = 1, x, y;
  size_t row, npal;
  uint32_t *px;
  const unsigned char *pal;
  if (n < 34) return 0;	/* the file header, the info header's size, and the compression at 30 */
  off = le32(b + 10);
  hdr = le32(b + 14);
  w = (int)le32(b + 18);
  h = (int)le32(b + 22);
  bpp = le16(b + 28);
  if (h < 0) {
    h = -h;
    flip = 0;
  }
  if (w <= 0 || h <= 0 || off >= n || (size_t)w * (size_t)h > (1u << 26)) return 0;
  if (bpp != 24 && bpp != 32 && bpp != 8) return 0;
  if (le32(b + 30) != 0 && bpp != 32) return 0;	/* compressed: not read here */
  if ((size_t)hdr > n - 14) return 0;	/* the header's own size comes from the file */
  pal = b + 14 + hdr;
  npal = (size_t)(n - 14 - hdr) / 4;	/* the color table entries the file really holds */
  row = ((size_t)w * (size_t)bpp / 8 + 3) & ~(size_t)3;
  if ((size_t)off + row * (size_t)h > n) return 0;
  px = (uint32_t *)xmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
  for (y = 0; y < h; y++) {
    const unsigned char *r = b + off + row * (size_t)(flip ? h - 1 - y : y);
    for (x = 0; x < w; x++) {
      uint32_t c;
      if (bpp == 8) {
        size_t idx = r[x];
        c = idx < npal ? ((uint32_t)pal[idx * 4 + 2] << 16) | ((uint32_t)pal[idx * 4 + 1] << 8) | pal[idx * 4] : 0;
      }
      else {
        const unsigned char *p = r + (size_t)x * (size_t)(bpp / 8);
        c = bpp == 32 && p[3] != 255 ? blend(p[2], p[1], p[0], p[3], x, y)
                                     : (((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]);
      }
      px[(size_t)y * (size_t)w + (size_t)x] = c;
    }
  }
  out->px = px;
  out->w = w;
  out->h = h;
  return 1;
}


/* --- GIF: the first frame --- */

static int gif_decode (const unsigned char *b, size_t n, Pix *out) {
  unsigned char pal[256 * 3], lpal[256 * 3];
  const unsigned char *p = b + 13, *e = b + n;
  int gsize = 0, w, h, x, y, tr = -1;
  uint32_t *px;
  if (n < 14) return 0;
  memset(pal, 0, sizeof(pal));	/* a picture may use an index the color table does not have */
  if (b[10] & 0x80) {
    gsize = 2 << (b[10] & 7);
    if (13 + gsize * 3 > (int)n) return 0;
    memcpy(pal, b + 13, (size_t)gsize * 3);
    p = b + 13 + gsize * 3;
  }
  while (p < e) {	/* the blocks, up to the first picture */
    if (*p == 0x21) {	/* an extension */
      if (p + 6 < e && p[1] == 0xF9 && p[2] >= 4) tr = (p[3] & 1) ? p[6] : -1;	/* p[6] is read: it must be there */
      p += 2;
      while (p < e && *p) p += 1 + *p;	/* its sub blocks */
      p++;
      continue;
    }
    if (*p == 0x2C) break;	/* the picture */
    if (*p == 0x3B) return 0;
    p++;
  }
  if (p + 10 >= e) return 0;
  w = le16(p + 5);
  h = le16(p + 7);
  if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > (1u << 26)) return 0;
  if (p[9] & 0x80) {	/* this picture's own colors */
    int ls = 2 << (p[9] & 7);
    if (p + 10 + ls * 3 >= e) return 0;
    memcpy(lpal, p + 10, (size_t)ls * 3);
    memcpy(pal, lpal, (size_t)ls * 3);
    gsize = ls;
    p += ls * 3;
  }
  if (gsize == 0) return 0;
  p += 10;
  /* the code size comes from the file; outside 2..8 the tables below would not hold the codes */
  if (*p < 2 || *p > 8) return 0;
  {	/* LZW: the codes are the pixels' colors */
    int min = *p++, clear = 1 << min, end = clear + 1, size = min + 1, next = end + 1, prev = -1;
    unsigned char *data = NULL, *pixels;
    size_t nd = 0, at = 0, outn = 0;
    unsigned acc = 0;
    int nbit = 0;
    static int pre[4096], suf[4096];
    while (p < e && *p) {	/* the sub blocks are one stream */
      size_t len = *p++;
      if (p + len > e) break;
      data = (unsigned char *)xrealloc(data, nd + len);
      memcpy(data + nd, p, len);
      nd += len;
      p += len;
    }
    pixels = (unsigned char *)xmalloc((size_t)w * (size_t)h + 4096);
    while (at < nd || nbit >= size) {
      int code;
      unsigned char stack[4096];
      int sp = 0;
      while (nbit < size && at < nd) {
        acc |= (unsigned)data[at++] << nbit;
        nbit += 8;
      }
      if (nbit < size) break;
      code = (int)(acc & (unsigned)((1 << size) - 1));
      acc >>= size;
      nbit -= size;
      if (code == clear) {
        size = min + 1;
        next = end + 1;
        prev = -1;
        continue;
      }
      if (code == end || code > next) break;	/* above next the tables do not hold it: broken */
      if (code < next && code != next) {
        int c = code;
        while (c >= clear && sp < 4095) {	/* 4095: one is put after the loop */
          stack[sp++] = (unsigned char)suf[c];
          c = pre[c];
        }
        if (sp < 4096) stack[sp++] = (unsigned char)c;
      }
      else if (prev >= 0) {	/* the code not there yet: the one before plus its first */
        int c = prev;
        unsigned char tmp[4096];
        int t = 0;
        while (c >= clear && t < 4095) {	/* 4095: one is put after the loop */
          tmp[t++] = (unsigned char)suf[c];
          c = pre[c];
        }
        if (t < 4096) tmp[t++] = (unsigned char)c;
        stack[sp++] = (unsigned char)c;
        while (t-- > 0 && sp < 4096) stack[sp++] = tmp[t];
        {	/* it comes out in order: turn it back */
          int a2 = 0, b2 = sp - 1;
          while (a2 < b2) {
            unsigned char t2 = stack[a2];
            stack[a2++] = stack[b2];
            stack[b2--] = t2;
          }
        }
      }
      else break;
      while (sp > 0 && outn < (size_t)w * (size_t)h) pixels[outn++] = stack[--sp];
      if (prev >= 0 && next < 4096) {
        pre[next] = prev;
        suf[next] = pixels[outn - 1];	/* the first letter of this code */
        {	/* the first of the code just put out */
          int c = code < next ? code : prev;
          while (c >= clear) c = pre[c];
          suf[next] = (unsigned char)c;
        }
        next++;
        if (next >= (1 << size) && size < 12) size++;
      }
      prev = code;
    }
    free(data);
    px = (uint32_t *)xmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
        size_t k = (size_t)y * (size_t)w + (size_t)x;
        int idx = k < outn ? pixels[k] : 0;
        if (idx == tr) px[k] = blend(0, 0, 0, 0, x, y);
        else px[k] = ((uint32_t)pal[idx * 3] << 16) | ((uint32_t)pal[idx * 3 + 1] << 8) | pal[idx * 3 + 2];
      }
    free(pixels);
  }
  out->px = px;
  out->w = w;
  out->h = h;
  return 1;
}


/* an .ico holds PNGs or BMP-like pictures: the biggest one */
static int ico_decode (const unsigned char *b, size_t n, Pix *out) {
  int count, i, best = -1, bw = 0;
  if (n < 6) return 0;
  count = le16(b + 4);
  for (i = 0; i < count && (size_t)(6 + 16 * (i + 1)) <= n; i++) {
    int w = b[6 + 16 * i] ? b[6 + 16 * i] : 256;
    if (w > bw) {
      bw = w;
      best = i;
    }
  }
  if (best < 0) return 0;
  {
    const unsigned char *e = b + 6 + 16 * best;
    size_t len = le32(e + 8), off = le32(e + 12);	/* size_t: the sum of two uint32_t must not wrap */
    if (len == 0 || off > n || len > n - off) return 0;
    if (len > 8 && memcmp(b + off, "\x89PNG", 4) == 0) return png_decode(b + off, len, out);
    {	/* a BMP without its file header: one is made for it */
      unsigned char *tmp = (unsigned char *)xmalloc(len + 14);
      int ok;
      memcpy(tmp + 14, b + off, len);
      memset(tmp, 0, 14);
      tmp[0] = 'B';
      tmp[1] = 'M';
      tmp[10] = 54;
      ok = bmp_decode(tmp, len + 14, out);
      free(tmp);
      if (ok) out->h /= 2;	/* an icon's height counts its mask too */
      return ok;
    }
  }
}


/* the file's pixels; 0: mme cannot read this kind */
static int decode (const unsigned char *b, size_t n, const char *kind, Pix *out) {
  memset(out, 0, sizeof(*out));
  if (strcmp(kind, "PNG") == 0) return png_decode(b, n, out);
  if (strcmp(kind, "BMP") == 0) return bmp_decode(b, n, out);
  if (strcmp(kind, "GIF") == 0) return gif_decode(b, n, out);
  if (strcmp(kind, "ICO") == 0) return ico_decode(b, n, out);
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Sixel: the picture for the terminal
** ===================================================================
*/

/* the picture in w by h pixels (nearest, which keeps it sharp) */
static uint32_t *scale (const Pix *p, int w, int h) {
  uint32_t *out = (uint32_t *)xmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
  int x, y;
  for (y = 0; y < h; y++) {
    int sy = (int)((long long)y * p->h / h);
    for (x = 0; x < w; x++) {
      int sx = (int)((long long)x * p->w / w);
      out[(size_t)y * (size_t)w + (size_t)x] = p->px[(size_t)sy * (size_t)p->w + (size_t)sx];
    }
  }
  return out;
}


/* a color to one of 6*6*6 plus 24 grays, as the terminals' palettes are */
static int quant (uint32_t c) {
  int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
  int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  if (mx - mn < 12) {	/* gray */
    int k = (mx * 23 + 127) / 255;
    return 216 + k;
  }
  return (r * 5 + 127) / 255 * 36 + (g * 5 + 127) / 255 * 6 + (b * 5 + 127) / 255;
}


static void pal_rgb (int i, int *r, int *g, int *b) {
  if (i >= 216) {
    int v = (i - 216) * 255 / 23;
    *r = *g = *b = v * 100 / 255;
    return;
  }
  *r = (i / 36) * 100 / 5;
  *g = ((i / 6) % 6) * 100 / 5;
  *b = (i % 6) * 100 / 5;
}


/*
** The pixels as a sixel string: six rows at a time, a run of the same
** bits written once ("!n<char>"), one pass per color in the band.
*/
static char *sixel_of (const uint32_t *px, int w, int h, size_t *out_n) {
  Buf o;
  unsigned char *idx = (unsigned char *)xmalloc((size_t)w * (size_t)h);
  int used[240], band, i;
  size_t k;
  buf_init(&o);
  memset(used, 0, sizeof(used));
  for (k = 0; k < (size_t)w * (size_t)h; k++) {
    idx[k] = (unsigned char)quant(px[k]);
    used[idx[k]] = 1;
  }
  buf_puts(&o, "\033P0;1;0q");
  buf_printf(&o, "\"1;1;%d;%d", w, h);
  for (i = 0; i < 240; i++)	/* the colors this picture uses */
    if (used[i]) {
      int r, g, b;
      pal_rgb(i, &r, &g, &b);
      buf_printf(&o, "#%d;2;%d;%d;%d", i, r, g, b);
    }
  for (band = 0; band < h; band += 6) {
    int first = 1;
    for (i = 0; i < 240; i++) {
      int x, any = 0, run = 0, last = -1;
      if (!used[i]) continue;
      for (x = 0; x < w && !any; x++) {	/* is this color in the band at all? */
        int row;
        for (row = 0; row < 6 && band + row < h; row++)
          if (idx[(size_t)(band + row) * (size_t)w + (size_t)x] == i) any = 1;
      }
      if (!any) continue;
      if (!first) buf_putc(&o, '$');	/* back to the left, another color over it */
      first = 0;
      buf_printf(&o, "#%d", i);
      for (x = 0; x < w; x++) {
        int bits = 0, row, ch;
        for (row = 0; row < 6 && band + row < h; row++)
          if (idx[(size_t)(band + row) * (size_t)w + (size_t)x] == i) bits |= 1 << row;
        ch = '?' + bits;
        if (ch == last) run++;
        else {
          if (run > 3) buf_printf(&o, "!%d%c", run, last);
          else while (run-- > 0) buf_putc(&o, (char)last);
          last = ch;
          run = 1;
        }
      }
      if (run > 3) buf_printf(&o, "!%d%c", run, last);
      else while (run-- > 0) buf_putc(&o, (char)last);
    }
    buf_putc(&o, '-');	/* the next band */
  }
  buf_puts(&o, "\033\\");
  free(idx);
  *out_n = o.len;
  return buf_take(&o);
}

/* }================================================================== */


/*
** {==================================================================
** The page
** ===================================================================
*/

typedef struct Img {
  char *path;
  char *kind;	/* "PNG" ... */
  size_t bytes;
  int w, h;	/* the picture's own size */
  Pix pix;	/* its pixels, 0 when mme cannot read the kind */
  int zoom;	/* -3 .. 6: how much bigger or smaller (0: fit) */
  char *sixel;	/* what was sent last */
  size_t nsixel;
  int sx_w, sx_h;	/* the cells it covers */
  int fit_w, fit_h;	/* the size it was made for */
  int x, y, w_box, h_box;
} Img;


void *img_open (const char *path) {
  Img *im;
  size_t len = 0;
  char *s = read_file(path, &len);
  const char *kind;
  if (s == NULL) return NULL;
  kind = kind_of((const unsigned char *)s, len);
  if (kind == NULL) {
    free(s);
    return NULL;
  }
  im = (Img *)xmalloc(sizeof(Img));
  memset(im, 0, sizeof(*im));
  im->path = xstrdup(path);
  im->kind = xstrdup(kind);
  im->bytes = len;
  size_of((const unsigned char *)s, len, kind, &im->w, &im->h);
  if (decode((const unsigned char *)s, len, kind, &im->pix)) {
    if (im->w == 0) im->w = im->pix.w;
    if (im->h == 0) im->h = im->pix.h;
  }
  free(s);
  term_ask_pixels();	/* can the terminal draw pictures? */
  return im;
}


void img_close (void *page) {
  Img *im = (Img *)page;
  if (im == NULL) return;
  free(im->path);
  free(im->kind);
  free(im->pix.px);
  free(im->sixel);
  free(im);
}


void img_reload (void *page) {
  Img *im = (Img *)page;
  void *fresh;
  if (im == NULL || (fresh = img_open(im->path)) == NULL) return;
  {
    Img *f = (Img *)fresh;
    int zoom = im->zoom;
    free(im->path);
    free(im->kind);
    free(im->pix.px);
    free(im->sixel);
    *im = *f;
    im->zoom = zoom;
    free(f);
  }
}


/* "1920x1080 • PNG • 240 KB", the line VS Code shows under a picture */
const char *img_status (void *page) {
  static char s[160];
  Img *im = (Img *)page;
  char size[32];
  if (im == NULL) return "";
  if (im->bytes >= 1024 * 1024) snprintf(size, sizeof(size), "%.1f MB", (double)im->bytes / (1024 * 1024));
  else if (im->bytes >= 1024) snprintf(size, sizeof(size), "%.1f KB", (double)im->bytes / 1024);
  else snprintf(size, sizeof(size), "%lu B", (unsigned long)im->bytes);
  if (im->w > 0) snprintf(s, sizeof(s), "%dx%d \xE2\x80\xA2 %s \xE2\x80\xA2 %s", im->w, im->h, im->kind, size);
  else snprintf(s, sizeof(s), "%s \xE2\x80\xA2 %s", im->kind, size);
  return s;
}


void img_zoom (void *page, int delta) {
  Img *im = (Img *)page;
  if (im == NULL) return;
  im->zoom = delta == 0 ? 0 : im->zoom + delta;
  if (im->zoom > 6) im->zoom = 6;
  if (im->zoom < -3) im->zoom = -3;
  free(im->sixel);	/* it is made again at its new size */
  im->sixel = NULL;
}


int img_key (void *page, int k) {
  if (page == NULL) return 0;
  switch (KEY_CODE(k)) {
    case '+': case '=': img_zoom(page, 1); return 1;
    case '-': case '_': img_zoom(page, -1); return 1;
    case '0': img_zoom(page, 0); return 1;
    default: return 0;
  }
}


/* without sixel: a cell is 2 by 4 dots, its color the middle of what it covers */
static void braille (const Img *im, int x, int y, int w, int h) {
  int cx, cy;
  const Pix *p = &im->pix;
  int cw, ch;
  if (p->px == NULL || p->w <= 0 || p->h <= 0) return;
  if (2 * (long long)h * p->w <= (long long)w * p->h) {	/* the height is what limits it */
    ch = h;
    cw = (int)((long long)h * 2 * p->w / p->h);
  }
  else {
    cw = w;
    ch = (int)((long long)w * p->h / (2 * p->w));
  }
  if (cw < 1) cw = 1;
  if (ch < 1) ch = 1;
  if (cw > w) cw = w;
  if (ch > h) ch = h;
  x += (w - cw) / 2;	/* in the middle, as VS Code puts a picture */
  for (cy = 0; cy < ch; cy++)
    for (cx = 0; cx < cw; cx++) {
      uint32_t dots = 0x2800, sum_r = 0, sum_g = 0, sum_b = 0;
      int i, j, lum_avg = 0, n = 0;
      static const int bit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
      for (j = 0; j < 4; j++)
        for (i = 0; i < 2; i++) {
          int sx = (int)((long long)(cx * 2 + i) * p->w / (cw * 2));
          int sy = (int)((long long)(cy * 4 + j) * p->h / (ch * 4));
          uint32_t c = p->px[(size_t)sy * (size_t)p->w + (size_t)sx];
          int lum = (int)(((c >> 16) & 255) * 30 + ((c >> 8) & 255) * 59 + (c & 255) * 11) / 100;
          sum_r += (c >> 16) & 255;
          sum_g += (c >> 8) & 255;
          sum_b += c & 255;
          lum_avg += lum;
          n++;
        }
      lum_avg /= n ? n : 1;
      for (j = 0; j < 4; j++)
        for (i = 0; i < 2; i++) {
          int sx = (int)((long long)(cx * 2 + i) * p->w / (cw * 2));
          int sy = (int)((long long)(cy * 4 + j) * p->h / (ch * 4));
          uint32_t c = p->px[(size_t)sy * (size_t)p->w + (size_t)sx];
          int lum = (int)(((c >> 16) & 255) * 30 + ((c >> 8) & 255) * 59 + (c & 255) * 11) / 100;
          if (lum >= lum_avg) dots |= (uint32_t)bit[j][i];
        }
      scr_put_rgb(x + cx, y + cy, dots, ((sum_r / 8) << 16) | ((sum_g / 8) << 8) | (sum_b / 8),
                  ui_color(C_EDITOR_BG), 0);
    }
}


void img_draw (void *page, int x, int y, int w, int h, int focus) {
  Img *im = (Img *)page;
  int cw = 0, ch = 0, r, box_h = h - 2;
  char line[300];
  uint32_t bg = ui_color(C_EDITOR_BG), fg = ui_color(C_EDITOR_FG), dim = ui_color(C_DIM);
  (void)focus;
  if (im == NULL) return;
  im->x = x;
  im->y = y;
  im->w_box = w;
  im->h_box = h;
  for (r = 0; r < h; r++) {
    int k;
    for (k = 0; k < w; k++) scr_put_rgb(x + k, y + r, ' ', fg, bg, 0);
  }
  if (h < 3 || w < 20) return;
  if (im->pix.px && term_cell_px(&cw, &ch) && cw > 1 && ch > 1) {	/* the picture itself */
    int max_w = (w - 2) * cw, max_h = box_h * ch, pw = im->pix.w, ph = im->pix.h;
    double f = (double)max_w / pw < (double)max_h / ph ? (double)max_w / pw : (double)max_h / ph;
    int i;	/* as big as the editor area holds, its shape kept */
    for (i = 0; i < im->zoom; i++) f *= 1.25;	/* Ctrl+= and Ctrl+- from there */
    for (i = 0; i > im->zoom; i--) f /= 1.25;
    {
      int dw = (int)(pw * f), dh = (int)(ph * f);
      int cells_w, cells_h;
      if (dw < 1) dw = 1;
      if (dh < 1) dh = 1;
      if (dw > max_w) dw = max_w;
      if (dh > max_h) dh = max_h;
      cells_w = (dw + cw - 1) / cw;
      cells_h = (dh + ch - 1) / ch;
      if (im->sixel == NULL || im->fit_w != dw || im->fit_h != dh) {
        uint32_t *sc = scale(&im->pix, dw, dh);
        free(im->sixel);
        im->sixel = sixel_of(sc, dw, dh, &im->nsixel);
        im->fit_w = dw;
        im->fit_h = dh;
        free(sc);
      }
      im->sx_w = cells_w;
      im->sx_h = cells_h;
      scr_image(x + (w - cells_w) / 2, y + (box_h - cells_h) / 2 > 0 ? y + (box_h - cells_h) / 2 : y,
                cells_w, cells_h, im->sixel, im->nsixel);
    }
  }
  else if (im->pix.px) braille(im, x + 1, y, w - 2, box_h);	/* no pictures in this terminal */
  else {	/* a kind mme does not read: what it is, in the middle */
    const char *why = strcmp(im->kind, "SVG") == 0 ? "SVG is drawn by a browser, not by mme"
                                                   : "mme reads PNG, BMP, GIF and ICO pictures";
    int cy = y + box_h / 2;
    snprintf(line, sizeof(line), "\xF0\x9F\x96\xBC  %s", path_basename(im->path));
    scr_putsw(x + (w - (int)str_cols(line)) / 2, cy - 1, w, line, S_TEXT);
    scr_putsw(x + (w - (int)str_cols(img_status(page))) / 2, cy, w, img_status(page), S_TEXT);
    scr_putsw(x + (w - (int)str_cols(why)) / 2, cy + 2, w, why, S_LINE);
  }
  {	/* the line under it, like VS Code's */
    int k;
    snprintf(line, sizeof(line), " %s   %s   %s", path_basename(im->path), img_status(page),
             im->zoom ? "0 fits it again" : "+ bigger, - smaller");
    for (k = 0; k < w; k++) scr_put_rgb(x + k, y + h - 1, ' ', dim, bg, 0);
    scr_putsw(x, y + h - 1, w, line, S_STATUS);
  }
}


int img_click (void *page, int mx, int my) {
  Img *im = (Img *)page;
  if (im == NULL) return 0;
  return mx >= im->x && mx < im->x + im->w_box && my >= im->y && my < im->y + im->h_box;
}


/*
** "120x80 png, 12 KB" for the Markdown preview's place-holders; 0 when
** the file is not there or is not a picture. Only its head is read.
*/
int img_info (const char *path, char *out, size_t n) {
  unsigned char head[4096];
  const char *kind;
  OsStat st;
  size_t got;
  int w = 0, h = 0, fd = os_open(path, OS_READ);
  if (fd < 0) return 0;
  {
    long r = os_read(fd, (char *)head, sizeof(head));
    os_close(fd);
    if (r <= 0) return 0;
    got = (size_t)r;
  }
  if ((kind = kind_of(head, got)) == NULL) return 0;
  size_of(head, got, kind, &w, &h);
  if (os_stat(path, &st) != 0) st.size = 0;
  {
    char size[32];
    char low[8];
    size_t i;
    snprintf(low, sizeof(low), "%s", kind);
    for (i = 0; low[i]; i++) low[i] = (char)(low[i] >= 'A' && low[i] <= 'Z' ? low[i] + 32 : low[i]);
    if (st.size >= 1024 * 1024) snprintf(size, sizeof(size), ", %.1f MB", (double)st.size / (1024 * 1024));
    else if (st.size >= 1024) snprintf(size, sizeof(size), ", %.0f KB", (double)st.size / 1024);
    else if (st.size > 0) snprintf(size, sizeof(size), ", %lld B", (long long)st.size);
    else size[0] = '\0';
    if (w > 0) snprintf(out, n, "%dx%d %s%s", w, h, low, size);
    else snprintf(out, n, "%s%s", low, size);
  }
  return 1;
}


/* the kinds that open in the picture page */
int img_is_image (const char *path) {
  static const char *const ext[] = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".ico", ".svg"};
  const char *dot = path ? strrchr(path_basename(path), '.') : NULL;
  size_t i;
  if (dot == NULL) return 0;
  for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
    if (m_fncmp(dot, ext[i]) == 0) return 1;
  return 0;
}

/* }================================================================== */

