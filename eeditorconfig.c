/*
** eeditorconfig.c - .editorconfig: what a project says of its files, like
** VS Code's EditorConfig extension
**
** When a file is opened, the .editorconfig files are looked for from its
** folder up, until one says root = true. Each is an INI file whose section
** names are globs ('*', '**', '?', [abc], [!abc], {a,b}, {1..3}); a glob
** without a '/' is for the file's name anywhere under that folder. The
** farthest file is read first and a later section wins over an earlier
** one, so the closest says the last word. What they say is kept in the
** Doc (d->ec): indent_style, indent_size and tab_width win over the
** detected indent at once, charset reads the file; end_of_line,
** trim_trailing_whitespace and insert_final_newline are done on save, as
** the extension does them (end_of_line at once for a new file).
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* the properties mme uses; each kept as its (lower case) value, "" not set */
enum { EP_STYLE, EP_SIZE, EP_TABW, EP_EOL, EP_CHARSET, EP_TRIM, EP_FINAL, EP_N };

static const char *const ep_name[EP_N] = {
  "indent_style", "indent_size", "tab_width", "end_of_line", "charset",
  "trim_trailing_whitespace", "insert_final_newline"
};

typedef struct Props {
  char v[EP_N][16];
} Props;


/*
** {==================================================================
** Globs
** ===================================================================
*/

static int glob_match (const char *g, const char *s);


/* the '}' that closes the '{' at g, NULL: none (the '{' is itself then) */
static const char *brace_end (const char *g) {
  int depth = 0;
  for (; *g; g++) {
    if (*g == '\\' && g[1]) g++;
    else if (*g == '{') depth++;
    else if (*g == '}' && --depth == 0) return g;
  }
  return NULL;
}


/* {3..12}: the numbers, 1; else 0 */
static int brace_range (const char *a, const char *e, long *lo, long *hi) {
  char *end;
  const char *dots;
  for (dots = a; dots + 1 < e && !(dots[0] == '.' && dots[1] == '.'); dots++) ;
  if (dots + 1 >= e || dots == a || dots + 2 == e) return 0;
  *lo = strtol(a, &end, 10);
  if (end != dots) return 0;
  *hi = strtol(dots + 2, &end, 10);
  if (end != e) return 0;
  if (*lo > *hi) {
    long t = *lo;
    *lo = *hi;
    *hi = t;
  }
  return 1;
}


/* {a,b} at g (close: its '}'): does one of them, then the rest, match s? */
static int brace_match (const char *g, const char *close, const char *s) {
  const char *a = g + 1, *p;
  long lo, hi;
  int depth = 0, found = 0;
  if (brace_range(a, close, &lo, &hi)) {	/* a number in the range: every length of digits is tried */
    size_t k = *s == '-', d;
    long v = 0;
    for (d = k; s[d] >= '0' && s[d] <= '9' && v < 100000000L; d++) {
      if (d > k && s[k] == '0') break;	/* 07 is not 7 */
      v = v * 10 + (s[d] - '0');
      if ((k ? -v : v) >= lo && (k ? -v : v) <= hi && glob_match(close + 1, s + d + 1)) return 1;
    }
    return 0;
  }
  for (p = a; p < close; p++) {	/* a single one ({abc}) is the text "{abc}" */
    if (*p == '\\' && p + 1 < close) p++;
    else if (*p == '{') depth++;
    else if (*p == '}') depth--;
    else if (*p == ',' && depth == 0) found = 1;
  }
  if (!found) return -1;
  for (p = a;; p++) {	/* each alternative, and the glob after the braces */
    if (p == close || (*p == ',' && depth == 0)) {
      size_t n = (size_t)(p - a), rest = strlen(close + 1);
      char *alt = (char *)xmalloc(n + rest + 1);
      int ok;
      memcpy(alt, a, n);
      memcpy(alt + n, close + 1, rest + 1);
      ok = glob_match(alt, s);
      free(alt);
      if (ok) return 1;
      if (p == close) return 0;
      a = p + 1;
    }
    else if (*p == '\\' && p + 1 < close) p++;
    else if (*p == '{') depth++;
    else if (*p == '}') depth--;
  }
}


