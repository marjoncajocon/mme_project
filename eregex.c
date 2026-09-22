/*
** eregex.c - regular expressions for Find, Replace and Search (".*")
**
** A small backtracking matcher, the subset VS Code's users type most:
** . [abc] [^a-z] * + ? {n,m} (lazy with a '?' after), | (...) (?:...)
** ^ $ \d \w \s \b and their capitals, \n \t \. ... A pattern is parsed to
** a tree of nodes, each with the node after it; matching walks them with
** a stack of what is left to do (Cont), trying again on failure. Case can
** be ignored (ASCII); '.' and classes take a whole UTF-8 character.
*/

#include "mme.h"

#include <stdlib.h>
#include <string.h>


enum { N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_GROUP, N_ALT, N_REP };

#define CL_D	1	/* \d in a class */
#define CL_W	2
#define CL_S	4
#define CL_ND	8	/* \D */
#define CL_NW	16
#define CL_NS	32

typedef struct Node {
  int t;
  struct Node *next;	/* what comes after it */
  struct Node *all;	/* every node, to free them */
  int c;	/* N_CHAR: the byte */
  int idx;	/* N_GROUP: its number, -1 not captured */
  struct Node *kid;	/* N_GROUP, N_REP: what is inside */
  struct Node **alt;	/* N_ALT: the choices */
  int nalt;
  int min, max, lazy;	/* N_REP; max -1: no end */
  uint32_t *rng;	/* N_CLASS: lo, hi pairs */
  int nrng, neg, flags;
} Node;

struct Regex {
  Node *first;
  Node *all;
  int icase;
  int ngroup;
};

typedef struct Parser {
  const char *s;
  Regex *re;
  const char *err;
} Parser;


static Node *node (Parser *p, int t) {
  Node *n = (Node *)xmalloc(sizeof(Node));
  memset(n, 0, sizeof(*n));
  n->t = t;
  n->idx = -1;
  n->all = p->re->all;
  p->re->all = n;
  return n;
}


static Node *parse_alt (Parser *p);


static int hexval (int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}


/* the character after a backslash (not a class like \d): its value */
static uint32_t escaped (Parser *p) {
  int c = (unsigned char)*p->s++;
  switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x':
      if (hexval(p->s[0]) >= 0 && hexval(p->s[1]) >= 0) {
        uint32_t v = (uint32_t)(hexval(p->s[0]) * 16 + hexval(p->s[1]));
        p->s += 2;
        return v;
      }
      return 'x';
  }
  if (c >= 0x80) {	/* a UTF-8 character escaped */
    size_t len;
    uint32_t cp = utf8_decode(p->s - 1, strlen(p->s - 1), &len);
    p->s += len - 1;
    return cp;
  }
  return (uint32_t)c;
}


static void add_range (Node *n, uint32_t lo, uint32_t hi) {
  n->rng = (uint32_t *)xrealloc(n->rng, (size_t)(n->nrng + 1) * 2 * sizeof(uint32_t));
  n->rng[n->nrng * 2] = lo;
  n->rng[n->nrng * 2 + 1] = hi;
  n->nrng++;
}


static int class_flag (int c) {
  switch (c) {
    case 'd': return CL_D;
    case 'w': return CL_W;
    case 's': return CL_S;
    case 'D': return CL_ND;
    case 'W': return CL_NW;
    case 'S': return CL_NS;
  }
  return 0;
}


/* [...]: p->s is after the '[' */
static Node *parse_class (Parser *p) {
  Node *n = node(p, N_CLASS);
  int first = 1;
  if (*p->s == '^') {
    n->neg = 1;
    p->s++;
  }
  while (*p->s && (*p->s != ']' || first)) {
    uint32_t lo, hi;
    size_t len;
    first = 0;
    if (*p->s == '\\' && class_flag((unsigned char)p->s[1])) {
      n->flags |= class_flag((unsigned char)p->s[1]);
      p->s += 2;
      continue;
    }
    if (*p->s == '\\') {
      p->s++;
      lo = escaped(p);
    }
    else {
      lo = utf8_decode(p->s, strlen(p->s), &len);
      p->s += len;
    }
    hi = lo;
    if (p->s[0] == '-' && p->s[1] && p->s[1] != ']') {	/* a-z */
      p->s++;
      if (*p->s == '\\') {
        p->s++;
        hi = escaped(p);
      }
      else {
        hi = utf8_decode(p->s, strlen(p->s), &len);
        p->s += len;
      }
      if (hi < lo) {
        p->err = "Range out of order in character class";
        return n;
      }
    }
    add_range(n, lo, hi);
  }
  if (*p->s != ']') p->err = "Unterminated character class";
  else p->s++;
  return n;
}


