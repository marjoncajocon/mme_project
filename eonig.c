/*
** eonig.c - regular expressions as TextMate grammars write them
**
** VS Code's grammars are written for Oniguruma (its Ruby syntax): (?x)
** and (?i), lookahead and lookbehind, atomic groups, lazy and possessive
** quantifiers, \G \A \z \Z, named groups, back references, \g<name>
** calls, POSIX brackets and class intersection (&&). A pattern is parsed
** into a tree of nodes; the matcher backtracks over it, a continuation
** saying what comes after a group. Text is UTF-8 bytes; a character that
** is not ASCII is a letter for \w and the like (near enough).
*/

#include "mme.h"

#include <stdlib.h>
#include <string.h>


enum { N_STR, N_ANY, N_CLASS, N_BOL, N_EOL, N_BOS, N_EOS, N_EOSNL, N_G, N_WORDB, N_NWORDB,
       N_GROUP, N_ALT, N_REP, N_LOOK, N_ATOMIC, N_BACKREF, N_CALL, N_EMPTY };

enum { F_ICASE = 1, F_EXT = 2, F_DOTALL = 4 };

/* non-ASCII characters a class takes in, roughly */
enum { HI_ALL = 1, HI_WORD = 2, HI_SPACE = 4 };

typedef struct Class Class;
struct Class {
  unsigned char bits[16];	/* bytes 0..127 */
  uint32_t *rng;	/* code points from 128: from, to */
  int nrng, caprng;
  int hi;	/* HI_*: non-ASCII sets */
  Class **sub;	/* [a[bc]]: more of them */
  int nsub, capsub;
  Class *and;	/* [a-z&&[^aeiou]]: and this too */
  int neg;
};

typedef struct Node Node;
struct Node {
  unsigned char type, icase, greedy, possessive, neg, behind, dotall;
  Node *next;
  unsigned char *str;	/* N_STR */
  int len;
  Class *cls;	/* N_CLASS */
  Node *kid;	/* N_GROUP, N_REP, N_LOOK, N_ATOMIC */
  Node **alt;	/* N_ALT */
  int nalt;
  int min, max;	/* N_REP; max -1: no end */
  int cap;	/* N_GROUP: its number, -1 none; N_BACKREF, N_CALL: the group's */
  int lens[8], nlens;	/* N_LOOK behind: its lengths in characters; 0: they vary */
};

typedef struct Block {
  struct Block *next;
  size_t used, size;
  /* the memory follows */
} Block;

struct Onig {
  Node *root;
  int ngroup;
  Node **group;	/* by number, for \g<n> (HTML's entities have 900 groups) */
  char **name;	/* a group's name, or NULL */
  int capgroup;
  unsigned char first[32];	/* the bytes a match can start with */
  int has_first;
  int anch;	/* ANCH_*: where a match can start */
  int uses_g;	/* \G or \A is in it: its matches depend on more than the text */
  Block *mem;
};

enum { ANCH_NONE, ANCH_G, ANCH_BOL, ANCH_A };

#define NONE	((size_t)-1)
#define MAX_STEPS	400000
#define MAX_DEPTH	2000


/*
** {==================================================================
** Memory: a regex's nodes live in its blocks, freed at once
** ===================================================================
*/

static void *alloc (Onig *re, size_t n) {
  Block *b = re->mem;
  n = (n + 15) & ~(size_t)15;
  if (b == NULL || b->used + n > b->size) {
    size_t size = n > 4000 ? n : 4000;
    b = (Block *)xmalloc(sizeof(Block) + size);
    b->next = re->mem;
    b->used = 0;
    b->size = size;
    re->mem = b;
  }
  {
    void *p = (char *)(b + 1) + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
  }
}


static Node *node (Onig *re, int type) {
  Node *n = (Node *)alloc(re, sizeof(Node));
  n->type = (unsigned char)type;
  n->cap = -1;
  return n;
}


static Class *new_class (Onig *re) {
  return (Class *)alloc(re, sizeof(Class));
}


void onig_free (Onig *re) {
  Block *b, *nb;
  int i;
  if (re == NULL) return;
  for (b = re->mem; b; b = nb) {
    nb = b->next;
    free(b);
  }
  for (i = 0; i <= re->ngroup && re->name; i++) free(re->name[i]);
  free(re->name);
  free(re->group);
  free(re);
}


int onig_groups (const Onig *re) {
  return re->ngroup;
}


int onig_uses_g (const Onig *re) {
  return re->uses_g;
}

/* }================================================================== */


/*
** {==================================================================
** Classes
** ===================================================================
*/

static int lower (int c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}


static int upper (int c) {
  return (c >= 'a' && c <= 'z') ? c - 32 : c;
}


static int is_word_cp (uint32_t c) {
  if (c < 128) return c == '_' || (c >= '0' && c <= '9') || (lower((int)c) >= 'a' && lower((int)c) <= 'z');
  return !(c == 0xA0 || (c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) || c == 0xFEFF);
}


static int is_space_hi (uint32_t c) {
  return c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}


static void bit_set (Class *c, int b) {
  c->bits[b >> 3] |= (unsigned char)(1 << (b & 7));
}


static void add_range (Onig *re, Class *c, uint32_t a, uint32_t b, int icase) {
  uint32_t x;
  for (x = a; x <= b && x < 128; x++) {
    bit_set(c, (int)x);
    if (icase) {
      bit_set(c, lower((int)x));
      bit_set(c, upper((int)x));
    }
  }
  if (b >= 128) {
    if (a < 128) a = 128;
    if (c->nrng + 2 > c->caprng) {
      uint32_t *nr;
      c->caprng = c->caprng ? c->caprng * 2 : 8;
      nr = (uint32_t *)alloc(re, (size_t)c->caprng * sizeof(uint32_t));
      if (c->nrng) memcpy(nr, c->rng, (size_t)c->nrng * sizeof(uint32_t));
      c->rng = nr;
    }
    c->rng[c->nrng++] = a;
    c->rng[c->nrng++] = b;
  }
}


static void add_sub (Onig *re, Class *c, Class *s) {
  if (c->nsub == c->capsub) {
    Class **ns;
    c->capsub = c->capsub ? c->capsub * 2 : 4;
    ns = (Class **)alloc(re, (size_t)c->capsub * sizeof(Class *));
    if (c->nsub) memcpy(ns, c->sub, (size_t)c->nsub * sizeof(Class *));
    c->sub = ns;
  }
  c->sub[c->nsub++] = s;
}