/* [abc], [!a-z] at *g: 1 c is in it, 0 not, -1 it is no class ('[' is itself); *g after it */
static int class_match (const char **g, int c) {
  const char *p = *g + 1;
  int neg = 0, in = 0;
  if (*p == '!' || *p == '^') {
    neg = 1;
    p++;
  }
  if (*p == ']') {	/* []] and [!]]: a ']' first is one of them */
    in = c == ']';
    p++;
  }
  for (; *p && *p != ']'; p++) {
    int a = (unsigned char)(*p == '\\' && p[1] ? *++p : *p), b = a;
    if (*p == '/') return -1;	/* a class never spans folders */
    if (p[1] == '-' && p[2] && p[2] != ']') {
      b = (unsigned char)p[2];
      p += 2;
    }
    if (c >= a && c <= b) in = 1;
  }
  if (*p != ']') return -1;
  *g = p;
  return in != neg;
}


/* does glob g match all of s? '*' and '?' stop at '/', '**' does not */
static int glob_match (const char *g, const char *s) {
  for (; *g; g++) {
    switch (*g) {
      case '*':
        if (g[1] == '*') {	/* two stars: any path, none too ("a/<2 stars>/b" is "a/b") */
          const char *r = g + 2;
          while (*r == '*') r++;
          if (*r == '/' && glob_match(r + 1, s)) return 1;
          for (;; s++) {
            if (glob_match(r, s)) return 1;
            if (*s == '\0') return 0;
          }
        }
        for (;; s++) {
          if (glob_match(g + 1, s)) return 1;
          if (*s == '\0' || *s == '/') return 0;
        }
      case '?':
        if (*s == '\0' || *s == '/') return 0;
        s += utf8_len(s);
        break;
      case '[': {
        const char *at = g;
        int r;
        if (*s == '\0' || *s == '/') return 0;
        r = class_match(&at, (unsigned char)*s);
        if (r == 0) return 0;
        if (r == 1) g = at;
        else if (*s != '[') return 0;	/* no class: a '[' */
        s++;
        break;
      }
      case '{': {
        const char *close = brace_end(g);
        int r = close ? brace_match(g, close, s) : -1;
        if (r >= 0) return r;
        if (*s != '{') return 0;	/* no braces: a '{' */
        s++;
        break;
      }
      case '\\':
        if (g[1]) g++;
        /* fall through */
      default:
        if (*s != *g) return 0;
        s++;
    }
  }
  return *s == '\0';
}


/* is a section's glob for rel, the file's path from the .editorconfig's folder ('/' between)? */
static int section_for (const char *glob, const char *rel) {
  char *g;
  int ok;
  if (strchr(glob, '/') == NULL) g = xstrcat3("**/", glob, "");	/* its name, in any folder */
  else g = xstrdup(glob[0] == '/' ? glob + 1 : glob);
  ok = glob_match(g, rel);
  free(g);
  return ok;
}

/* }================================================================== */


/*
** {==================================================================
** Reading them
** ===================================================================
*/

