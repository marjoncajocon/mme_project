/*
** ejson.c - JSON: read into a tree, write with escapes
**
** For settings.json (which, like VS Code's, may have // and block comments
** and a comma before a closing bracket) and for the messages of language
** servers. A tree is one malloc'd node per value.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct JParse {
  const char *s, *e;
  int depth;
} JParse;


static void skip (JParse *p) {
  for (;;) {
    while (p->s < p->e && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')) p->s++;
    if (p->s + 1 < p->e && p->s[0] == '/' && p->s[1] == '/') {
      while (p->s < p->e && *p->s != '\n') p->s++;
    }
    else if (p->s + 1 < p->e && p->s[0] == '/' && p->s[1] == '*') {
      p->s += 2;
      while (p->s + 1 < p->e && !(p->s[0] == '*' && p->s[1] == '/')) p->s++;
      p->s = p->s + 2 <= p->e ? p->s + 2 : p->e;
    }
    else return;
  }
}


static Json *node (int type) {
  Json *j = (Json *)xmalloc(sizeof(Json));
  memset(j, 0, sizeof(*j));
  j->type = type;
  return j;
}


static void add_kid (Json *parent, Json *kid) {
  if (parent->n == parent->cap) {
    parent->cap = parent->cap ? parent->cap * 2 : 8;
    parent->kid = (Json **)xrealloc(parent->kid, parent->cap * sizeof(Json *));
  }
  parent->kid[parent->n++] = kid;
}


static unsigned hex4 (const char *s) {
  unsigned v = 0;
  int i;
  for (i = 0; i < 4; i++) {
    char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if ((c | 32) >= 'a' && (c | 32) <= 'f') v |= (unsigned)((c | 32) - 'a' + 10);
  }
  return v;
}


/* a string after its opening quote; NULL when it does not end */
static char *string (JParse *p, size_t *len) {
  Buf b;
  buf_init(&b);
  while (p->s < p->e && *p->s != '"') {
    char c = *p->s++;
    if (c != '\\') {
      buf_putc(&b, c);
      continue;
    }
    if (p->s >= p->e) break;
    c = *p->s++;
    switch (c) {
      case 'n': buf_putc(&b, '\n'); break;
      case 't': buf_putc(&b, '\t'); break;
      case 'r': buf_putc(&b, '\r'); break;
      case 'b': buf_putc(&b, '\b'); break;
      case 'f': buf_putc(&b, '\f'); break;
      case 'u': {
        unsigned cp;
        char u[4];
        if (p->e - p->s < 4) break;
        cp = hex4(p->s);
        p->s += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && p->e - p->s >= 6 && p->s[0] == '\\' && p->s[1] == 'u') {
          unsigned lo = hex4(p->s + 2);	/* a surrogate pair */
          if (lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p->s += 6;
          }
        }
        buf_putn(&b, u, (size_t)utf8_encode(cp, u));
        break;
      }
      default: buf_putc(&b, c);
    }
  }
  if (p->s >= p->e) {
    buf_free(&b);
    return NULL;
  }
  p->s++;
  *len = b.len;
  buf_putc(&b, '\0');
  return buf_take(&b);
}


static Json *value (JParse *p) {
  Json *j;
  skip(p);
  if (p->s >= p->e || ++p->depth > 200) return NULL;
  switch (*p->s) {
    case '{': case '[': {
      char close = *p->s == '{' ? '}' : ']';
      j = node(close == '}' ? J_OBJ : J_ARR);
      p->s++;
      for (;;) {
        Json *kid;
        char *key = NULL;
        size_t klen;
        skip(p);
        if (p->s < p->e && *p->s == close) {
          p->s++;
          break;
        }
        if (close == '}') {
          if (p->s >= p->e || *p->s != '"') goto bad;
          p->s++;
          if ((key = string(p, &klen)) == NULL) goto bad;
          skip(p);
          if (p->s >= p->e || *p->s != ':') {
            free(key);
            goto bad;
          }
          p->s++;
        }
        kid = value(p);
        if (kid == NULL) {
          free(key);
          goto bad;
        }
        kid->key = key;
        add_kid(j, kid);
        skip(p);
        if (p->s < p->e && *p->s == ',') p->s++;
        else if (p->s >= p->e || *p->s != close) goto bad;
      }
      break;
    }
    case '"':
      j = node(J_STR);
      p->s++;
      if ((j->str = string(p, &j->len)) == NULL) goto bad;
      break;
    case 't': case 'f': case 'n':
      if (p->e - p->s >= 4 && strncmp(p->s, "true", 4) == 0) (j = node(J_BOOL))->b = 1, p->s += 4;
      else if (p->e - p->s >= 5 && strncmp(p->s, "false", 5) == 0) j = node(J_BOOL), p->s += 5;
      else if (p->e - p->s >= 4 && strncmp(p->s, "null", 4) == 0) j = node(J_NULL), p->s += 4;
      else return NULL;
      break;
    default: {
      char num[64];
      size_t k = 0;
      while (p->s < p->e && k + 1 < sizeof(num) && strchr("+-0123456789.eE", *p->s)) num[k++] = *p->s++;
      if (k == 0) return NULL;
      num[k] = '\0';
      j = node(J_NUM);
      j->num = strtod(num, NULL);
    }
  }
  p->depth--;
  return j;
bad:
  json_free(j);
  return NULL;
}