static int cls_has (const Class *c, uint32_t cp) {
  int r = 0, i;
  if (cp < 128) r = (c->bits[cp >> 3] >> (cp & 7)) & 1;
  else {
    if ((c->hi & HI_ALL) || ((c->hi & HI_WORD) && is_word_cp(cp)) || ((c->hi & HI_SPACE) && is_space_hi(cp))) r = 1;
    for (i = 0; !r && i < c->nrng; i += 2)
      if (cp >= c->rng[i] && cp <= c->rng[i + 1]) r = 1;
  }
  for (i = 0; !r && i < c->nsub; i++) r = cls_has(c->sub[i], cp);
  if (r && c->and) r = cls_has(c->and, cp);
  return c->neg ? !r : r;
}


/* \d \w \s \h and their negations */
static Class *shorthand (Onig *re, int e) {
  Class *c = new_class(re);
  int x;
  switch (lower(e)) {
    case 'd': add_range(re, c, '0', '9', 0); break;
    case 'h': add_range(re, c, '0', '9', 0); add_range(re, c, 'a', 'f', 0); add_range(re, c, 'A', 'F', 0); break;
    case 'w':
      add_range(re, c, '0', '9', 0);
      add_range(re, c, 'a', 'z', 0);
      add_range(re, c, 'A', 'Z', 0);
      bit_set(c, '_');
      c->hi = HI_WORD;
      break;
    case 's':
      for (x = 9; x <= 13; x++) bit_set(c, x);
      bit_set(c, ' ');
      c->hi = HI_SPACE;
      break;
  }
  if (e >= 'A' && e <= 'Z') c->neg = 1;
  return c;
}


