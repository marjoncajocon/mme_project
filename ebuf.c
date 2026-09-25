/*
** ebuf.c - the text of a file: an array of lines, load and save, undo
**
** Every change goes through doc_insert and doc_delete, which remember it:
** undo plays the list backwards, redo forwards again. A file is split at
** every '\n', so "a\nb\n" is three lines, the last one empty, like VS
** Code shows it; saving joins them back with the same line end.
*/

#include "mme.h"

#include <stdlib.h>
#include <string.h>


int pos_cmp (Pos a, Pos b) {
  if (a.y != b.y) return a.y < b.y ? -1 : 1;
  if (a.x != b.x) return a.x < b.x ? -1 : 1;
  return 0;
}


Pos pos_after (Pos a, const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] == '\n') {
      a.y++;
      a.x = 0;
    }
    else a.x++;
  }
  return a;
}


/*
** {==================================================================
** Lines
** ===================================================================
*/

static void row_room (Row *r, size_t need) {
  if (need <= r->cap) return;
  r->cap = r->cap ? r->cap : 16;
  while (r->cap < need) r->cap *= 2;
  r->s = (char *)xrealloc(r->s, r->cap);
}


static void row_append (Row *r, const char *s, size_t n) {
  row_room(r, r->len + n);
  memcpy(r->s + r->len, s, n);
  r->len += n;
}


/* a new empty line at index at */
static Row *row_new (Doc *d, size_t at) {
  if (d->n == d->cap) {
    d->cap = d->cap ? d->cap * 2 : 64;
    d->row = (Row *)xrealloc(d->row, d->cap * sizeof(Row));
  }
  memmove(d->row + at + 1, d->row + at, (d->n - at) * sizeof(Row));
  d->n++;
  memset(&d->row[at], 0, sizeof(Row));
  return &d->row[at];
}


static void rows_free (Doc *d) {
  size_t i;
  for (i = 0; i < d->n; i++) free(d->row[i].s);
  free(d->row);
  d->row = NULL;
  d->n = d->cap = 0;
}

/* }================================================================== */


/*
** {==================================================================
** Changes without undo
** ===================================================================
*/

static Pos raw_insert (Doc *d, Pos at, const char *s, size_t n) {
  Row *r = &d->row[at.y];
  if (at.y < d->hl_from) d->hl_from = at.y;
  if (at.y < d->br_from) d->br_from = at.y;
  if (at.y < d->tm_from) d->tm_from = at.y;
  char *tail = xstrndup(r->s ? r->s + at.x : "", r->len - at.x);
  size_t tlen = r->len - at.x, i, from = 0;
  r->len = at.x;
  for (i = 0; i <= n; i++) {
    if (i < n && s[i] != '\n') continue;
    row_append(&d->row[at.y], s + from, i - from);
    if (i < n) {	/* a newline: the rest goes on a new line */
      at.x = 0;
      at.y++;
      row_new(d, at.y);
    }
    else at.x = d->row[at.y].len;
    from = i + 1;
  }
  row_append(&d->row[at.y], tail, tlen);
  free(tail);
  return at;
}


static void raw_delete (Doc *d, Pos a, Pos b) {
  Row *ra = &d->row[a.y], *rb = &d->row[b.y];
  size_t i;
  if (a.y < d->hl_from) d->hl_from = a.y;
  if (a.y < d->br_from) d->br_from = a.y;
  if (a.y < d->tm_from) d->tm_from = a.y;
  if (a.y == b.y) {
    memmove(ra->s + a.x, ra->s + b.x, ra->len - b.x);
    ra->len -= b.x - a.x;
    return;
  }
  ra->len = a.x;
  row_append(ra, rb->s + b.x, rb->len - b.x);
  for (i = a.y + 1; i <= b.y; i++) free(d->row[i].s);
  memmove(d->row + a.y + 1, d->row + b.y + 1, (d->n - b.y - 1) * sizeof(Row));
  d->n -= b.y - a.y;
}

/* }================================================================== */


/*
** {==================================================================
** Doc
** ===================================================================
*/