Json *json_parse (const char *s, size_t n) {
  JParse p;
  p.s = s;
  p.e = s + n;
  p.depth = 0;
  return value(&p);
}


void json_free (Json *j) {
  size_t i;
  if (j == NULL) return;
  for (i = 0; i < j->n; i++) json_free(j->kid[i]);
  free(j->kid);
  free(j->key);
  free(j->str);
  free(j);
}


/*
** "a.b.c": the member c of the member b of a; NULL when one is not there.
** A dot that is part of a name is written "\.": "editor\.tabSize".
*/
const Json *json_get (const Json *j, const char *path) {
  char key[256];
  while (j && *path) {
    size_t n = 0, i;
    const Json *found = NULL;
    while (*path && *path != '.' && n + 1 < sizeof(key)) {
      if (path[0] == '\\' && path[1] == '.') path++;
      key[n++] = *path++;
    }
    key[n] = '\0';
    if (*path == '.') path++;
    if (j->type != J_OBJ) return NULL;
    for (i = 0; i < j->n; i++)
      if (j->kid[i]->key && strcmp(j->kid[i]->key, key) == 0) found = j->kid[i];
    j = found;
  }
  return j;
}


const char *json_str (const Json *j, const char *def) {
  return (j && j->type == J_STR) ? j->str : def;
}


double json_num (const Json *j, double def) {
  return (j && j->type == J_NUM) ? j->num : def;
}


int json_bool (const Json *j, int def) {
  return (j && j->type == J_BOOL) ? j->b : def;
}


void json_put_str (Buf *b, const char *s, size_t n) {
  size_t i;
  buf_putc(b, '"');
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      buf_putc(b, '\\');
      buf_putc(b, (char)c);
    }
    else if (c == '\n') buf_puts(b, "\\n");
    else if (c == '\r') buf_puts(b, "\\r");
    else if (c == '\t') buf_puts(b, "\\t");
    else if (c < 32) buf_printf(b, "\\u%04x", c);
    else buf_putc(b, (char)c);
  }
  buf_putc(b, '"');
}


/* the tree as JSON text again (for what goes back to a server as it came) */
void json_write (Buf *b, const Json *j) {
  size_t i;
  switch (j->type) {
    case J_NULL: buf_puts(b, "null"); break;
    case J_BOOL: buf_puts(b, j->b ? "true" : "false"); break;
    case J_NUM:
      if (j->num > -9e18 && j->num < 9e18 && j->num == (double)(long long)j->num)	/* a cast of 1e300 is undefined */
        buf_printf(b, "%lld", (long long)j->num);
      else buf_printf(b, "%.17g", j->num);
      break;
    case J_STR: json_put_str(b, j->str, j->len); break;
    case J_ARR: case J_OBJ:
      buf_putc(b, j->type == J_ARR ? '[' : '{');
      for (i = 0; i < j->n; i++) {
        if (i) buf_putc(b, ',');
        if (j->type == J_OBJ) {
          json_put_str(b, j->kid[i]->key, strlen(j->kid[i]->key));
          buf_putc(b, ':');
        }
        json_write(b, j->kid[i]);
      }
      buf_putc(b, j->type == J_ARR ? ']' : '}');
      break;
  }
}