/* [:alpha:] and \p{Alpha}: 0 when the name is not known */
static int named_class (Onig *re, Class *c, const char *nm, size_t n) {
  static const struct {
    const char *name;
    const char *what;	/* ranges as pairs of bytes; a letter after: the non-ASCII */
  } tab[] = {
    {"alpha", "azAZ" "w"}, {"alnum", "azAZ09" "w"}, {"digit", "09"}, {"upper", "AZ"},
    {"lower", "az"}, {"space", "\t\r  " "s"}, {"blank", "\t\t  "}, {"punct", "!/:@[`{~"},
    {"xdigit", "09afAF"}, {"word", "azAZ09__" "w"}, {"cntrl", "\x01\x1f\x7f\x7f"},
    {"print", " ~" "a"}, {"graph", "!~" "a"}, {"ascii", "\x01\x7f"},
    {"l", "azAZ" "w"}, {"letter", "azAZ" "w"}, {"lu", "AZ"}, {"ll", "az"}, {"lt", "AZ"}, {"lm", "" "w"},
    {"lo", "" "w"}, {"n", "09"}, {"nd", "09"}, {"number", "09"}, {"nl", ""}, {"no", ""},
    {"p", "!/:@[`{~"}, {"s", "$$++<>^^``||~~"}, {"sm", "++<>||~~"}, {"sc", "$$"},
    {"z", "  " "s"}, {"zs", "  " "s"}, {"m", ""}, {"mn", ""}, {"mc", ""}, {"pc", "__"},
    {"pd", "--"}, {"ps", "(([[{{"}, {"pe", "))]]}}"}, {"po", "!!\"\"##%%''**,,..//::;;??@@\\\\"},
    {"han", "" "w"}, {"hiragana", "" "w"}, {"katakana", "" "w"}, {"latin", "azAZ" "w"},
    {"greek", "" "w"}, {"cyrillic", "" "w"}, {"any", "\x01\x7f" "a"}, {"emoji", "" "w"}
  };
  char low[32];
  size_t i, k;
  if (n >= sizeof(low)) return 0;
  for (i = 0, k = 0; i < n; i++)
    if (nm[i] != '_' && nm[i] != ' ' && nm[i] != '-') low[k++] = (char)lower((unsigned char)nm[i]);
  low[k] = '\0';
  if (strncmp(low, "is", 2) == 0 && strlen(low) > 2 && strcmp(low, "isolated") != 0) memmove(low, low + 2, k - 1);
  for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
    if (strcmp(low, tab[i].name) == 0) {
      const char *w = tab[i].what;
      size_t wl = strlen(w);
      for (k = 0; k + 1 < wl; k += 2) add_range(re, c, (unsigned char)w[k], (unsigned char)w[k + 1], 0);
      if (wl % 2 == 1) c->hi |= w[wl - 1] == 'w' ? HI_WORD : w[wl - 1] == 's' ? HI_SPACE : HI_ALL;
      return 1;
    }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Parsing
** ===================================================================
*/

typedef struct P {
  const char *s;
  size_t i, n;
  Onig *re;
  const char *err;
  int depth;
} P;


static Node *parse_alt (P *p, int flags);


static int hexd (int c) {
  return c >= '0' && c <= '9' ? c - '0' : lower(c) >= 'a' && lower(c) <= 'f' ? lower(c) - 'a' + 10 : -1;
}


static int enc (uint32_t cp, unsigned char *out) {
  if (cp < 0x80) {
    out[0] = (unsigned char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (unsigned char)(0xC0 | (cp >> 6));
    out[1] = (unsigned char)(0x80 | (cp & 63));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (unsigned char)(0xE0 | (cp >> 12));
    out[1] = (unsigned char)(0x80 | ((cp >> 6) & 63));
    out[2] = (unsigned char)(0x80 | (cp & 63));
    return 3;
  }
  out[0] = (unsigned char)(0xF0 | (cp >> 18));
  out[1] = (unsigned char)(0x80 | ((cp >> 12) & 63));
  out[2] = (unsigned char)(0x80 | ((cp >> 6) & 63));
  out[3] = (unsigned char)(0x80 | (cp & 63));
  return 4;
}


/* the code point at p->i (a UTF-8 character), moved over */
static uint32_t next_cp (P *p) {
  size_t len;
  uint32_t cp = utf8_decode(p->s + p->i, p->n - p->i, &len);
  p->i += len ? len : 1;
  return cp;
}


/* after a backslash, an escape that is one character (\t, \x41, é ...); -1: not one */
static long escape_char (P *p, int e) {
  long v = 0;
  int d, k;
  switch (e) {
    case 't': return '\t';
    case 'n': return '\n';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return 7;
    case 'e': return 27;
    case 'c':
      if (p->i < p->n) return p->s[p->i++] & 31;
      return 'c';
    case 'x':
      if (p->i < p->n && p->s[p->i] == '{') {
        p->i++;
        while (p->i < p->n && p->s[p->i] != '}' && (d = hexd((unsigned char)p->s[p->i])) >= 0) {
          v = v * 16 + d;
          p->i++;
        }
        if (p->i < p->n && p->s[p->i] == '}') p->i++;
        return v;
      }
      for (k = 0; k < 2 && p->i < p->n && (d = hexd((unsigned char)p->s[p->i])) >= 0; k++, p->i++) v = v * 16 + d;
      return v;
    case 'u':
      for (k = 0; k < 4 && p->i < p->n && (d = hexd((unsigned char)p->s[p->i])) >= 0; k++, p->i++) v = v * 16 + d;
      return v;
    case '0':
      for (k = 0; k < 2 && p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '7'; k++, p->i++) v = v * 8 + (p->s[p->i] - '0');
      return v;
  }
  return -1;
}


/* \p{Alpha} \p{^Alpha} \P{..}: into c */
static int prop_class (P *p, Class *c, int big) {
  size_t a;
  Class *k;
  int neg = big;
  if (p->i >= p->n || p->s[p->i] != '{') return 0;
  p->i++;
  if (p->i < p->n && p->s[p->i] == '^') {
    neg = !neg;
    p->i++;
  }
  a = p->i;
  while (p->i < p->n && p->s[p->i] != '}') p->i++;
  k = new_class(p->re);
  if (!named_class(p->re, k, p->s + a, p->i - a)) k->hi = HI_WORD;	/* not known: letters, near enough */
  if (p->i < p->n) p->i++;
  k->neg = neg;
  add_sub(p->re, c, k);
  return 1;
}


static Class *parse_class (P *p, int flags);


/* the class body after '[' (and '^'), to its ']' */
static void class_items (P *p, Class *c, int flags) {
  int first = 1;
  while (p->i < p->n) {
    uint32_t a, b;
    int ch = (unsigned char)p->s[p->i];
    if (ch == ']' && !first) {
      p->i++;
      return;
    }
    first = 0;
    if (ch == '[' && p->i + 1 < p->n && p->s[p->i + 1] == ':') {	/* [:alpha:] [:^alpha:] */
      const char *e = strstr(p->s + p->i + 2, ":]");
      if (e && (size_t)(e - p->s) < p->n) {
        size_t a0 = p->i + 2;
        Class *k = new_class(p->re);
        if (p->s[a0] == '^') {
          k->neg = 1;
          a0++;
        }
        named_class(p->re, k, p->s + a0, (size_t)(e - p->s) - a0);
        add_sub(p->re, c, k);
        p->i = (size_t)(e - p->s) + 2;
        continue;
      }
    }
    if (ch == '[') {	/* a class in the class: added */
      p->i++;
      add_sub(p->re, c, parse_class(p, flags));
      continue;
    }
    if (ch == '&' && p->i + 1 < p->n && p->s[p->i + 1] == '&') {	/* the rest must match too */
      Class *r = new_class(p->re);
      p->i += 2;
      class_items(p, r, flags);
      p->i--;	/* its ']' is ours */
      c->and = r;
      if (p->i < p->n && p->s[p->i] == ']') p->i++;
      return;
    }
    if (ch == '\\' && p->i + 1 < p->n) {
      int e = (unsigned char)p->s[p->i + 1];
      long v;
      p->i += 2;
      if (strchr("dDwWsShH", e)) {
        add_sub(p->re, c, shorthand(p->re, e));
        continue;
      }
      if (e == 'p' || e == 'P') {
        prop_class(p, c, e == 'P');
        continue;
      }
      if (e == 'b') a = 8;
      else if ((v = escape_char(p, e)) >= 0) a = (uint32_t)v;
      else {
        p->i--;
        a = next_cp(p);
      }
    }
    else a = next_cp(p);
    b = a;
    if (p->i + 1 < p->n && p->s[p->i] == '-' && p->s[p->i + 1] != ']') {	/* a range */
      p->i++;
      if (p->s[p->i] == '\\' && p->i + 1 < p->n) {
        long v;
        int e = (unsigned char)p->s[p->i + 1];
        p->i += 2;
        if ((v = escape_char(p, e)) >= 0) b = (uint32_t)v;
        else {
          p->i--;
          b = next_cp(p);
        }
      }
      else b = next_cp(p);
      if (b < a) b = a;
    }
    add_range(p->re, c, a, b, flags & F_ICASE);
  }
  p->err = "a class without its ]";
}


static Class *parse_class (P *p, int flags) {
  Class *c = new_class(p->re);
  if (p->i < p->n && p->s[p->i] == '^') {
    c->neg = 1;
    p->i++;
  }
  class_items(p, c, flags);
  return c;
}


static Node *class_node (P *p, Class *c) {
  Node *n = node(p->re, N_CLASS);
  n->cls = c;
  return n;
}


static Node *str_node (P *p, const unsigned char *b, int len, int flags) {
  Node *n = node(p->re, N_STR);
  n->str = (unsigned char *)alloc(p->re, (size_t)len + 1);
  memcpy(n->str, b, (size_t)len);
  n->len = len;
  n->icase = (flags & F_ICASE) != 0;
  return n;
}


/* one more group: its number */
static int new_group (Onig *re) {
  int g = ++re->ngroup;
  if (g >= re->capgroup) {
    int old = re->capgroup;
    re->capgroup = re->capgroup ? re->capgroup * 2 : 32;
    re->group = (Node **)xrealloc(re->group, (size_t)re->capgroup * sizeof(Node *));
    re->name = (char **)xrealloc(re->name, (size_t)re->capgroup * sizeof(char *));
    memset(re->group + old, 0, (size_t)(re->capgroup - old) * sizeof(Node *));
    memset(re->name + old, 0, (size_t)(re->capgroup - old) * sizeof(char *));
  }
  return g;
}


static int group_by_name (P *p, const char *nm, size_t n) {
  int g;
  for (g = 1; g <= p->re->ngroup; g++)
    if (p->re->name[g] && strlen(p->re->name[g]) == n && memcmp(p->re->name[g], nm, n) == 0) return g;
  return -1;
}


/* \k<name> \g<name> \k<1>: the group's number; -1 not known */
static int ref_group (P *p) {
  char close;
  size_t a;
  int g;
  if (p->i >= p->n || (p->s[p->i] != '<' && p->s[p->i] != '\'')) return -1;
  close = p->s[p->i] == '<' ? '>' : '\'';
  a = ++p->i;
  while (p->i < p->n && p->s[p->i] != close) p->i++;
  if (p->i >= p->n) return -1;
  p->i++;
  if (p->s[a] >= '0' && p->s[a] <= '9') return atoi(p->s + a);
  if (p->s[a] == '-' || p->s[a] == '+') return -1;	/* relative: not done */
  g = group_by_name(p, p->s + a, p->i - 1 - a);
  return g;
}


/* a backslash's atom */
static Node *parse_escape (P *p, int flags) {
  int e;
  long v;
  Node *n;
  if (p->i >= p->n) {
    p->err = "\\ at the end";
    return NULL;
  }
  e = (unsigned char)p->s[p->i++];
  if (strchr("dDwWsShH", e)) return class_node(p, shorthand(p->re, e));
  switch (e) {
    case 'b': return node(p->re, N_WORDB);
    case 'B': return node(p->re, N_NWORDB);
    case 'A': p->re->uses_g = 1; return node(p->re, N_BOS);
    case 'z': return node(p->re, N_EOS);
    case 'Z': return node(p->re, N_EOSNL);
    case 'G': p->re->uses_g = 1; return node(p->re, N_G);
    case 'K': return node(p->re, N_EMPTY);
    case 'p': case 'P': {
      Class *c = new_class(p->re);
      prop_class(p, c, e == 'P');
      return class_node(p, c);
    }
    case 'R': {	/* a line break */
      Class *c = new_class(p->re);
      add_range(p->re, c, 10, 13, 0);
      add_range(p->re, c, 0x85, 0x85, 0);
      add_range(p->re, c, 0x2028, 0x2029, 0);
      return class_node(p, c);
    }
    case 'X': {
      n = node(p->re, N_ANY);
      n->dotall = 1;
      return n;
    }
    case 'k': case 'g': {
      int g = ref_group(p);
      if (g < 0) {
        p->err = "a reference to a group that is not there";
        return NULL;
      }
      n = node(p->re, e == 'k' ? N_BACKREF : N_CALL);
      n->cap = g;
      n->icase = (flags & F_ICASE) != 0;
      return n;
    }
    case 'Q': {	/* \Q...\E: as it is */
      size_t a = p->i;
      const char *q = strstr(p->s + a, "\\E");
      size_t b = q && (size_t)(q - p->s) <= p->n ? (size_t)(q - p->s) : p->n;
      p->i = b < p->n ? b + 2 : p->n;
      if (b == a) return node(p->re, N_EMPTY);
      return str_node(p, (const unsigned char *)p->s + a, (int)(b - a), flags);
    }
  }
  if (e >= '1' && e <= '9') {	/* \1: a back reference */
    int g = e - '0';
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9' && g * 10 + (p->s[p->i] - '0') <= p->re->ngroup)
      g = g * 10 + (p->s[p->i++] - '0');
    n = node(p->re, N_BACKREF);
    n->cap = g;
    n->icase = (flags & F_ICASE) != 0;
    return n;
  }
  if ((v = escape_char(p, e)) >= 0) {
    unsigned char b[4];
    return str_node(p, b, enc((uint32_t)v, b), flags);
  }
  {	/* \. \/ \-: the character itself */
    unsigned char b[4];
    uint32_t cp;
    p->i--;
    cp = next_cp(p);
    return str_node(p, b, enc(cp, b), flags);
  }
}


/* (?imx-imx: the flags, into *flags; the index after them */
static void parse_flags (P *p, int *flags) {
  int on = 1;
  while (p->i < p->n && strchr("imx-", p->s[p->i])) {
    int f = 0;
    switch (p->s[p->i++]) {
      case '-': on = 0; continue;
      case 'i': f = F_ICASE; break;
      case 'm': f = F_DOTALL; break;	/* Ruby's m: . takes \n */
      case 'x': f = F_EXT; break;
    }
    if (on) *flags |= f;
    else *flags &= ~f;
  }
}


/* after '(': a group of some kind; NULL with no error: nothing (flags, a comment) */
static Node *parse_group (P *p, int *flags) {
  Node *n;
  int inner = *flags, cap = -1, type = N_GROUP, neg = 0, behind = 0;
  if (p->i < p->n && p->s[p->i] == '?') {
    char c;
    p->i++;
    c = p->i < p->n ? p->s[p->i] : '\0';
    if (c == '#') {	/* (?# a comment) */
      while (p->i < p->n && p->s[p->i] != ')') p->i++;
      if (p->i < p->n) p->i++;
      return NULL;
    }
    if (c == ':') p->i++;
    else if (c == '=' || c == '!') {
      type = N_LOOK;
      neg = c == '!';
      p->i++;
    }
    else if (c == '<' && p->i + 1 < p->n && (p->s[p->i + 1] == '=' || p->s[p->i + 1] == '!')) {
      type = N_LOOK;
      behind = 1;
      neg = p->s[p->i + 1] == '!';
      p->i += 2;
    }
    else if (c == '>') {
      type = N_ATOMIC;
      p->i++;
    }
    else if (c == '~') {	/* (?~abc): anything, near enough */
      p->i++;
      type = N_GROUP;
      cap = -2;
    }
    else if (c == '<' || c == '\'' || (c == 'P' && p->i + 1 < p->n && p->s[p->i + 1] == '<')) {	/* (?<name> */
      char close = c == '\'' ? '\'' : '>';
      size_t a;
      if (c == 'P') p->i++;
      a = ++p->i;
      while (p->i < p->n && p->s[p->i] != close) p->i++;
      if (p->i >= p->n) {
        p->err = "a bad group name";
        return NULL;
      }
      cap = new_group(p->re);
      p->re->name[cap] = (char *)xmalloc(p->i - a + 1);
      memcpy(p->re->name[cap], p->s + a, p->i - a);
      p->re->name[cap][p->i - a] = '\0';
      p->i++;
    }
    else if (c == '(') {
      p->err = "conditional groups are not done";
      return NULL;
    }
    else {	/* (?i) or (?i:...) */
      parse_flags(p, &inner);
      if (p->i < p->n && p->s[p->i] == ')') {
        p->i++;
        *flags = inner;
        return NULL;
      }
      if (p->i < p->n && p->s[p->i] == ':') p->i++;
      else {
        p->err = "a bad (? group";
        return NULL;
      }
    }
  }
  else cap = new_group(p->re);
  if (cap == -2) {	/* the absent operator: .*? up to its ')' */
    int depth = 1;
    while (p->i < p->n && depth > 0) {
      if (p->s[p->i] == '\\') p->i++;
      else if (p->s[p->i] == '(') depth++;
      else if (p->s[p->i] == ')') depth--;
      p->i++;
    }
    n = node(p->re, N_REP);
    n->kid = node(p->re, N_ANY);
    n->kid->dotall = 1;
    n->min = 0;
    n->max = -1;
    return n;
  }
  n = node(p->re, type);
  n->cap = type == N_GROUP ? cap : -1;
  n->neg = (unsigned char)neg;
  n->behind = (unsigned char)behind;
  if (cap > 0) p->re->group[cap] = n;
  if (++p->depth > 200) {
    p->err = "too deep";
    return NULL;
  }
  n->kid = parse_alt(p, inner);
  p->depth--;
  if (p->err) return NULL;
  if (p->i >= p->n || p->s[p->i] != ')') {
    p->err = "a group without its )";
    return NULL;
  }
  p->i++;
  return n;
}


static void skip_ext (P *p, int flags) {
  if (!(flags & F_EXT)) return;
  for (;;) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r' ||
                           p->s[p->i] == '\f' || p->s[p->i] == '\v'))
      p->i++;
    if (p->i < p->n && p->s[p->i] == '#') {
      while (p->i < p->n && p->s[p->i] != '\n') p->i++;
      continue;
    }
    return;
  }
}