static char *trim (char *a, char *e) {
  while (a < e && (*a == ' ' || *a == '\t')) a++;
  while (e > a && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
  *e = '\0';
  return a;
}


static void lower (char *s) {
  for (; *s; s++)
    if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}


/*
** One .editorconfig's text (changed in place): the sections for rel set
** their properties in p ("unset" takes one back). 1: its preamble says
** root = true.
*/
static int read_one (char *text, const char *rel, Props *p) {
  char *line = text, *next;
  int in = 0, hit = 0, root = 0, i;
  for (; line; line = next) {
    char *e = strchr(line, '\n'), *s, *eq;
    next = e ? e + 1 : NULL;
    s = trim(line, e ? e : line + strlen(line));
    if (*s == '\0' || *s == '#' || *s == ';') continue;
    if (*s == '[') {	/* [glob]: its last ']' ends it, a glob has classes */
      char *close = strrchr(s, ']');
      in = 1;
      hit = 0;
      if (close == NULL || close == s + 1) continue;
      *close = '\0';
      hit = section_for(s + 1, rel);
      continue;
    }
    eq = strpbrk(s, "=:");
    if (eq == NULL) continue;
    {
      char *key = trim(s, eq), *val = eq + 1, *c = strpbrk(val, "#;");	/* a comment after it goes */
      val = trim(val, c ? c : val + strlen(val));
      lower(key);
      lower(val);
      if (!in) {
        if (strcmp(key, "root") == 0) root = strcmp(val, "true") == 0;
        continue;
      }
      if (!hit) continue;
      for (i = 0; i < EP_N; i++)
        if (strcmp(key, ep_name[i]) == 0)
          snprintf(p->v[i], sizeof(p->v[i]), "%s", strcmp(val, "unset") == 0 ? "" : val);
    }
  }
  return root;
}


/* a number 1 .. 16 (editor.tabSize's), -1 none */
static int num (const char *s) {
  long v;
  char *end;
  if (*s == '\0') return -1;
  v = strtol(s, &end, 10);
  if (*end || v < 1) return -1;
  return v > 16 ? 16 : (int)v;
}


/* the properties as mme's: EditorConfig's own rules, then the extension's */
static void resolve (EdConf *c, Props *p) {
  static const char *const cs[] = {"utf-8", "utf-8-bom", "utf-16le", "utf-16be", "latin1"};
  static const int ce[] = {ENC_UTF8, ENC_UTF8BOM, ENC_UTF16LE, ENC_UTF16BE, ENC_LATIN1};
  const char *style = p->v[EP_STYLE], *size = p->v[EP_SIZE];
  int tabw, isize, i, tabs;
  if (strcmp(style, "tab") == 0 && *size == '\0') size = "tab";	/* indent_size is then tab_width */
  isize = num(size);
  tabw = num(p->v[EP_TABW]);
  if (isize > 0 && tabw < 0) tabw = isize;
  if (strcmp(size, "tab") == 0) isize = tabw;
  tabs = strcmp(style, "tab") == 0 ? 1 : strcmp(style, "space") == 0 ? 0 : strcmp(size, "tab") == 0 ? 1 : -1;
  c->tabs = tabs;
  c->indent = tabs == 1 ? (tabw > 0 ? tabw : isize) : (isize > 0 ? isize : tabw);
  c->tabw = tabw;
  c->crlf = strcmp(p->v[EP_EOL], "crlf") == 0 ? 1 : strcmp(p->v[EP_EOL], "lf") == 0 ? 0 : -1;	/* "cr": not one mme writes */
  c->enc = -1;
  for (i = 0; i < 5; i++)
    if (strcmp(p->v[EP_CHARSET], cs[i]) == 0) c->enc = ce[i];
  c->trim = strcmp(p->v[EP_TRIM], "true") == 0 ? 1 : strcmp(p->v[EP_TRIM], "false") == 0 ? 0 : -1;
  c->final_nl = strcmp(p->v[EP_FINAL], "true") == 0 ? 1 : strcmp(p->v[EP_FINAL], "false") == 0 ? 0 : -1;
}


void edconf_none (EdConf *c) {
  c->tabs = c->indent = c->tabw = c->crlf = c->enc = c->trim = c->final_nl = -1;
}


void edconf_read (EdConf *c, const char *native) {
  char *dir = path_dirname(native), *abs = os_realpath(dir), *full, *text[64];
  size_t cut[64], n = 0, i;
  Props p;
  edconf_none(c);
  if (abs == NULL) abs = xstrdup(dir);
  free(dir);
  full = path_join(abs, path_basename(native));
  for (;;) {	/* from its folder up, until root = true */
    char *f = path_join(abs, ".editorconfig"), *up, *t = read_file(f, NULL);
    free(f);
    if (t && n < 64) {
      Props none;
      char *copy = xstrdup(t);
      int root;
      memset(&none, 0, sizeof(none));
      root = read_one(copy, "", &none);
      free(copy);
      text[n] = t;
      cut[n++] = strlen(abs);
      if (root) break;
    }
    else free(t);
    up = path_dirname(abs);
    if (strlen(up) >= strlen(abs)) {	/* the top */
      free(up);
      break;
    }
    free(abs);
    abs = up;
  }
  memset(&p, 0, sizeof(p));
  for (i = n; i-- > 0;) {	/* the farthest first: the closest wins */
    char *rel = full + cut[i], *s;
    while (path_is_sep(*rel)) rel++;
    rel = xstrdup(rel);
    for (s = rel; *s; s++)
      if (path_is_sep(*s)) *s = '/';
    read_one(text[i], rel, &p);
    free(rel);
    free(text[i]);
  }
  resolve(c, &p);
  free(full);
  free(abs);
}


void edconf_apply (Doc *d, int is_new) {
  const EdConf *c = &d->ec;
  if (c->tabs >= 0) d->tabs = c->tabs;
  if (c->indent > 0) d->indent = c->indent;
  if (c->tabw > 0) d->tabw = c->tabw;
  if (is_new && c->crlf >= 0) d->crlf = c->crlf;
}

/* }================================================================== */