/*
** Caches elsewhere (the highlighter, the conflicts, the preview) remember
** what they worked out for a Doc as (its address, its edits). A Doc that
** is freed and made again can land on the same address, so a fresh one
** never starts at the same count as the one before it: no cache of the
** old text is ever taken for the new one.
*/
static unsigned long g_docgen;


void doc_init (Doc *d) {
  memset(d, 0, sizeof(*d));
  d->edits = ++g_docgen;
  row_new(d, 0);
  d->indent = opt.tab_size;
  d->tabs = !opt.insert_spaces;
  d->enc = opt.encoding;
  edconf_none(&d->ec);
  d->disk_mtime = d->disk_size = -1;
}


static void undo_clear (UndoList *u) {
  size_t i;
  for (i = 0; i < u->n; i++) free(u->v[i].text);
  free(u->v);
  u->v = NULL;
  u->n = u->cap = 0;
}


void doc_free (Doc *d) {
  tm_doc_free(d);
  syntax_doc_free(d);
  quick_forget(d);	/* its git copy too, or the entry keeps a Doc that is gone */
  rows_free(d);
  free(d->hl);
  free(d->depth);
  undo_clear(&d->undo);
  undo_clear(&d->redo);
  free(d->path);
  memset(d, 0, sizeof(*d));
}


/* tabs or spaces, and how many: from the steps between the lines' indents */
void doc_detect_indent (Doc *d) {
  size_t i, j, ntab = 0, nspace = 0;
  size_t hist[17] = {0}, prev = 0;	/* steps 2..8, and read at editor.tabSize, which goes to 16 */
  static const int steps[] = {2, 4, 8, 3};
  int k;
  for (i = 0; i < d->n; i++) {
    const Row *r = &d->row[i];
    if (r->len == 0) continue;
    if (r->s[0] == '\t') {
      ntab++;
      continue;
    }
    for (j = 0; j < r->len && r->s[j] == ' '; j++) ;
    if (j == r->len) continue;	/* only spaces */
    if (j >= 2) nspace++;
    if (j != prev) {
      size_t step = j > prev ? j - prev : prev - j;
      if (step >= 2 && step <= 8) hist[step]++;
    }
    prev = j;
  }
  if (!opt.detect_indent || (ntab == 0 && nspace == 0)) {	/* the settings say */
    d->tabs = !opt.insert_spaces;
    d->indent = opt.tab_size;
    return;
  }
  d->tabs = ntab > nspace;
  d->indent = opt.tab_size;
  {	/* the most common step wins; hist only counts 2 .. 8, so a wider editor.tabSize counts as none */
    size_t have = (d->indent >= 2 && d->indent <= 8) ? hist[d->indent] : 0;
    for (k = 0; k < 4; k++)
      if (hist[steps[k]] > have) {
        have = hist[steps[k]];
        d->indent = steps[k];
      }
  }
}


/*
** {==================================================================
** Encodings: the text is UTF-8; a file may be UTF-16 or Windows 1252
** ===================================================================
*/

static const struct {
  const char *id, *name;
} encs[ENC_N] = {
  {"utf8", "UTF-8"}, {"utf8bom", "UTF-8 with BOM"}, {"utf16le", "UTF-16 LE"},
  {"utf16be", "UTF-16 BE"}, {"windows1252", "Western (Windows 1252)"},
  {"iso88591", "Western (ISO 8859-1)"}
};

/* Windows 1252's 0x80 .. 0x9F; the rest is Latin-1 */
static const uint16_t cp1252[32] = {
  0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
  0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178
};


const char *enc_name (int enc) {
  return (enc >= 0 && enc < ENC_N) ? encs[enc].name : "UTF-8";
}


const char *enc_id (int enc) {
  return (enc >= 0 && enc < ENC_N) ? encs[enc].id : "utf8";
}


int enc_by_id (const char *id) {
  int i;
  for (i = 0; id && i < ENC_N; i++)
    if (strcmp(encs[i].id, id) == 0) return i;
  return -1;
}


/* the encoding of s by its BOM; ENC_N: it has none */
static int bom_of (const char *s, size_t len) {
  const unsigned char *u = (const unsigned char *)s;
  if (len >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) return ENC_UTF8BOM;
  if (len >= 2 && u[0] == 0xFF && u[1] == 0xFE) return ENC_UTF16LE;
  if (len >= 2 && u[0] == 0xFE && u[1] == 0xFF) return ENC_UTF16BE;
  return ENC_N;
}