/* a quantifier after an atom: 1 read */
static int parse_quant (P *p, int *min, int *max) {
  char c;
  if (p->i >= p->n) return 0;
  c = p->s[p->i];
  if (c == '*') *min = 0, *max = -1;
  else if (c == '+') *min = 1, *max = -1;
  else if (c == '?') *min = 0, *max = 1;
  else if (c == '{') {	/* {n} {n,} {n,m} {,m}; else a plain { */
    size_t i = p->i + 1;
    int a = -1, b = -1, comma = 0;
    if (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') {
      a = 0;
      while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') a = a * 10 + (p->s[i++] - '0');
    }
    if (i < p->n && p->s[i] == ',') {
      comma = 1;
      i++;
      if (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') {
        b = 0;
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') b = b * 10 + (p->s[i++] - '0');
      }
    }
    if (i >= p->n || p->s[i] != '}' || (a < 0 && b < 0)) return 0;
    *min = a < 0 ? 0 : a;
    *max = comma ? b : a;
    p->i = i;
  }
  else return 0;
  p->i++;
  return 1;
}


/* a sequence, up to | or ) or the end */
static Node *parse_seq (P *p, int *flags) {
  Node *head = NULL, *tail = NULL;
  for (;;) {
    Node *a = NULL;
    int c, min, max;
    skip_ext(p, *flags);
    if (p->i >= p->n) break;
    c = (unsigned char)p->s[p->i];
    if (c == '|' || c == ')') break;
    p->i++;
    switch (c) {
      case '(': a = parse_group(p, flags); break;
      case '[': a = class_node(p, parse_class(p, *flags)); break;
      case '.': a = node(p->re, N_ANY); a->dotall = (*flags & F_DOTALL) != 0; break;
      case '^': a = node(p->re, N_BOL); break;
      case '$': a = node(p->re, N_EOL); break;
      case '\\': a = parse_escape(p, *flags); break;
      default: {
        unsigned char b[4];
        uint32_t cp;
        p->i--;
        cp = next_cp(p);
        a = str_node(p, b, enc(cp, b), *flags);
      }
    }
    if (p->err) return NULL;
    if (a == NULL) continue;
    skip_ext(p, *flags);
    while (parse_quant(p, &min, &max)) {	/* a* a+? a{2}+ ... */
      Node *r = node(p->re, N_REP);
      r->kid = a;
      r->min = min;
      r->max = max;
      r->greedy = 1;
      if (p->i < p->n && p->s[p->i] == '?') {
        r->greedy = 0;
        p->i++;
      }
      else if (p->i < p->n && p->s[p->i] == '+') {
        r->possessive = 1;
        p->i++;
      }
      a = r;
      skip_ext(p, *flags);
    }
    if (tail && tail->type == N_STR && a->type == N_STR && tail->icase == a->icase) {	/* "ab": one string */
      unsigned char *s = (unsigned char *)alloc(p->re, (size_t)(tail->len + a->len) + 1);
      memcpy(s, tail->str, (size_t)tail->len);
      memcpy(s + tail->len, a->str, (size_t)a->len);
      tail->str = s;
      tail->len += a->len;
      continue;
    }
    if (tail) tail->next = a;
    else head = a;
    tail = a;
  }
  if (head == NULL) head = node(p->re, N_EMPTY);
  return head;
}