/* one piece: a character, a class, a group, an anchor */
static Node *parse_atom (Parser *p) {
  int c = (unsigned char)*p->s;
  Node *n;
  if (c == '(') {
    p->s++;
    n = node(p, N_GROUP);
    if (p->s[0] == '?' && p->s[1] == ':') p->s += 2;	/* (?:...) not captured */
    else if (p->re->ngroup < 9) n->idx = ++p->re->ngroup;
    n->kid = parse_alt(p);
    if (*p->s != ')') {
      if (!p->err) p->err = "Unterminated group";
    }
    else p->s++;
    return n;
  }
  if (c == '[') {
    p->s++;
    return parse_class(p);
  }
  if (c == '.') {
    p->s++;
    return node(p, N_ANY);
  }
  if (c == '^') {
    p->s++;
    return node(p, N_BOL);
  }
  if (c == '$') {
    p->s++;
    return node(p, N_EOL);
  }
  if (c == '\\') {
    int e = (unsigned char)p->s[1];
    if (e == '\0') {
      p->err = "\\ at end of pattern";
      p->s++;
      return node(p, N_CHAR);
    }
    if (e == 'b' || e == 'B') {
      p->s += 2;
      return node(p, e == 'b' ? N_WORDB : N_NWORDB);
    }
    if (class_flag(e)) {
      p->s += 2;
      n = node(p, N_CLASS);
      n->flags = class_flag(e);
      return n;
    }
    p->s++;
    {
      uint32_t cp = escaped(p);
      char u[4];
      int k, len;
      if (cp < 0x80) {
        n = node(p, N_CHAR);
        n->c = (int)cp;
        return n;
      }
      len = utf8_encode(cp, u);	/* the bytes of it, as one piece */
      n = node(p, N_GROUP);
      for (k = len - 1; k >= 0; k--) {
        Node *b = node(p, N_CHAR);
        b->c = (unsigned char)u[k];
        b->next = n->kid;
        n->kid = b;
      }
      return n;
    }
  }
  if (c >= 0x80) {	/* a UTF-8 character: its bytes, as one piece */
    size_t len, k;
    utf8_decode(p->s, strlen(p->s), &len);
    n = node(p, N_GROUP);
    for (k = len; k-- > 0;) {
      Node *b = node(p, N_CHAR);
      b->c = (unsigned char)p->s[k];
      b->next = n->kid;
      n->kid = b;
    }
    p->s += len;
    return n;
  }
  p->s++;
  n = node(p, N_CHAR);
  n->c = c;
  return n;
}


static int number (Parser *p, int *v) {
  int got = 0;
  *v = 0;
  while (*p->s >= '0' && *p->s <= '9') {
    if (*v < 100000) *v = *v * 10 + (*p->s - '0');
    p->s++;
    got = 1;
  }
  return got;
}


/* pieces one after the other, each maybe repeated, to a '|' or ')' */
static Node *parse_seq (Parser *p) {
  Node *first = NULL, *last = NULL;
  while (*p->s && *p->s != '|' && *p->s != ')' && !p->err) {
    Node *a;
    int min = -1, max = -1;
    if (*p->s == '*' || *p->s == '+' || *p->s == '?') {
      p->err = "Nothing to repeat";
      break;
    }
    a = parse_atom(p);
    if (*p->s == '*') min = 0, max = -1, p->s++;
    else if (*p->s == '+') min = 1, max = -1, p->s++;
    else if (*p->s == '?') min = 0, max = 1, p->s++;
    else if (*p->s == '{') {	/* {n}, {n,}, {n,m}: else a '{' as it is */
      const char *save = p->s;
      int lo, hi;
      p->s++;
      if (number(p, &lo)) {
        hi = lo;
        if (*p->s == ',') {
          p->s++;
          if (!number(p, &hi)) hi = -1;
        }
        if (*p->s == '}' && (hi < 0 || hi >= lo)) {
          p->s++;
          min = lo;
          max = hi;
        }
        else p->s = save;
      }
      else p->s = save;
    }
    if (min >= 0) {
      Node *r = node(p, N_REP);
      r->kid = a;
      r->min = min;
      r->max = max;
      if (*p->s == '?') {
        r->lazy = 1;
        p->s++;
      }
      a = r;
    }
    if (last) last->next = a;
    else first = a;
    last = a;
  }
  return first;
}