/* the bytes of a file in enc, as UTF-8 (the BOM gone); malloc'd */
static char *decode (const char *s, size_t len, int enc, size_t *out) {
  Buf b;
  size_t i = 0;
  char u[4];
  buf_init(&b);
  if (enc == ENC_UTF8BOM || enc == ENC_UTF16LE || enc == ENC_UTF16BE) i = bom_of(s, len) == enc ? (enc == ENC_UTF8BOM ? 3 : 2) : 0;
  if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) {
    int le = enc == ENC_UTF16LE;
    for (; i + 1 < len; i += 2) {
      uint32_t c = le ? ((unsigned char)s[i] | (unsigned char)s[i + 1] << 8)
                      : ((unsigned char)s[i] << 8 | (unsigned char)s[i + 1]);
      if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {	/* a surrogate pair */
        uint32_t lo = le ? ((unsigned char)s[i + 2] | (unsigned char)s[i + 3] << 8)
                         : ((unsigned char)s[i + 2] << 8 | (unsigned char)s[i + 3]);
        if (lo >= 0xDC00 && lo < 0xE000) {
          c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
      }
      buf_putn(&b, u, (size_t)utf8_encode(c, u));
    }
  }
  else if (enc == ENC_1252 || enc == ENC_LATIN1) {
    for (; i < len; i++) {
      uint32_t c = (unsigned char)s[i];
      if (enc == ENC_1252 && c >= 0x80 && c < 0xA0) c = cp1252[c - 0x80];
      buf_putn(&b, u, (size_t)utf8_encode(c, u));
    }
  }
  else buf_putn(&b, s + i, len - i);
  *out = b.len;
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/* UTF-8 text put into b in enc (with its BOM when it has one) */
static void encode (Buf *b, const char *s, size_t n, int enc) {
  size_t i = 0, len;
  if (enc == ENC_UTF8) {
    buf_putn(b, s, n);
    return;
  }
  if (enc == ENC_UTF8BOM) {
    buf_puts(b, "\xEF\xBB\xBF");
    buf_putn(b, s, n);
    return;
  }
  if (enc == ENC_UTF16LE) buf_putn(b, "\xFF\xFE", 2);
  if (enc == ENC_UTF16BE) buf_putn(b, "\xFE\xFF", 2);
  while (i < n) {
    uint32_t c = utf8_decode(s + i, n - i, &len);
    if (len == 0) len = 1;
    i += len;
    if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) {
      uint32_t w[2];
      int k, nw = 1;
      w[0] = c;
      if (c >= 0x10000) {
        w[0] = 0xD800 + ((c - 0x10000) >> 10);
        w[1] = 0xDC00 + ((c - 0x10000) & 0x3FF);
        nw = 2;
      }
      for (k = 0; k < nw; k++) {
        char two[2];
        two[enc == ENC_UTF16LE ? 0 : 1] = (char)(w[k] & 255);
        two[enc == ENC_UTF16LE ? 1 : 0] = (char)(w[k] >> 8);
        buf_putn(b, two, 2);
      }
    }
    else {	/* one byte: what 1252 / Latin-1 has of it, else '?' */
      int ch = '?';
      if (c < 0x80 || (c >= 0xA0 && c < 0x100) || (enc == ENC_LATIN1 && c < 0x100)) ch = (int)c;
      else if (enc == ENC_1252) {
        int j;
        for (j = 0; j < 32; j++)
          if (cp1252[j] == c) ch = 0x80 + j;
      }
      buf_putc(b, (char)ch);
    }
  }
}

/* }================================================================== */


/* the lines of s (LF ends them; CR LF was made LF) */
void doc_set_text (Doc *d, const char *s, size_t len) {
  size_t i, from = 0;
  rows_free(d);
  d->n = 0;
  d->hl_n = d->hl_from = d->br_from = d->tm_from = 0;	/* the highlighter starts again */
  d->edits++;
  for (i = 0; i <= len; i++) {
    if (i < len && s[i] != '\n') continue;
    row_append(row_new(d, d->n), s + from, i - from);
    from = i + 1;
  }
}