static Node *parse_alt (P *p, int flags) {
  Node *first = parse_seq(p, &flags), **alts, *a;
  int n = 1, cap = 16;
  if (p->err || p->i >= p->n || p->s[p->i] != '|') return p->err ? NULL : first;
  alts = (Node **)xmalloc((size_t)cap * sizeof(Node *));	/* HTML's entities: thousands of them */
  alts[0] = first;
  while (p->err == NULL && p->i < p->n && p->s[p->i] == '|') {
    p->i++;
    if (n == cap) alts = (Node **)xrealloc(alts, (size_t)(cap *= 2) * sizeof(Node *));
    alts[n++] = parse_seq(p, &flags);
  }
  if (p->err) {
    free(alts);
    return NULL;
  }
  a = node(p->re, N_ALT);
  a->alt = (Node **)alloc(p->re, (size_t)n * sizeof(Node *));
  memcpy(a->alt, alts, (size_t)n * sizeof(Node *));
  a->nalt = n;
  free(alts);
  return a;
}

/* }================================================================== */


/*
** {==================================================================
** What a pattern can start with, and lookbehind lengths
** ===================================================================
*/

static int nullable (const Node *n);


static int seq_nullable (const Node *n) {
  for (; n; n = n->next)
    if (!nullable(n)) return 0;
  return 1;
}


static int nullable (const Node *n) {
  int i;
  switch (n->type) {
    case N_STR: return n->len == 0;
    case N_ANY: case N_CLASS: return 0;
    case N_GROUP: case N_ATOMIC: return seq_nullable(n->kid);
    case N_ALT:
      for (i = 0; i < n->nalt; i++)
        if (seq_nullable(n->alt[i])) return 1;
      return 0;
    case N_REP: return n->min == 0 || seq_nullable(n->kid);
    case N_CALL: return 1;
  }
  return 1;	/* anchors, lookarounds, back references */
}