static Node *parse_alt (Parser *p) {
  Node *seq = parse_seq(p), *n;
  if (*p->s != '|') return seq;
  n = node(p, N_ALT);
  n->alt = (Node **)xmalloc(sizeof(Node *));
  n->alt[n->nalt++] = seq;
  while (*p->s == '|' && !p->err) {
    p->s++;
    n->alt = (Node **)xrealloc(n->alt, (size_t)(n->nalt + 1) * sizeof(Node *));
    n->alt[n->nalt++] = parse_seq(p);
  }
  return n;
}


void re_free (Regex *re) {
  Node *n, *next;
  if (re == NULL) return;
  for (n = re->all; n; n = next) {
    next = n->all;
    free(n->alt);
    free(n->rng);
    free(n);
  }
  free(re);
}


Regex *re_compile (const char *pat, int icase, const char **err) {
  Parser p;
  Regex *re = (Regex *)xmalloc(sizeof(Regex));
  memset(re, 0, sizeof(*re));
  re->icase = icase;
  p.s = pat;
  p.re = re;
  p.err = NULL;
  re->first = parse_alt(&p);
  if (!p.err && *p.s == ')') p.err = "Unmatched )";
  if (p.err) {
    if (err) *err = p.err;
    re_free(re);
    return NULL;
  }
  if (err) *err = NULL;
  return re;
}


/*
** {==================================================================
** Matching
** ===================================================================
*/

enum { K_SEQ, K_GROUP, K_REP };

typedef struct Cont {	/* what is left to do when a part matched */
  int t;
  const Node *n;	/* K_SEQ: go on with n; K_GROUP, K_REP: the node it ends */
  int count;	/* K_REP: the times done */
  size_t start;	/* K_REP: where this time began */
  const struct Cont *up;
} Cont;

typedef struct M {
  const Regex *re;
  const unsigned char *s;
  size_t n, end;
  size_t cap[20];
  long steps;	/* a limit: some patterns take too long */
} M;


static int step (M *m, const Node *n, size_t i, const Cont *k);


static int word_at (const M *m, size_t i) {
  int c;
  if (i >= m->n) return 0;
  c = m->s[i];
  return c == '_' || c >= 0x80 || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
}


static int lowc (int c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}