/* the file's bytes in the text's encoding, CR LF made LF; NULL: it can't be read */
static char *load_bytes (const char *native, int *enc, int *crlf, size_t *len) {
  size_t n, i;
  char *raw = read_file(native, &n), *s;
  int e;
  if (raw == NULL) return NULL;
  e = bom_of(raw, n);
  if (e == ENC_N) e = *enc >= 0 ? *enc : opt.encoding;
  if (e == ENC_UTF8BOM && bom_of(raw, n) != ENC_UTF8BOM) e = ENC_UTF8;
  s = decode(raw, n, e, len);
  free(raw);
  *enc = e;
  *crlf = 0;
  for (i = 0; i < *len; i++)
    if (s[i] == '\n') {
      *crlf = (i > 0 && s[i - 1] == '\r');
      break;
    }
  if (*crlf) crlf_to_lf(s, len);
  return s;
}


int doc_load (Doc *d, const char *native) {
  return doc_load_enc(d, native, -1);
}


int doc_load_enc (Doc *d, const char *native, int enc) {
  size_t len;
  OsStat st;
  char *s;
  int crlf, refs = d->refs, asked = enc;
  doc_free(d);
  doc_init(d);
  d->refs = refs;	/* the tabs that show it still do */
  d->path = xstrdup(native);
  edconf_read(&d->ec, native);	/* .editorconfig: its charset reads the file (a BOM still wins) */
  if (enc < 0) enc = d->ec.enc;
  d->enc = enc >= 0 ? enc : opt.encoding;
  if (os_stat(native, &st) != 0 || !st.exists) {
    edconf_apply(d, 1);
    return 1;
  }
  if (st.is_dir) return -1;
  s = load_bytes(native, &enc, &crlf, &len);
  if (s == NULL) return -1;
  d->enc = asked < 0 && d->ec.enc >= 0 ? d->ec.enc : enc;	/* and it is saved in it */
  d->crlf = crlf;
  doc_set_text(d, s, len);
  free(s);
  doc_detect_indent(d);
  edconf_apply(d, 0);	/* its indent wins over the detected one */
  doc_stamp(d);
  return 0;
}


char *doc_disk_text (const Doc *d, size_t *len) {
  int enc = d->enc, crlf;
  if (d->path == NULL) return NULL;
  return load_bytes(d->path, &enc, &crlf, len);
}


void doc_stamp (Doc *d) {
  OsStat st;
  d->disk_state = DISK_SAME;
  if (d->path && os_stat(d->path, &st) == 0 && st.exists) {
    d->disk_mtime = (long long)st.mtime;
    d->disk_size = st.size;
  }
  else d->disk_mtime = d->disk_size = -1;
}


/* has the file changed since it was read or saved? (the watcher asks) */
int doc_disk_check (Doc *d) {
  OsStat st;
  if (d->path == NULL) return 0;
  if (os_stat(d->path, &st) != 0 || !st.exists) {
    if (d->disk_mtime == -1 || d->disk_state == DISK_GONE) return 0;	/* never there, or known */
    d->disk_state = DISK_GONE;
    return 2;
  }
  if (d->disk_state == DISK_GONE || d->disk_mtime == -1) {	/* there again */
    d->disk_state = DISK_NEWER;
    return 3;
  }
  if ((long long)st.mtime != d->disk_mtime || st.size != d->disk_size) {
    if (d->disk_state == DISK_NEWER) return 0;	/* told already */
    d->disk_state = DISK_NEWER;
    return 1;
  }
  return 0;
}


int doc_save (Doc *d) {
  Buf b, t;
  size_t i;
  int fd, ok;
  if (d->path == NULL) return -1;
  if (d->ec.crlf >= 0) d->crlf = d->ec.crlf;	/* .editorconfig's end_of_line: on save, as its extension does */
  buf_init(&t);
  for (i = 0; i < d->n; i++) {
    if (i > 0) buf_puts(&t, d->crlf ? "\r\n" : "\n");
    buf_putn(&t, d->row[i].s, d->row[i].len);
  }
  buf_init(&b);
  encode(&b, t.s ? t.s : "", t.len, d->enc);
  buf_free(&t);
  fd = os_open(d->path, OS_WRITE);
  if (fd < 0) {
    buf_free(&b);
    return -1;
  }
  ok = b.len == 0 || os_write(fd, b.s, b.len) == (long)b.len;
  os_close(fd);
  buf_free(&b);
  if (!ok) return -1;
  d->saved = d->changes;
  doc_stamp(d);
  return 0;
}