static void set_all (unsigned char *f) {
  memset(f, 0xFF, 32);
}


static void first_node (const Node *n, unsigned char *f);


/* the first bytes of a sequence; 1: it can be empty (what follows counts too) */
static int first_seq (const Node *n, unsigned char *f) {
  for (; n; n = n->next) {
    first_node(n, f);
    if (!nullable(n)) return 0;
  }
  return 1;
}


static void first_node (const Node *n, unsigned char *f) {
  int i, b;
  switch (n->type) {
    case N_STR:
      if (n->len == 0) return;
      b = n->str[0];
      f[b >> 3] |= (unsigned char)(1 << (b & 7));
      if (n->icase) {
        b = lower(n->str[0]);
        f[b >> 3] |= (unsigned char)(1 << (b & 7));
        b = upper(n->str[0]);
        f[b >> 3] |= (unsigned char)(1 << (b & 7));
      }
      return;
    case N_ANY: set_all(f); return;
    case N_CLASS:
      for (b = 0; b < 128; b++)
        if (cls_has(n->cls, (uint32_t)b)) f[b >> 3] |= (unsigned char)(1 << (b & 7));
      for (b = 0xC0; b < 256; b++) f[b >> 3] |= (unsigned char)(1 << (b & 7));	/* non-ASCII: maybe */
      return;
    case N_GROUP: case N_ATOMIC: first_seq(n->kid, f); return;
    case N_ALT:
      for (i = 0; i < n->nalt; i++) first_seq(n->alt[i], f);
      return;
    case N_REP: first_seq(n->kid, f); return;
    case N_BACKREF: case N_CALL: set_all(f); return;
  }
}


/* a sequence's length in characters; -1: it varies */
static int char_len (const Node *n) {
  int len = 0, k, i, a;
  for (; n; n = n->next) {
    switch (n->type) {
      case N_STR:
        for (k = 0; k < n->len; k++)
          if ((n->str[k] & 0xC0) != 0x80) len++;
        break;
      case N_ANY: case N_CLASS: len++; break;
      case N_GROUP: case N_ATOMIC:
        if ((k = char_len(n->kid)) < 0) return -1;
        len += k;
        break;
      case N_ALT:
        a = char_len(n->alt[0]);
        for (i = 1; i < n->nalt; i++)
          if (char_len(n->alt[i]) != a) return -1;
        if (a < 0) return -1;
        len += a;
        break;
      case N_REP:
        if (n->min != n->max || (k = char_len(n->kid)) < 0) return -1;
        len += k * n->min;
        break;
      case N_BACKREF: case N_CALL: return -1;
    }
  }
  return len;
}


/* each lookbehind: the lengths it can have */
static void mark_nodes (Node *n) {
  int i;
  for (; n; n = n->next) {
    if (n->type == N_LOOK && n->behind) {
      int k = char_len(n->kid);
      n->nlens = 0;
      if (k >= 0) n->lens[n->nlens++] = k;
      else if (n->kid->type == N_ALT && n->kid->next == NULL) {
        for (i = 0; i < n->kid->nalt; i++) {
          int l = char_len(n->kid->alt[i]), j, seen = 0;
          if (l < 0 || n->nlens == 8) {
            n->nlens = 0;
            break;
          }
          for (j = 0; j < n->nlens; j++)
            if (n->lens[j] == l) seen = 1;
          if (!seen) n->lens[n->nlens++] = l;
        }
      }
    }
    if (n->kid) mark_nodes(n->kid);
    for (i = 0; i < n->nalt; i++) mark_nodes(n->alt[i]);
  }
}


Onig *onig_compile (const char *pat, const char **err) {
  P p;
  Onig *re = (Onig *)calloc(1, sizeof(Onig));
  if (re == NULL) return NULL;
  memset(&p, 0, sizeof(p));
  p.s = pat;
  p.n = strlen(pat);
  p.re = re;
  re->root = parse_alt(&p, 0);
  if (p.err == NULL && p.i < p.n) p.err = "a ) without its (";
  if (p.err) {
    if (err) *err = p.err;
    onig_free(re);
    return NULL;
  }
  mark_nodes(re->root);
  if (!seq_nullable(re->root)) {
    memset(re->first, 0, sizeof(re->first));
    first_seq(re->root, re->first);
    re->has_first = 1;
  }
  if (re->root->type == N_G) re->anch = ANCH_G;
  else if (re->root->type == N_BOS) re->anch = ANCH_A;
  else if (re->root->type == N_BOL) re->anch = ANCH_BOL;
  return re;
}

/* }================================================================== */


/*
** {==================================================================
** Matching
** ===================================================================
*/

enum { K_NEXT, K_GROUP, K_REP, K_STOP, K_AT, K_CALL };

typedef struct Cont {
  int kind;
  const Node *node;
  const struct Cont *up;
  size_t pos;	/* K_GROUP, K_REP: where it began; K_AT: where it must end */
  int count;	/* K_REP: the repetitions before this one */
} Cont;

typedef struct M {
  const Onig *re;
  const unsigned char *s;
  size_t n;
  size_t *cap;
  int ncap;	/* entries in cap: 2 * (groups + 1) */
  size_t g;	/* where \G matches */
  int first;	/* \A may match */
  size_t end;
  long steps;
  int depth, abort;
} M;


static int run (M *m, const Node *nd, size_t i, const Cont *k);


static int word_at (const M *m, size_t i) {
  size_t len;
  if (i >= m->n) return 0;
  return is_word_cp(utf8_decode((const char *)m->s + i, m->n - i, &len));
}


static int word_before (const M *m, size_t i) {
  size_t j = i, len;
  if (i == 0) return 0;
  j--;
  while (j > 0 && (m->s[j] & 0xC0) == 0x80) j--;
  return is_word_cp(utf8_decode((const char *)m->s + j, m->n - j, &len));
}


static size_t back_char (const M *m, size_t j) {
  if (j == 0) return 0;
  j--;
  while (j > 0 && (m->s[j] & 0xC0) == 0x80) j--;
  return j;
}