static int in_class (const M *m, const Node *n, uint32_t cp) {
  int hit = 0, i;
  int w = cp == '_' || (cp >= '0' && cp <= '9') || ((cp | 32) >= 'a' && (cp | 32) <= 'z');
  int d = cp >= '0' && cp <= '9';
  int sp = cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
  if (((n->flags & CL_D) && d) || ((n->flags & CL_W) && w) || ((n->flags & CL_S) && sp) ||
      ((n->flags & CL_ND) && !d) || ((n->flags & CL_NW) && !w) || ((n->flags & CL_NS) && !sp))
    hit = 1;
  for (i = 0; i < n->nrng && !hit; i++) {
    uint32_t lo = n->rng[i * 2], hi = n->rng[i * 2 + 1];
    if (cp >= lo && cp <= hi) hit = 1;
    else if (m->re->icase && cp < 128) {	/* the other case */
      uint32_t o = (cp >= 'a' && cp <= 'z') ? cp - 32 : (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
      if (o >= lo && o <= hi) hit = 1;
    }
  }
  return hit != n->neg;
}


static int try_rep (M *m, const Node *r, size_t i, int count, const Cont *up) {
  Cont c;
  c.t = K_REP;
  c.n = r;
  c.count = count;
  c.start = i;
  c.up = up;
  if (!r->lazy) {
    if ((r->max < 0 || count < r->max) && step(m, r->kid, i, &c)) return 1;
    return count >= r->min && step(m, r->next, i, up);
  }
  if (count >= r->min && step(m, r->next, i, up)) return 1;
  return (r->max < 0 || count < r->max) && step(m, r->kid, i, &c);
}


/* a part matched, up to i: what comes after it */
static int cont (M *m, size_t i, const Cont *k) {
  if (k == NULL) {
    m->end = i;
    return 1;
  }
  switch (k->t) {
    case K_SEQ: return step(m, k->n, i, k->up);
    case K_GROUP: {
      int g = k->n->idx;
      size_t old = g >= 0 ? m->cap[g * 2 + 1] : 0;
      if (g >= 0) m->cap[g * 2 + 1] = i;
      if (step(m, k->n->next, i, k->up)) return 1;
      if (g >= 0) m->cap[g * 2 + 1] = old;
      return 0;
    }
    default:	/* K_REP: one more time, or on; never around an empty one */
      if (i == k->start) return k->count + 1 >= k->n->min && step(m, k->n->next, i, k->up);
      return try_rep(m, k->n, i, k->count + 1, k->up);
  }
}


/* the nodes from n on match at i, then k */
static int step (M *m, const Node *n, size_t i, const Cont *k) {
  for (;;) {
    if (++m->steps > 2000000) return 0;
    if (n == NULL) return cont(m, i, k);
    switch (n->t) {
      case N_CHAR:
        if (i < m->n && (m->s[i] == n->c || (m->re->icase && lowc(m->s[i]) == lowc(n->c)))) {
          i++;
          n = n->next;
          continue;
        }
        return 0;
      case N_ANY:
      case N_CLASS: {
        size_t len;
        uint32_t cp;
        if (i >= m->n) return 0;
        cp = utf8_decode((const char *)m->s + i, m->n - i, &len);
        if (n->t == N_ANY ? (cp == '\n' || cp == '\r') : !in_class(m, n, cp)) return 0;
        i += len;
        n = n->next;
        continue;
      }
      case N_BOL:
        if (i != 0) return 0;
        n = n->next;
        continue;
      case N_EOL:
        if (i != m->n) return 0;
        n = n->next;
        continue;
      case N_WORDB:
      case N_NWORDB: {
        int b = (i > 0 && word_at(m, i - 1)) != word_at(m, i);
        if (b != (n->t == N_WORDB)) return 0;
        n = n->next;
        continue;
      }
      case N_GROUP: {
        Cont c;
        size_t old = n->idx >= 0 ? m->cap[n->idx * 2] : 0;
        c.t = K_GROUP;
        c.n = n;
        c.up = k;
        if (n->idx >= 0) m->cap[n->idx * 2] = i;
        if (step(m, n->kid, i, &c)) return 1;
        if (n->idx >= 0) m->cap[n->idx * 2] = old;
        return 0;
      }
      case N_ALT: {
        Cont c;
        int a;
        c.t = K_SEQ;
        c.n = n->next;
        c.up = k;
        for (a = 0; a < n->nalt; a++)
          if (step(m, n->alt[a], i, &c)) return 1;
        return 0;
      }
      default: return try_rep(m, n, i, 0, k);
    }
  }
}


/* does re match s at byte 'at'? its end in *end, the groups in cap[20] (-1: none) */
int re_at (const Regex *re, const char *s, size_t n, size_t at, size_t *end, size_t *cap) {
  M m;
  size_t i;
  m.re = re;
  m.s = (const unsigned char *)s;
  m.n = n;
  m.end = at;
  m.steps = 0;
  for (i = 0; i < 20; i++) m.cap[i] = (size_t)-1;
  if (!step(&m, re->first, at, NULL)) return 0;
  *end = m.end;
  if (cap) {
    memcpy(cap, m.cap, sizeof(m.cap));
    cap[0] = at;
    cap[1] = m.end;
  }
  return 1;
}


/* the first match from byte 'from' on that is not empty: 1, and where */
int re_find (const Regex *re, const char *s, size_t n, size_t from, size_t *a, size_t *b) {
  size_t i, e;
  for (i = from; i < n; i++) {
    if (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) continue;	/* inside a character */
    if (re_at(re, s, n, i, &e, NULL) && e > i) {
      *a = i;
      *b = e;
      return 1;
    }
  }
  return 0;
}


int re_groups (const Regex *re) {
  return re->ngroup;
}


/*
** The replace text for a match: $0 or $& the match, $1..$9 its groups,
** $$ a '$', \n \t a newline and a tab (as VS Code's regex replace).
*/
char *re_expand (const char *repl, const char *s, const size_t *cap, size_t *len) {
  Buf b;
  buf_init(&b);
  for (; *repl; repl++) {
    if (repl[0] == '$' && ((repl[1] >= '0' && repl[1] <= '9') || repl[1] == '&')) {
      int g = repl[1] == '&' ? 0 : repl[1] - '0';
      if (cap[g * 2] != (size_t)-1 && cap[g * 2 + 1] != (size_t)-1 && cap[g * 2 + 1] >= cap[g * 2])
        buf_putn(&b, s + cap[g * 2], cap[g * 2 + 1] - cap[g * 2]);
      repl++;
    }
    else if (repl[0] == '$' && repl[1] == '$') {
      buf_putc(&b, '$');
      repl++;
    }
    else if (repl[0] == '\\' && (repl[1] == 'n' || repl[1] == 't' || repl[1] == '\\')) {
      buf_putc(&b, repl[1] == 'n' ? '\n' : repl[1] == 't' ? '\t' : '\\');
      repl++;
    }
    else buf_putc(&b, *repl);
  }
  *len = b.len;
  buf_putc(&b, '\0');
  return buf_take(&b);
}

/* }================================================================== */