int doc_dirty (const Doc *d) {
  return d->changes != d->saved;
}


Pos doc_clamp (const Doc *d, Pos p) {
  if (p.y >= d->n) {
    p.y = d->n - 1;
    p.x = d->row[p.y].len;
  }
  if (p.x > d->row[p.y].len) p.x = d->row[p.y].len;
  return p;
}


Pos doc_end (const Doc *d) {
  Pos p;
  p.y = d->n - 1;
  p.x = d->row[p.y].len;
  return p;
}


char *doc_text (const Doc *d, Pos a, Pos b, size_t *len) {
  Buf o;
  size_t y;
  buf_init(&o);
  for (y = a.y; y <= b.y; y++) {
    const Row *r = &d->row[y];
    size_t from = (y == a.y) ? a.x : 0, to = (y == b.y) ? b.x : r->len;
    buf_putn(&o, r->s + from, to - from);
    if (y != b.y) buf_putc(&o, '\n');
  }
  if (len) *len = o.len;
  return buf_take(&o);
}

/* }================================================================== */


/*
** {==================================================================
** Undo
** ===================================================================
*/

static void undo_push (UndoList *u, const Undo *e) {
  if (u->n == u->cap) {
    u->cap = u->cap ? u->cap * 2 : 64;
    u->v = (Undo *)xrealloc(u->v, u->cap * sizeof(Undo));
  }
  u->v[u->n++] = *e;
}


static void record (Doc *d, int ins, Pos a, const char *s, size_t n) {
  Undo e;
  e.ins = ins;
  e.a = a;
  e.text = xstrndup(s, n);
  e.len = n;
  e.group = d->group;
  undo_push(&d->undo, &e);
  undo_clear(&d->redo);
  if (d->saved > d->changes) d->saved = -1;	/* that state can't come back */
  d->changes++;
  d->edits++;
  d->changed_at = os_now_us();
}


Pos doc_insert (Doc *d, Pos at, const char *s, size_t n) {
  if (n == 0) return at;
  record(d, 1, at, s, n);
  return raw_insert(d, at, s, n);
}


void doc_delete (Doc *d, Pos a, Pos b) {
  char *s;
  size_t n;
  if (pos_cmp(a, b) >= 0) return;
  s = doc_text(d, a, b, &n);
  record(d, 0, a, s, n);
  free(s);
  raw_delete(d, a, b);
}


void doc_group (Doc *d) {
  d->group++;
}


/* one entry played backwards (undo) or forwards (redo); where the cursor goes */
static Pos play (Doc *d, const Undo *e, int back) {
  d->edits++;
  if (e->ins != back) return raw_insert(d, e->a, e->text, e->len);
  raw_delete(d, e->a, pos_after(e->a, e->text, e->len));
  return e->a;
}


int doc_undo (Doc *d, Pos *cur) {
  long g;
  if (d->undo.n == 0) return 0;
  g = d->undo.v[d->undo.n - 1].group;
  while (d->undo.n > 0 && d->undo.v[d->undo.n - 1].group == g) {
    Undo e = d->undo.v[--d->undo.n];
    *cur = play(d, &e, 1);
    undo_push(&d->redo, &e);
    d->changes--;
  }
  d->group++;
  return 1;
}


int doc_redo (Doc *d, Pos *cur) {
  long g;
  if (d->redo.n == 0) return 0;
  g = d->redo.v[d->redo.n - 1].group;
  while (d->redo.n > 0 && d->redo.v[d->redo.n - 1].group == g) {
    Undo e = d->redo.v[--d->redo.n];
    *cur = play(d, &e, 0);
    undo_push(&d->undo, &e);
    d->changes++;
  }
  d->group++;	/* what comes next is not part of that step */
  return 1;
}

/* }================================================================== */