/* a node that takes one character: how many bytes it took at i, 0 none */
static size_t one (const M *m, const Node *nd, size_t i) {
  size_t len;
  uint32_t cp;
  if (i >= m->n) return 0;
  switch (nd->type) {
    case N_STR:
      if (i + (size_t)nd->len > m->n) return 0;
      if (nd->icase) {
        int k;
        for (k = 0; k < nd->len; k++)
          if (lower(m->s[i + (size_t)k]) != lower(nd->str[k])) return 0;
        return (size_t)nd->len;
      }
      return memcmp(m->s + i, nd->str, (size_t)nd->len) == 0 ? (size_t)nd->len : 0;
    case N_ANY:
      if (m->s[i] == '\n' && !nd->dotall) return 0;
      utf8_decode((const char *)m->s + i, m->n - i, &len);
      return len ? len : 1;
    case N_CLASS:
      if (m->s[i] < 128) return cls_has(nd->cls, m->s[i]) ? 1 : 0;
      cp = utf8_decode((const char *)m->s + i, m->n - i, &len);
      return cls_has(nd->cls, cp) ? (len ? len : 1) : 0;
  }
  return 0;
}


static int single (const Node *nd) {
  if (nd->next) return 0;
  if (nd->type == N_ANY || nd->type == N_CLASS) return 1;
  if (nd->type == N_STR && nd->len > 0) {	/* one character */
    int k, chars = 0;
    for (k = 0; k < nd->len; k++)
      if ((nd->str[k] & 0xC0) != 0x80) chars++;
    return chars == 1;
  }
  return 0;
}


static int cont (M *m, size_t i, const Cont *k);


/* a copy of the captures, to put back when a path fails */
static size_t *caps_save (const M *m, size_t *small) {
  size_t *v = m->ncap <= 64 ? small : (size_t *)xmalloc((size_t)m->ncap * sizeof(size_t));
  memcpy(v, m->cap, (size_t)m->ncap * sizeof(size_t));
  return v;
}


static void caps_done (size_t *v, const size_t *small) {
  if (v != small) free(v);
}
static int rep (M *m, const Node *nd, int count, size_t i, const Cont *k, int inner);


/* a sub-match (lookaround, atomic): nd's sequence from i; its end in m->end */
static int sub (M *m, const Node *nd, size_t i) {
  Cont stop;
  stop.kind = K_STOP;
  stop.node = NULL;
  stop.up = NULL;
  stop.pos = 0;
  stop.count = 0;
  return run(m, nd, i, &stop);
}


/* lookbehind: nd's kid ends right at i */
static int behind (M *m, const Node *nd, size_t i) {
  Cont at;
  int li, c;
  at.kind = K_AT;
  at.node = NULL;
  at.up = NULL;
  at.pos = i;
  at.count = 0;
  if (nd->nlens > 0) {
    for (li = 0; li < nd->nlens; li++) {
      size_t j = i;
      for (c = 0; c < nd->lens[li]; c++) {
        if (j == 0) break;
        j = back_char(m, j);
      }
      if (c < nd->lens[li]) continue;
      if (run(m, nd->kid, j, &at)) return 1;
      if (m->abort) return 0;
    }
    return 0;
  }
  {	/* it varies: every start not too far back */
    size_t j = i;
    for (c = 0; c < 128; c++) {
      if (run(m, nd->kid, j, &at)) return 1;
      if (m->abort || j == 0) return 0;
      j = back_char(m, j);
    }
  }
  return 0;
}


static int run (M *m, const Node *nd, size_t i, const Cont *k) {
  int r = 0;
  if (++m->steps > MAX_STEPS || m->depth > MAX_DEPTH) {
    m->abort = 1;
    return 0;
  }
  m->depth++;
  for (;;) {
    size_t w;
    if (nd == NULL) {
      r = cont(m, i, k);
      break;
    }
    switch (nd->type) {
      case N_STR: case N_ANY: case N_CLASS:
        if ((w = one(m, nd, i)) == 0) goto done;
        i += w;
        nd = nd->next;
        continue;
      case N_EMPTY: nd = nd->next; continue;
      case N_BOL: if (i != 0 && (m->s[i - 1] != '\n' || i == m->n)) goto done; nd = nd->next; continue;	/* not after the last \n */
      case N_EOL: if (i != m->n && m->s[i] != '\n') goto done; nd = nd->next; continue;
      case N_BOS: if (i != 0 || !m->first) goto done; nd = nd->next; continue;
      case N_EOS: if (i != m->n) goto done; nd = nd->next; continue;
      case N_EOSNL: if (!(i == m->n || (i + 1 == m->n && m->s[i] == '\n'))) goto done; nd = nd->next; continue;
      case N_G: if (i != m->g) goto done; nd = nd->next; continue;
      case N_WORDB: if (word_before(m, i) == word_at(m, i)) goto done; nd = nd->next; continue;
      case N_NWORDB: if (word_before(m, i) != word_at(m, i)) goto done; nd = nd->next; continue;
      case N_GROUP: {
        Cont c;
        c.kind = nd->cap > 0 ? K_GROUP : K_NEXT;
        c.node = nd;
        c.up = k;
        c.pos = i;
        c.count = 0;
        r = run(m, nd->kid, i, &c);
        goto done;
      }
      case N_ALT: {
        Cont c;
        int j;
        c.kind = K_NEXT;
        c.node = nd;
        c.up = k;
        c.pos = i;
        c.count = 0;
        for (j = 0; j < nd->nalt; j++) {
          if (run(m, nd->alt[j], i, &c)) {
            r = 1;
            break;
          }
          if (m->abort) break;
        }
        goto done;
      }
      case N_REP: r = rep(m, nd, 0, i, k, 0); goto done;
      case N_LOOK: case N_ATOMIC: {
        size_t small[64], *save = caps_save(m, small);
        int ok;
        if (nd->type == N_ATOMIC) ok = sub(m, nd->kid, i);
        else ok = nd->behind ? behind(m, nd, i) : sub(m, nd->kid, i);
        if (!m->abort) {
          if (nd->type == N_LOOK && nd->neg) ok = !ok;
          if (!ok || (nd->type == N_LOOK && nd->neg)) memcpy(m->cap, save, (size_t)m->ncap * sizeof(size_t));
          if (ok) {
            r = run(m, nd->next, nd->type == N_ATOMIC ? m->end : i, k);
            if (!r) memcpy(m->cap, save, (size_t)m->ncap * sizeof(size_t));
          }
        }
        caps_done(save, small);
        goto done;
      }
      case N_BACKREF: {
        size_t a, b, len, q;
        if (nd->cap <= 0 || 2 * nd->cap + 1 >= m->ncap || (a = m->cap[2 * nd->cap]) == NONE ||
            (b = m->cap[2 * nd->cap + 1]) == NONE)
          goto done;
        len = b - a;
        if (i + len > m->n) goto done;
        for (q = 0; q < len; q++)
          if (nd->icase ? lower(m->s[a + q]) != lower(m->s[i + q]) : m->s[a + q] != m->s[i + q]) goto done;
        i += len;
        nd = nd->next;
        continue;
      }
      case N_CALL: {
        Cont c;
        const Node *g = (nd->cap > 0 && nd->cap <= m->re->ngroup) ? m->re->group[nd->cap] : NULL;
        if (g == NULL) goto done;
        c.kind = K_CALL;
        c.node = nd;
        c.up = k;
        c.pos = i;
        c.count = 0;
        r = run(m, g->kid, i, &c);
        goto done;
      }
      default: goto done;
    }
  }
done:
  m->depth--;
  return r;
}


/* after a repetition's body ended at i */
static int rep_after (M *m, const Cont *k, size_t i) {
  const Node *nd = k->node;
  if (i == k->pos) return run(m, nd->possessive ? NULL : nd->next, i, k->up);	/* an empty one: no more of them */
  return rep(m, nd, k->count + 1, i, k->up, 1);
}


static int rep (M *m, const Node *nd, int count, size_t i, const Cont *k, int inner) {
  const Node *kid = nd->kid, *nx;
  Cont c;
  if (count == 0 && single(kid)) {	/* a* [^"]+ .*?: without recursion */
    size_t j = i, w;
    int n = 0;
    if (nd->greedy || nd->possessive) {
      while ((nd->max < 0 || n < nd->max) && (w = one(m, kid, j)) > 0) {
        j += w;
        n++;
      }
      if (n < nd->min) return 0;
      if (nd->possessive) return run(m, nd->next, j, k);
      for (;;) {
        if (run(m, nd->next, j, k)) return 1;
        if (m->abort || n == nd->min) return 0;
        j = back_char(m, j);
        n--;
      }
    }
    while (n < nd->min) {
      if ((w = one(m, kid, j)) == 0) return 0;
      j += w;
      n++;
    }
    for (;;) {
      if (run(m, nd->next, j, k)) return 1;
      if (m->abort || (nd->max >= 0 && n >= nd->max) || (w = one(m, kid, j)) == 0) return 0;
      j += w;
      n++;
    }
  }
  if (nd->possessive && !inner) {	/* as an atomic group of the greedy one */
    size_t small[64], *save = caps_save(m, small);
    Cont stop;
    int r = 0;
    stop.kind = K_STOP;
    stop.node = NULL;
    stop.up = NULL;
    stop.pos = 0;
    stop.count = 0;
    if (rep(m, nd, count, i, &stop, 1)) r = run(m, nd->next, m->end, k);
    if (!r) memcpy(m->cap, save, (size_t)m->ncap * sizeof(size_t));
    caps_done(save, small);
    return r;
  }
  nx = nd->possessive ? NULL : nd->next;	/* possessive: inside its atomic run; the rest comes after it */
  c.kind = K_REP;
  c.node = nd;
  c.up = k;
  c.pos = i;
  c.count = count;
  if (nd->greedy || nd->possessive) {
    if (nd->max < 0 || count < nd->max) {
      if (run(m, kid, i, &c)) return 1;
      if (m->abort) return 0;
    }
    return count >= nd->min && run(m, nx, i, k);
  }
  if (count >= nd->min) {
    if (run(m, nx, i, k)) return 1;
    if (m->abort) return 0;
  }
  if (nd->max >= 0 && count >= nd->max) return 0;
  return run(m, kid, i, &c);
}


static int cont (M *m, size_t i, const Cont *k) {
  if (k == NULL) {
    m->end = i;
    return 1;
  }
  switch (k->kind) {
    case K_NEXT: case K_CALL: return run(m, k->node->next, i, k->up);
    case K_GROUP: {
      int c = k->node->cap;
      size_t os = m->cap[2 * c], oe = m->cap[2 * c + 1];
      m->cap[2 * c] = k->pos;
      m->cap[2 * c + 1] = i;
      if (run(m, k->node->next, i, k->up)) return 1;
      m->cap[2 * c] = os;
      m->cap[2 * c + 1] = oe;
      return 0;
    }
    case K_REP: return rep_after(m, k, i);
    case K_STOP:
      m->end = i;
      return 1;
    case K_AT:
      if (i != k->pos) return 0;
      m->end = i;
      return 1;
  }
  return 0;
}


/*
** The first match at from or later in s[0..n): its groups in caps
** (2 * (groups + 1) entries, (size_t)-1 for a group that took no part).
** \G matches only at g; \A only when first is set. 0: none.
*/
int onig_search (const Onig *re, const char *s, size_t n, size_t from, size_t g, int first, size_t *caps) {
  M m;
  size_t i = from;
  int k;
  if (from > n) return 0;
  memset(&m, 0, sizeof(m));
  m.re = re;
  m.s = (const unsigned char *)s;
  m.n = n;
  m.cap = caps;
  m.ncap = 2 * (re->ngroup + 1);
  m.g = g;
  m.first = first;
  if (re->anch == ANCH_G) {
    if (g == NONE || g < from || g > n) return 0;
    i = g;
  }
  else if (re->anch == ANCH_A) {
    if (from != 0 || !first) return 0;
  }
  for (;;) {
    if (re->anch == ANCH_BOL && i != 0 && (m.s[i - 1] != '\n' || i == n)) goto next;
    if (re->has_first) {
      while (i < n && !((re->first[m.s[i] >> 3] >> (m.s[i] & 7)) & 1)) i++;
      if (i >= n) return 0;
      if (re->anch == ANCH_BOL && i != 0 && m.s[i - 1] != '\n') goto next;
    }
    for (k = 0; k < m.ncap; k++) caps[k] = NONE;
    m.depth = 0;
    if (run(&m, re->root, i, NULL)) {
      caps[0] = i;
      caps[1] = m.end;
      return 1;
    }
    if (m.abort) return 0;
  next:
    if (re->anch == ANCH_G || re->anch == ANCH_A || i >= n) return 0;
    i++;
    while (i < n && (m.s[i] & 0xC0) == 0x80) i++;
  }
}

/* }================================================================== */
