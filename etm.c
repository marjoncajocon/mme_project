/*
** etm.c - TextMate grammars: syntax highlighting as VS Code does it
**
** VS Code colors code with the TextMate grammars (*.tmLanguage.json) its
** extensions bring, the built-in ones living in VS Code itself. mme looks
** for VS Code (next to `code` on the PATH, then the usual places), its
** extensions (~/.vscode/extensions, a portable install's data folder)
** and mme-data/extensions, and reads their package.json: which languages
** there are (their file extensions, names, first lines) and which grammar
** each has. A line is tokenized like vscode-textmate does it, from the
** rule stack the line before left; the scopes of a token get their color
** from the theme's tokenColors, VS Code's way. The stacks are kept for
** every line of a Doc, so an edit tokenizes again only from its line.
** When there is no grammar, esyntax.c's scanner does the job.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define NONE	((size_t)-1)
#define MAX_LINE	20000	/* longer lines are not tokenized (VS Code's maxTokenizationLineLength) */

typedef struct Grammar Grammar;
typedef struct Rule Rule;
typedef struct Scope Scope;


static int is_dir (const char *p) {
  OsStat st;
  return p && os_stat(p, &st) == 0 && st.exists && st.is_dir;
}


static int is_file (const char *p) {
  OsStat st;
  return p && os_stat(p, &st) == 0 && st.exists && !st.is_dir;
}


static Json *read_json (const char *path) {
  size_t len;
  char *s = read_file(path, &len);
  Json *j;
  if (s == NULL) return NULL;
  if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
    j = json_parse(s + 3, len - 3);	/* a BOM */
  else j = json_parse(s, len);
  free(s);
  return j;
}


/* an object's member by its name (json_get takes dots as a path) */
static const Json *member (const Json *o, const char *key) {
  size_t i;
  if (o == NULL || o->type != J_OBJ) return NULL;
  for (i = 0; i < o->n; i++)
    if (o->kid[i]->key && strcmp(o->kid[i]->key, key) == 0) return o->kid[i];
  return NULL;
}


static const char *mstr (const Json *o, const char *key) {
  const Json *v = member(o, key);
  return v && v->type == J_STR ? v->str : NULL;
}


/*
** {==================================================================
** VS Code and its extensions: the languages, the grammars
** ===================================================================
*/

typedef struct TLang {
  char *id, *alias, *config;
  Vec exts, names;	/* ".clj", "Dockerfile" */
  char *first;	/* firstLine: a pattern */
  int conf_read;
  char *line, *open, *close;	/* its comments */
} TLang;

typedef struct TGram {
  char *scope, *path, *lang;
  Grammar *g;
  int tried;
} TGram;

static TLang *g_lang;
static size_t g_nlang, g_caplang;
static TGram *g_gram;
static size_t g_ngram, g_capgram;
static char *g_builtin;	/* VS Code's built-in extensions */
static int g_scanned;


static TLang *lang_by_id (const char *id) {
  size_t i;
  for (i = 0; id && i < g_nlang; i++)
    if (strcmp(g_lang[i].id, id) == 0) return &g_lang[i];
  return NULL;
}


/* one extension's package.json: its languages and grammars */
static void read_ext (const char *dir) {
  char *f = path_join(dir, "package.json");
  Json *j = read_json(f);
  const Json *co, *ls, *gs;
  size_t i, k;
  free(f);
  if (j == NULL) return;
  co = member(j, "contributes");
  ls = member(co, "languages");
  gs = member(co, "grammars");
  for (i = 0; ls && ls->type == J_ARR && i < ls->n; i++) {
    const Json *l = ls->kid[i], *a;
    const char *id = mstr(l, "id");
    TLang *t;
    if (id == NULL) continue;
    if ((t = lang_by_id(id)) == NULL) {
      if (g_nlang == g_caplang) {
        g_caplang = g_caplang ? g_caplang * 2 : 64;
        g_lang = (TLang *)xrealloc(g_lang, g_caplang * sizeof(TLang));
      }
      t = &g_lang[g_nlang++];
      memset(t, 0, sizeof(*t));
      t->id = xstrdup(id);
      vec_init(&t->exts);
      vec_init(&t->names);
    }
    a = member(l, "aliases");
    if (t->alias == NULL && a && a->type == J_ARR && a->n > 0 && a->kid[0]->type == J_STR) t->alias = xstrdup(a->kid[0]->str);
    if (t->config == NULL && mstr(l, "configuration")) t->config = path_join(dir, mstr(l, "configuration"));
    if (t->first == NULL && mstr(l, "firstLine")) t->first = xstrdup(mstr(l, "firstLine"));
    a = member(l, "extensions");
    for (k = 0; a && a->type == J_ARR && k < a->n; k++)
      if (a->kid[k]->type == J_STR) vec_push(&t->exts, xstrdup(a->kid[k]->str));
    a = member(l, "filenames");
    for (k = 0; a && a->type == J_ARR && k < a->n; k++)
      if (a->kid[k]->type == J_STR) vec_push(&t->names, xstrdup(a->kid[k]->str));
  }
  for (i = 0; gs && gs->type == J_ARR && i < gs->n; i++) {
    const Json *g = gs->kid[i];
    const char *scope = mstr(g, "scopeName"), *path = mstr(g, "path"), *lang = mstr(g, "language");
    TGram *t = NULL;
    if (scope == NULL || path == NULL) continue;
    for (k = 0; k < g_ngram; k++)	/* a later one (a user's extension) takes a scope over */
      if (strcmp(g_gram[k].scope, scope) == 0) t = &g_gram[k];
    if (t == NULL) {
      if (g_ngram == g_capgram) {
        g_capgram = g_capgram ? g_capgram * 2 : 64;
        g_gram = (TGram *)xrealloc(g_gram, g_capgram * sizeof(TGram));
      }
      t = &g_gram[g_ngram++];
      memset(t, 0, sizeof(*t));
      t->scope = xstrdup(scope);
    }
    else if (t->g) continue;	/* already read: kept */
    free(t->path);
    t->path = path_join(dir, path);
    if (lang) {	/* one without a language keeps the one it had */
      free(t->lang);
      t->lang = xstrdup(lang);
    }
  }
  json_free(j);
}


static void read_exts (const char *dir) {
  Vec v;
  size_t i;
  if (!is_dir(dir)) return;
  vec_init(&v);
  os_listdir(dir, &v);
  vec_sort(&v);
  for (i = 0; i < v.n; i++) {
    char *d = path_join(dir, v.v[i]);
    if (is_dir(d)) read_ext(d);
    free(d);
  }
  vec_free(&v);
}


/* a VS Code install's folders: its built-in extensions (*builtin), a portable one's data */
static void install_at (const char *inst, Vec *user) {
  static const char *const rel[] = {"resources/app/extensions", "Contents/Resources/app/extensions",
                                    "app/extensions", "extensions"};
  size_t i;
  char *d;
  if (!is_dir(inst)) return;
  if (g_builtin == NULL)
    for (i = 0; i < sizeof(rel) / sizeof(rel[0]) && g_builtin == NULL; i++) {
      d = path_join(inst, rel[i]);
      if (is_dir(d) && i < 3) g_builtin = d;
      else free(d);
    }
  if (g_builtin == NULL) {	/* 1.9x: <install>/<commit>/resources/app/extensions */
    Vec v;
    vec_init(&v);
    os_listdir(inst, &v);
    for (i = 0; i < v.n && g_builtin == NULL; i++) {
      char *sub = path_join(inst, v.v[i]);
      d = path_join(sub, "resources/app/extensions");
      if (is_dir(d)) g_builtin = d;
      else free(d);
      free(sub);
    }
    vec_free(&v);
  }
  d = path_join(inst, "data/extensions");	/* portable: its own extensions */
  if (is_dir(d)) vec_push(user, d);
  else free(d);
}


static void find_vscode (Vec *user) {
  static const char *const progs[] = {"code", "code-insiders", "codium"};
  static const char *const envs[] = {"LOCALAPPDATA", "ProgramFiles", "ProgramFiles(x86)"};
  static const char *const under[] = {"Programs/Microsoft VS Code", "Microsoft VS Code", "Programs/VSCodium"};
  static const char *const fixed[] = {"/usr/share/code", "/usr/lib/code", "/opt/visual-studio-code",
                                      "/usr/share/codium", "/snap/code/current/usr/share/code",
                                      "/Applications/Visual Studio Code.app/Contents/Resources/app/.."};
  size_t i, k;
  for (i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {	/* next to `code` on the PATH */
    char *p = find_program(progs[i]), *real, *bin, *inst;
    if (p == NULL) continue;
    real = os_realpath(p);
    bin = path_dirname(real ? real : p);
    inst = path_dirname(bin);
    install_at(inst, user);
    free(inst);
    free(bin);
    free(real);
    free(p);
  }
  for (i = 0; i < sizeof(envs) / sizeof(envs[0]); i++) {
    char *e = os_getenv(envs[i]);
    if (e == NULL) continue;
    for (k = 0; k < sizeof(under) / sizeof(under[0]); k++) {
      char *d = path_join(e, under[k]);
      install_at(d, user);
      free(d);
    }
    free(e);
  }
  for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) install_at(fixed[i], user);
  {	/* the user's extensions */
    char *home = os_getenv("HOME"), *d;
    if (home == NULL) home = os_getenv("USERPROFILE");
    if (home) {
      d = path_join(home, ".vscode/extensions");
      vec_push(user, d);
      free(home);
    }
#ifdef _WIN32
    home = os_getenv("USERPROFILE");
    if (home) {
      d = path_join(home, ".vscode/extensions");
      vec_push(user, d);
      free(home);
    }
#endif
  }
}


static void scan (void) {
  Vec user;
  size_t i, k;
  char *mine;
  if (g_scanned) return;
  g_scanned = 1;
  vec_init(&user);
  find_vscode(&user);
  if (g_builtin) read_exts(g_builtin);
  for (i = 0; i < user.n; i++) {	/* each folder once */
    int seen = 0;
    for (k = 0; k < i; k++)
      if (m_fncmp(user.v[k], user.v[i]) == 0) seen = 1;
    if (!seen) read_exts(user.v[i]);
  }
  vec_free(&user);
  mine = ext_dir();
  if (mine) read_exts(mine);
  free(mine);
}


static TGram *gram_by_scope (const char *scope) {
  size_t i;
  for (i = 0; scope && i < g_ngram; i++)
    if (strcmp(g_gram[i].scope, scope) == 0) return &g_gram[i];
  return NULL;
}


/* a language's comments, from its language-configuration.json */
static void lang_config (TLang *t) {
  Json *j;
  const Json *c, *b;
  if (t->conf_read) return;
  t->conf_read = 1;
  if (t->config == NULL || (j = read_json(t->config)) == NULL) return;
  c = member(j, "comments");
  if (mstr(c, "lineComment")) t->line = xstrdup(mstr(c, "lineComment"));
  b = member(c, "blockComment");
  if (b && b->type == J_ARR && b->n == 2 && b->kid[0]->type == J_STR && b->kid[1]->type == J_STR) {
    t->open = xstrdup(b->kid[0]->str);
    t->close = xstrdup(b->kid[1]->str);
  }
  json_free(j);
}

/* }================================================================== */


/*
** {==================================================================
** Grammars and their rules
** ===================================================================
*/

typedef struct Re {	/* a pattern, compiled when first used; its last search, for the same line */
  char *src;
  Onig *o;
  int tried;
  unsigned long text;	/* the line the result is for */
  size_t from;
  int found;
  size_t *caps;
  int ncap;	/* 2 * (groups + 1) */
} Re;

typedef struct Cap {
  char *name;	/* the scope, maybe with $1 */
  Rule *rule;	/* "patterns": the capture is tokenized with them */
} Cap;

enum { RK_MATCH, RK_BEGIN_END, RK_BEGIN_WHILE, RK_INCLUDE };

struct Rule {
  int kind;
  Grammar *g;
  const Json *js;
  const Json **chain;	/* the repositories it sees, the nearest first */
  int nchain;
  char *name, *content;
  Re begin;	/* match, or begin */
  Re end;	/* end, or while */
  int end_back;	/* the end has \1: made for each begin */
  int end_last;	/* applyEndPatternLast */
  Cap *caps, *bcaps, *ecaps;
  int ncaps, nbcaps, necaps;
  const Json *pats_js;	/* "patterns" */
  const char *inc;	/* {"include": ...} alone */
  Rule **pats;
  int npat;
  const Grammar *pats_base;	/* the grammar $base was for; NULL: not made */
  Rule **scan;	/* the rules a scan tries: the patterns, includes in place */
  int nscan;
  const Grammar *scan_base;
  unsigned visit;
};

struct Grammar {
  char *scope;
  Json *js;
  const Json *repo;
  Rule *root;
};

typedef struct Memo {	/* JSON object -> its rule */
  const Json *js;
  Rule *r;
} Memo;

static Memo *g_memo;
static size_t g_nmemo, g_capmemo;	/* a hash table: capmemo a power of two */
static unsigned g_stamp;


static Rule *memo_get (const Json *js) {
  size_t h;
  if (g_capmemo == 0) return NULL;
  h = ((size_t)js >> 4) & (g_capmemo - 1);
  while (g_memo[h].js) {
    if (g_memo[h].js == js) return g_memo[h].r;
    h = (h + 1) & (g_capmemo - 1);
  }
  return NULL;
}


static void memo_put (const Json *js, Rule *r) {
  size_t h;
  if ((g_nmemo + 1) * 2 > g_capmemo) {
    Memo *old = g_memo;
    size_t oc = g_capmemo, i;
    g_capmemo = oc ? oc * 2 : 1024;
    g_memo = (Memo *)calloc(g_capmemo, sizeof(Memo));
    g_nmemo = 0;
    for (i = 0; i < oc; i++)
      if (old[i].js) memo_put(old[i].js, old[i].r);
    free(old);
  }
  h = ((size_t)js >> 4) & (g_capmemo - 1);
  while (g_memo[h].js) h = (h + 1) & (g_capmemo - 1);
  g_memo[h].js = js;
  g_memo[h].r = r;
  g_nmemo++;
}


static char *dup_or_null (const char *s) {
  return s ? xstrdup(s) : NULL;
}


static Rule *rule_of (Grammar *g, const Json *js, const Json *const *chain, int nchain);


/* "captures": {"1": {"name": ...}} or [{...}] */
static Cap *caps_of (Rule *owner, const Json *cj, int *n) {
  Cap *v;
  size_t i;
  int max = -1;
  *n = 0;
  if (cj == NULL || (cj->type != J_OBJ && cj->type != J_ARR)) return NULL;
  for (i = 0; i < cj->n; i++) {
    int k = cj->type == J_ARR ? (int)i : atoi(cj->kid[i]->key ? cj->kid[i]->key : "-1");
    if (k > max && k < 10000) max = k;
  }
  if (max < 0) return NULL;
  v = (Cap *)calloc((size_t)max + 1, sizeof(Cap));
  for (i = 0; i < cj->n; i++) {
    const Json *c = cj->kid[i];
    int k = cj->type == J_ARR ? (int)i : atoi(c->key ? c->key : "-1");
    if (k < 0 || k > max || c->type != J_OBJ) continue;
    v[k].name = dup_or_null(mstr(c, "name"));
    if (member(c, "patterns")) v[k].rule = rule_of(owner->g, c, owner->chain, owner->nchain);
  }
  *n = max + 1;
  return v;
}


static int has_backref (const char *s) {
  for (; s && *s; s++) {
    if (s[0] == '\\' && s[1] >= '0' && s[1] <= '9') return 1;
    if (s[0] == '\\' && s[1]) s++;
  }
  return 0;
}


static Rule *rule_of (Grammar *g, const Json *js, const Json *const *chain, int nchain) {
  Rule *r;
  const Json *repo, *e, *w;
  if (js == NULL || js->type != J_OBJ) return NULL;
  if ((r = memo_get(js)) != NULL) return r;
  r = (Rule *)calloc(1, sizeof(Rule));
  memo_put(js, r);
  r->g = g;
  r->js = js;
  repo = member(js, "repository");
  r->nchain = nchain + (repo ? 1 : 0);
  r->chain = (const Json **)xmalloc((size_t)(r->nchain + 1) * sizeof(Json *));
  if (repo) r->chain[0] = repo;
  if (nchain) memcpy(r->chain + (repo ? 1 : 0), chain, (size_t)nchain * sizeof(Json *));
  r->name = dup_or_null(mstr(js, "name"));
  r->content = dup_or_null(mstr(js, "contentName"));
  r->pats_js = member(js, "patterns");
  r->inc = mstr(js, "include");
  e = member(js, "end");
  w = member(js, "while");
  if (mstr(js, "match")) {
    r->kind = RK_MATCH;
    r->begin.src = xstrdup(mstr(js, "match"));
    r->caps = caps_of(r, member(js, "captures"), &r->ncaps);
  }
  else if (mstr(js, "begin") && (e || w)) {
    const Json *bc = member(js, "beginCaptures"), *ec = member(js, e ? "endCaptures" : "whileCaptures");
    r->kind = e ? RK_BEGIN_END : RK_BEGIN_WHILE;
    r->begin.src = xstrdup(mstr(js, "begin"));
    r->end.src = xstrdup(json_str(e ? e : w, ""));
    r->end_back = has_backref(r->end.src);
    r->end_last = json_bool(member(js, "applyEndPatternLast"), 0) || json_num(member(js, "applyEndPatternLast"), 0) != 0;
    r->bcaps = caps_of(r, bc ? bc : member(js, "captures"), &r->nbcaps);
    r->ecaps = caps_of(r, ec ? ec : member(js, "captures"), &r->necaps);
  }
  else r->kind = RK_INCLUDE;
  return r;
}


static Grammar *grammar_of (const char *scope);


/* an include, seen from rule o: its rule; NULL: not there */
static Rule *resolve (Rule *o, const char *inc, Grammar *base) {
  int i;
  if (strcmp(inc, "$self") == 0) return o->g->root;
  if (strcmp(inc, "$base") == 0) return base ? base->root : o->g->root;
  if (inc[0] == '#') {
    for (i = 0; i < o->nchain; i++) {
      const Json *e = member(o->chain[i], inc + 1);
      if (e) return rule_of(o->g, e, o->chain + i, o->nchain - i);
    }
    return NULL;
  }
  {	/* another grammar: "source.js", "source.js#expression" */
    const char *h = strchr(inc, '#');
    char scope[256];
    size_t n = h ? (size_t)(h - inc) : strlen(inc);
    Grammar *g2;
    if (n >= sizeof(scope)) return NULL;
    memcpy(scope, inc, n);
    scope[n] = '\0';
    if ((g2 = grammar_of(scope)) == NULL) return NULL;
    if (h == NULL) return g2->root;
    {
      const Json *e = member(g2->repo, h + 1);
      return e ? rule_of(g2, e, &g2->repo, 1) : NULL;
    }
  }
}


/* a rule's patterns, includes resolved (for base grammar base) */
static void rule_pats (Rule *r, Grammar *base) {
  size_t i, n;
  if (r->pats_base == base && r->pats_base) return;
  free(r->pats);
  r->pats = NULL;
  r->npat = 0;
  r->pats_base = base;
  n = r->pats_js && r->pats_js->type == J_ARR ? r->pats_js->n : 0;
  r->pats = (Rule **)xmalloc((n + 1) * sizeof(Rule *));
  if (r->inc && n == 0) {	/* {"include": "#x"} alone */
    Rule *x = resolve(r, r->inc, base);
    if (x) r->pats[r->npat++] = x;
    return;
  }
  for (i = 0; i < n; i++) {
    const Json *p = r->pats_js->kid[i];
    const char *inc = mstr(p, "include");
    Rule *x = inc ? resolve(r, inc, base) : rule_of(r->g, p, r->chain, r->nchain);
    if (x) r->pats[r->npat++] = x;
  }
}


static void collect (Rule *r, Grammar *base, Rule ***v, int *n, int *cap, int depth) {
  int i;
  rule_pats(r, base);
  for (i = 0; i < r->npat; i++) {
    Rule *p = r->pats[i];
    if (p->visit == g_stamp) continue;
    p->visit = g_stamp;
    if (p->kind == RK_INCLUDE) {
      if (depth < 64) collect(p, base, v, n, cap, depth + 1);
      continue;
    }
    if (*n == *cap) {
      *cap = *cap ? *cap * 2 : 16;
      *v = (Rule **)xrealloc(*v, (size_t)*cap * sizeof(Rule *));
    }
    (*v)[(*n)++] = p;
  }
}


/* the rules a scan in rule r tries, in order */
static void scan_list (Rule *r, Grammar *base) {
  int cap = 0;
  if (r->scan_base == base && r->scan) return;
  free(r->scan);
  r->scan = NULL;
  r->nscan = 0;
  g_stamp++;
  r->visit = g_stamp;
  collect(r, base, &r->scan, &r->nscan, &cap, 0);
  if (r->scan == NULL) r->scan = (Rule **)xmalloc(sizeof(Rule *));
  r->scan_base = base;
}


static Grammar *grammar_of (const char *scope) {
  TGram *t = gram_by_scope(scope);
  Grammar *g;
  Json *j;
  const Json *repo;
  if (t == NULL) return NULL;
  if (t->g || t->tried) return t->g;
  t->tried = 1;
  if ((j = read_json(t->path)) == NULL) return NULL;
  g = (Grammar *)calloc(1, sizeof(Grammar));
  g->scope = xstrdup(scope);
  g->js = j;
  repo = member(j, "repository");
  g->repo = repo;
  g->root = (Rule *)calloc(1, sizeof(Rule));
  g->root->kind = RK_INCLUDE;
  g->root->g = g;
  g->root->js = j;
  g->root->pats_js = member(j, "patterns");
  g->root->nchain = repo ? 1 : 0;
  g->root->chain = (const Json **)xmalloc(sizeof(Json *));
  g->root->chain[0] = repo;
  g->root->name = xstrdup(scope);
  t->g = g;
  return g;
}


static Grammar *grammar_for_lang (const char *id) {
  size_t i;
  const char *scope = NULL;
  if (id == NULL) return NULL;
  scan();
  for (i = 0; i < g_ngram; i++)	/* the last one for the language */
    if (g_gram[i].lang && strcmp(g_gram[i].lang, id) == 0) scope = g_gram[i].scope;
  return scope ? grammar_of(scope) : NULL;
}

/* }================================================================== */


/*
** {==================================================================
** Scopes and the theme's colors for them
** ===================================================================
*/

struct Scope {
  Scope *parent;
  int atom;	/* its name */
  Scope *hnext;
  unsigned gen;	/* the theme its style is for */
  uint32_t fg;
  int has_fg, fs;
  unsigned char cls;	/* T_*: what it is for mme (comments, strings ...) */
};

static char **g_atom;
static int g_natom, g_capatom;
static int *g_ahash;	/* 8192 heads, -1 none */
static int *g_anext;
static Scope *g_shash[8192];

typedef struct TRule {
  char **part;	/* "source.c string": the parents, then the scope */
  int npart;
  uint32_t fg;
  int has_fg, fs, has_fs, order;
} TRule;

static TRule *g_tr;
static size_t g_ntr, g_captr;
static unsigned g_gen = 1;
static int g_theme = -99;	/* the theme the rules are for */


static unsigned hash_str (const char *s, size_t n) {
  unsigned h = 2166136261u;
  size_t i;
  for (i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}


static int atom (const char *s, size_t n) {
  unsigned h = hash_str(s, n) & 8191;
  int a;
  if (g_ahash == NULL) {
    int i;
    g_ahash = (int *)xmalloc(8192 * sizeof(int));
    for (i = 0; i < 8192; i++) g_ahash[i] = -1;
  }
  for (a = g_ahash[h]; a >= 0; a = g_anext[a])
    if (strlen(g_atom[a]) == n && memcmp(g_atom[a], s, n) == 0) return a;
  if (g_natom == g_capatom) {
    g_capatom = g_capatom ? g_capatom * 2 : 1024;
    g_atom = (char **)xrealloc(g_atom, (size_t)g_capatom * sizeof(char *));
    g_anext = (int *)xrealloc(g_anext, (size_t)g_capatom * sizeof(int));
  }
  a = g_natom++;
  g_atom[a] = (char *)xmalloc(n + 1);
  memcpy(g_atom[a], s, n);
  g_atom[a][n] = '\0';
  g_anext[a] = g_ahash[h];
  g_ahash[h] = a;
  return a;
}


static Scope *scope_push1 (Scope *parent, int a) {
  unsigned h = (unsigned)(((size_t)parent >> 4) * 31u + (unsigned)a) & 8191;
  Scope *s;
  for (s = g_shash[h]; s; s = s->hnext)
    if (s->parent == parent && s->atom == a) return s;
  s = (Scope *)calloc(1, sizeof(Scope));
  s->parent = parent;
  s->atom = a;
  s->hnext = g_shash[h];
  g_shash[h] = s;
  return s;
}


/* "meta.a string.b": each pushed */
static Scope *scope_push (Scope *parent, const char *names) {
  const char *p = names;
  if (names == NULL) return parent;
  while (*p) {
    const char *e;
    while (*p == ' ') p++;
    e = p;
    while (*e && *e != ' ') e++;
    if (e > p) parent = scope_push1(parent, atom(p, (size_t)(e - p)));
    p = e;
  }
  return parent;
}


/* is sel ("string.quoted") the scope or a start of it ("string.quoted.double.c")? */
static int seg_prefix (const char *sel, const char *name) {
  size_t n = strlen(sel);
  return strncmp(name, sel, n) == 0 && (name[n] == '\0' || name[n] == '.');
}


/* what a scope is for mme's other uses (brackets in strings, the minimap ...) */
static int cls_of (const char *name) {
  static const struct {
    const char *pre;
    int t;
  } map[] = {
    {"comment", T_COMMENT}, {"constant.character.escape", T_ESCAPE}, {"string", T_STRING},
    {"constant.numeric", T_NUMBER}, {"constant.regexp", T_STRING}, {"constant", T_CONST},
    {"support.constant", T_CONST}, {"variable.other.constant", T_CONST},
    {"variable.other.enummember", T_CONST}, {"keyword.operator", T_TEXT}, {"keyword.control", T_KEYWORD},
    {"keyword", T_STORAGE}, {"storage", T_STORAGE}, {"entity.name.function", T_FUNC},
    {"support.function", T_FUNC}, {"entity.name.type", T_TYPE}, {"entity.name.class", T_TYPE},
    {"entity.name.namespace", T_TYPE}, {"entity.other.inherited-class", T_TYPE}, {"support.type", T_TYPE},
    {"support.class", T_TYPE}, {"entity.name.tag", T_STORAGE}, {"entity.other.attribute-name", T_VAR},
    {"variable.language", T_STORAGE}, {"variable", T_VAR}, {"support.variable", T_VAR},
    {"markup.heading", T_HEADING}, {"entity.name.section", T_HEADING}, {"markup.inline.raw", T_STRING}
  };
  size_t i;
  for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (seg_prefix(map[i].pre, name)) return map[i].t;
  return -1;
}


static int hexv (int c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}


/* "#rgb" "#rrggbb" "#rrggbbaa", a translucent one over the editor's background */
static int color_of (const char *s, uint32_t *out) {
  unsigned v[8], r, g, b, a = 255, under = ui_color(C_EDITOR_BG);
  size_t n, i;
  if (s == NULL || s[0] != '#') return 0;
  n = strlen(++s);
  if (n != 3 && n != 4 && n != 6 && n != 8) return 0;
  for (i = 0; i < n; i++) {
    int h = hexv((unsigned char)s[i]);
    if (h < 0) return 0;
    v[i] = (unsigned)h;
  }
  if (n <= 4) {
    r = v[0] * 17;
    g = v[1] * 17;
    b = v[2] * 17;
    if (n == 4) a = v[3] * 17;
  }
  else {
    r = v[0] * 16 + v[1];
    g = v[2] * 16 + v[3];
    b = v[4] * 16 + v[5];
    if (n == 8) a = v[6] * 16 + v[7];
  }
  if (a < 255) {
    r = (r * a + (under >> 16) * (255 - a)) / 255;
    g = (g * a + ((under >> 8) & 255) * (255 - a)) / 255;
    b = (b * a + (under & 255) * (255 - a)) / 255;
  }
  *out = (r << 16) | (g << 8) | b;
  return 1;
}


static void theme_rules_free (void) {
  size_t i;
  int k;
  for (i = 0; i < g_ntr; i++) {
    for (k = 0; k < g_tr[i].npart; k++) free(g_tr[i].part[k]);
    free(g_tr[i].part);
  }
  g_ntr = 0;
}


/* one tokenColors entry: a rule for each of its selectors */
static void add_rule (const Json *e) {
  const Json *sc = member(e, "scope"), *st = member(e, "settings");
  const char *fg = mstr(st, "foreground"), *fs = mstr(st, "fontStyle");
  size_t i, nsel = sc ? (sc->type == J_ARR ? sc->n : 1) : 0;
  uint32_t rgb = 0;
  int has_fg = color_of(fg, &rgb), style = 0;
  if (sc == NULL || (!has_fg && fs == NULL)) return;
  if (fs) {
    if (strstr(fs, "italic")) style |= RGB_ITALIC;
    if (strstr(fs, "bold")) style |= RGB_BOLD;
    if (strstr(fs, "underline")) style |= RGB_UNDER;
  }
  for (i = 0; i < nsel; i++) {
    const char *s = sc->type == J_ARR ? json_str(sc->kid[i], "") : json_str(sc, "");
    while (*s) {	/* "a, b.c x.y" */
      const char *e2 = strchr(s, ','), *end;
      TRule *t;
      char buf[512];
      size_t n;
      int np = 0;
      char *parts[16];
      while (*s == ' ' || *s == '\t') s++;
      end = e2 ? e2 : s + strlen(s);
      n = (size_t)(end - s);
      while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
      if (n > 0 && n < sizeof(buf) && memchr(s, '-', n) != s && strstr(s, " -") == NULL) {
        char *p = buf, *q;
        memcpy(buf, s, n);
        buf[n] = '\0';
        while (*p && np < 16) {
          while (*p == ' ' || *p == '>') p++;
          q = p;
          while (*q && *q != ' ' && *q != '>') q++;
          if (q > p) {
            parts[np] = (char *)xmalloc((size_t)(q - p) + 1);
            memcpy(parts[np], p, (size_t)(q - p));
            parts[np][q - p] = '\0';
            np++;
          }
          p = q;
        }
        if (np > 0) {
          if (g_ntr == g_captr) {
            g_captr = g_captr ? g_captr * 2 : 256;
            g_tr = (TRule *)xrealloc(g_tr, g_captr * sizeof(TRule));
          }
          t = &g_tr[g_ntr];
          t->part = (char **)xmalloc((size_t)np * sizeof(char *));
          memcpy(t->part, parts, (size_t)np * sizeof(char *));
          t->npart = np;
          t->fg = rgb;
          t->has_fg = has_fg;
          t->fs = style;
          t->has_fs = fs != NULL;
          t->order = (int)g_ntr;
          g_ntr++;
        }
      }
      if (e2 == NULL) break;
      s = e2 + 1;
    }
  }
}


/* a theme file's tokenColors (its "include" first) */
static void theme_file (const char *path, int depth) {
  Json *j = read_json(path);
  const char *inc;
  const Json *tc;
  size_t i;
  if (j == NULL) return;
  inc = mstr(j, "include");
  if (inc && depth < 6) {
    char *dir = path_dirname(path), *f = path_join(dir, inc);
    theme_file(f, depth + 1);
    free(f);
    free(dir);
  }
  tc = member(j, "tokenColors");
  for (i = 0; tc && tc->type == J_ARR && i < tc->n; i++) add_rule(tc->kid[i]);
  json_free(j);
}


/* mme's own themes are VS Code's: their files in VS Code's theme extensions */
static char *builtin_theme (const char *name) {
  static const struct {
    const char *name, *file;
  } map[] = {
    {"Dark Modern", "theme-defaults/themes/dark_modern.json"}, {"Dark+", "theme-defaults/themes/dark_plus.json"},
    {"Light+", "theme-defaults/themes/light_plus.json"}, {"Monokai", "theme-monokai/themes/monokai-color-theme.json"},
    {"Solarized Dark", "theme-solarized-dark/themes/solarized-dark-color-theme.json"}
  };
  size_t i;
  if (g_builtin == NULL) return NULL;
  for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (strcmp(map[i].name, name) == 0) {
      char *f = path_join(g_builtin, map[i].file);
      if (is_file(f)) return f;
      free(f);
    }
  return NULL;
}


/* the theme now: its rules read when it changed */
static void theme_now (void) {
  int t = theme_current();
  const char *name;
  char *f;
  if (t == g_theme) return;
  g_theme = t;
  g_gen++;
  theme_rules_free();
  scan();
  name = theme_name(t);
  if (name == NULL) return;
  f = builtin_theme(name);
  if (f == NULL && ext_theme_path(name)) f = xstrdup(ext_theme_path(name));
  if (f) theme_file(f, 0);
  free(f);
}


/* how well a rule fits scope s (with its parents); -1 not */
static int rule_score (const TRule *r, const Scope *s) {
  const Scope *a;
  const char *t = r->part[r->npart - 1];
  int j = r->npart - 2, segs = 1;
  const char *c;
  if (!seg_prefix(t, g_atom[s->atom])) return -1;
  for (a = s->parent; a && j >= 0; a = a->parent)
    if (seg_prefix(r->part[j], g_atom[a->atom])) j--;
  if (j >= 0) return -1;
  for (c = t; *c; c++)
    if (*c == '.') segs++;
  return segs * 64 + (r->npart - 1);
}


static void scope_style (Scope *s) {
  size_t i;
  int bf = -1, bs = -1, c;
  const TRule *rf = NULL, *rs = NULL;
  if (s->gen == g_gen) return;
  if (s->parent) {
    scope_style(s->parent);
    s->fg = s->parent->fg;
    s->has_fg = s->parent->has_fg;
    s->fs = s->parent->fs;
    s->cls = s->parent->cls;
  }
  else {
    s->fg = 0;
    s->has_fg = 0;
    s->fs = 0;
    s->cls = T_TEXT;
  }
  if ((c = cls_of(g_atom[s->atom])) >= 0) s->cls = (unsigned char)c;
  for (i = 0; i < g_ntr; i++) {	/* the best rule for its color and its style (later ones win a tie) */
    const TRule *r = &g_tr[i];
    int sc = rule_score(r, s);
    if (sc < 0) continue;
    if (r->has_fg && sc >= bf) {
      bf = sc;
      rf = r;
    }
    if (r->has_fs && sc >= bs) {
      bs = sc;
      rs = r;
    }
  }
  if (rf) {
    s->fg = rf->fg;
    s->has_fg = 1;
  }
  if (rs) s->fs = rs->fs;
  s->gen = g_gen;
}

/* }================================================================== */


/*
** {==================================================================
** Tokenizing a line, vscode-textmate's way
** ===================================================================
*/

typedef struct Frame Frame;
struct Frame {
  Frame *parent;
  Rule *rule;
  int refs;
  unsigned long line;	/* the line it was pushed on */
  size_t enter, anchor;	/* on that line */
  int eol;	/* its begin ran to the end of that line */
  Re *end;	/* its end (while) with the begin's captures in it; NULL: the rule's */
  Scope *name, *content;
};

typedef struct Out {	/* what a line's bytes are */
  unsigned char *cls;
  uint32_t *fg;	/* 0x1000000 | rgb; 0: the theme's color of cls */
  unsigned char *fs;
  Scope **sc;	/* for Inspect Editor Tokens, or NULL */
  size_t n, last;
} Out;

static unsigned long g_line;	/* each physical line tokenized gets a new one */
static unsigned long g_text;	/* each string searched gets a new one */
static int g_guard;


static void fr_unref (Frame *f) {
  while (f && --f->refs == 0) {
    Frame *p = f->parent;
    free(f);
    f = p;
  }
}


static Frame *fr_push (Frame *parent, Rule *r) {	/* takes a reference to parent */
  Frame *f = (Frame *)calloc(1, sizeof(Frame));
  f->parent = parent;
  f->rule = r;
  f->refs = 1;
  return f;
}


static Frame *fr_own (Frame *f) {	/* a frame of our own to change */
  Frame *c;
  if (f->refs == 1) return f;
  c = (Frame *)xmalloc(sizeof(Frame));
  *c = *f;
  c->refs = 1;
  if (c->parent) c->parent->refs++;
  f->refs--;
  return c;
}


static Frame *fr_pop (Frame *f) {
  Frame *p = f->parent;
  if (p) p->refs++;
  fr_unref(f);
  return p;
}


static size_t enter_of (const Frame *f) {
  return f->line == g_line ? f->enter : NONE;
}


static void produce (Out *o, Scope *s, size_t end) {
  size_t i;
  if (o == NULL) return;
  if (end > o->n) end = o->n;
  if (end <= o->last) return;
  scope_style(s);
  for (i = o->last; i < end; i++) {
    o->cls[i] = s->cls;
    o->fg[i] = s->has_fg ? (s->fg | 0x1000000) : 0;
    o->fs[i] = (unsigned char)s->fs;
    if (o->sc) o->sc[i] = s;
  }
  o->last = end;
}


/* a pattern's search from pos in s[0..n), its last result used again when it is the same line */
static int re_search (Re *re, const char *s, size_t n, size_t pos, size_t g, int first) {
  if (!re->tried) {
    const char *err = NULL;
    re->tried = 1;
    re->o = onig_compile(re->src, &err);
    if (re->o) {
      re->ncap = 2 * (onig_groups(re->o) + 1);
      re->caps = (size_t *)xmalloc((size_t)re->ncap * sizeof(size_t));
    }
  }
  if (re->o == NULL) return 0;
  if (!onig_uses_g(re->o) && re->text == g_text && re->from <= pos && (!re->found || re->caps[0] >= pos))
    return re->found;
  re->text = g_text;
  re->from = pos;
  re->found = onig_search(re->o, s, n, pos, g, first, re->caps);
  if (onig_uses_g(re->o)) re->text = 0;
  return re->found;
}


/* $1, ${1:/downcase} in a scope name: the capture's text */
static const char *name_of (const char *tmpl, const char *s, const size_t *caps, int ncap, char *buf, size_t bn) {
  size_t o = 0;
  const char *p;
  if (tmpl == NULL || strchr(tmpl, '$') == NULL) return tmpl;
  for (p = tmpl; *p && o + 1 < bn;) {
    int k = -1, mode = 0;
    const char *q = p;
    if (p[0] == '$' && p[1] >= '0' && p[1] <= '9') {
      k = 0;
      q = p + 1;
      while (*q >= '0' && *q <= '9') k = k * 10 + (*q++ - '0');
    }
    else if (p[0] == '$' && p[1] == '{' && p[2] >= '0' && p[2] <= '9') {
      k = 0;
      q = p + 2;
      while (*q >= '0' && *q <= '9') k = k * 10 + (*q++ - '0');
      if (strncmp(q, ":/downcase}", 11) == 0) mode = 1, q += 11;
      else if (strncmp(q, ":/upcase}", 9) == 0) mode = 2, q += 9;
      else if (*q == '}') q++;
      else k = -1;
    }
    if (k < 0) {
      buf[o++] = *p++;
      continue;
    }
    if (2 * k + 1 < ncap && caps[2 * k] != NONE) {
      size_t a = caps[2 * k], b = caps[2 * k + 1];
      while (a < b && s[a] == '.') a++;
      for (; a < b && o + 1 < bn; a++) {
        char c = s[a];
        if (c == ' ') c = '_';
        if (mode == 1 && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (mode == 2 && c >= 'a' && c <= 'z') c = (char)(c - 32);
        buf[o++] = c;
      }
    }
    p = q;
  }
  buf[o] = '\0';
  return buf;
}


/* an end pattern with the begin's captures put in for \1 (quoted as regex text) */
static Re *end_with_back (Rule *r, const char *s, const size_t *caps, int ncap) {
  static Re **cache;
  static size_t ncache, capcache;
  Buf b;
  const char *p;
  size_t i;
  Re *re;
  buf_init(&b);
  for (p = r->end.src; *p; p++) {
    if (p[0] == '\\' && p[1] >= '0' && p[1] <= '9') {
      int k = p[1] - '0';
      if (2 * k + 1 < ncap && caps[2 * k] != NONE) {
        size_t a;
        for (a = caps[2 * k]; a < caps[2 * k + 1]; a++) {
          if (strchr("\\^$.|?*+()[]{}/-,# \t", s[a])) buf_putc(&b, '\\');
          buf_putc(&b, s[a]);
        }
      }
      p++;
      continue;
    }
    if (p[0] == '\\' && p[1]) buf_putc(&b, *p++);
    buf_putc(&b, *p);
  }
  buf_putc(&b, '\0');
  for (i = 0; i < ncache; i++)
    if (strcmp(cache[i]->src, b.s) == 0) {
      buf_free(&b);
      return cache[i];
    }
  if (ncache == capcache) {
    capcache = capcache ? capcache * 2 : 64;
    cache = (Re **)xrealloc(cache, capcache * sizeof(Re *));
  }
  re = (Re *)calloc(1, sizeof(Re));
  re->src = buf_take(&b);
  cache[ncache++] = re;
  return re;
}


typedef struct Hit {
  Rule *rule;	/* NULL: the end */
  size_t *caps;	/* small, or the heap for a pattern with many groups */
  int ncap;
  size_t small[64];
} Hit;


static void hit_set (Hit *h, Rule *r, const Re *re) {
  if (h->caps && h->caps != h->small) free(h->caps);
  h->rule = r;
  h->ncap = re->ncap;
  h->caps = h->ncap <= 64 ? h->small : (size_t *)xmalloc((size_t)h->ncap * sizeof(size_t));
  memcpy(h->caps, re->caps, (size_t)h->ncap * sizeof(size_t));
}


static void hit_done (Hit *h) {
  if (h->caps && h->caps != h->small) free(h->caps);
  h->caps = NULL;
}


/* the first match in the rule on top: the end, or one of its patterns */
static int scan_next (Grammar *base, Frame *st, const char *s, size_t n, size_t pos, size_t anchor, int first, Hit *h) {
  Rule *r = st->rule;
  size_t best = NONE, g = anchor == pos ? pos : NONE;
  int k, has_end = r->kind == RK_BEGIN_END;
  Re *end = st->end ? st->end : &r->end;
  scan_list(r, base);
  for (k = -1; k <= r->nscan; k++) {	/* the end first (or last), then the patterns in order */
    Re *re;
    Rule *which;
    if (k == -1 || k == r->nscan) {
      if (!has_end || (k == -1) == (r->end_last != 0)) continue;
      re = end;
      which = NULL;
    }
    else {
      which = r->scan[k];
      re = &which->begin;
    }
    if (!re_search(re, s, n, pos, g, first)) continue;
    if (best == NONE || re->caps[0] < best) {
      best = re->caps[0];
      hit_set(h, which, re);
      if (best == pos) break;	/* none can come before it */
    }
  }
  return best != NONE;
}


static Frame *tok_string (Grammar *base, const char *s, size_t n, int first, size_t pos, Frame *st, Out *o, int check_while);


/* a match's captures: their scopes, or their patterns */
static void captures (Grammar *base, const char *s, size_t n, int first, Frame *st, const Cap *cv, int ncv,
                      const Hit *h, Out *o) {
  Scope *ls[100];
  size_t le[100], maxend = h->caps[1];
  int nl = 0, i;	/* HTML's entity match has 900 captures, most of them without a name */
  char buf[512];
  (void)n;
  for (i = 0; i < ncv && 2 * i + 1 < h->ncap; i++) {
    const Cap *c = &cv[i];
    size_t a = h->caps[2 * i], b = h->caps[2 * i + 1];
    if ((c->name == NULL && c->rule == NULL) || a == NONE || b == a) continue;
    if (a > maxend) break;
    while (nl > 0 && le[nl - 1] <= a) {
      produce(o, ls[nl - 1], le[nl - 1]);
      nl--;
    }
    produce(o, nl > 0 ? ls[nl - 1] : st->content, a);
    if (c->rule && g_guard < 64) {	/* tokenized with its own patterns, up to its end */
      Frame *f;
      st->refs++;
      f = fr_push(st, c->rule);
      f->name = scope_push(st->content, name_of(c->name, s, h->caps, h->ncap, buf, sizeof(buf)));
      f->content = f->name;
      f->line = g_line;
      f->enter = a;
      f->anchor = NONE;
      g_guard++;
      fr_unref(tok_string(base, s, b, first && a == 0, a, f, o, 0));
      g_guard--;
      continue;
    }
    if (c->name && nl < 100) {
      ls[nl] = scope_push(nl > 0 ? ls[nl - 1] : st->content, name_of(c->name, s, h->caps, h->ncap, buf, sizeof(buf)));
      le[nl++] = b;
    }
  }
  while (nl > 0) {
    produce(o, ls[nl - 1], le[nl - 1]);
    nl--;
  }
}


/* at a line's start: the while rules on the stack must still match, else they end */
static Frame *check_whiles (Grammar *base, const char *s, size_t n, int *first, size_t *pos, size_t *anchor, Frame *st, Out *o) {
  Frame *w[64], *f;
  int nw = 0, i;
  *anchor = st->eol ? 0 : NONE;
  for (f = st; f && nw < 64; f = f->parent)
    if (f->rule && f->rule->kind == RK_BEGIN_WHILE) w[nw++] = f;
  for (i = nw - 1; i >= 0; i--) {	/* the outermost first */
    Frame *wf = w[i];
    Re *re = wf->end ? wf->end : &wf->rule->end;
    if (re_search(re, s, n, *pos, *anchor == *pos ? *pos : NONE, *first)) {
      Hit h;
      h.caps = NULL;
      hit_set(&h, NULL, re);
      produce(o, wf->content, h.caps[0]);
      captures(base, s, n, *first, wf, wf->rule->ecaps, wf->rule->necaps, &h, o);
      produce(o, wf->content, h.caps[1]);
      *anchor = h.caps[1];
      if (h.caps[1] > *pos) {
        *pos = h.caps[1];
        *first = 0;
      }
      hit_done(&h);
    }
    else {	/* it ended: it and what is above it go */
      Frame *p = wf->parent;
      if (p) p->refs++;
      fr_unref(st);
      st = p;
      break;
    }
  }
  return st;
}


static int same_rule (const Frame *before, const Frame *now) {
  const Frame *el;
  for (el = before; el && enter_of(el) == now->enter; el = el->parent)
    if (el->rule == now->rule) return 1;
  return 0;
}


/* s[0..n) from pos with stack st (ours); the stack after it (ours) */
static Frame *tok_string (Grammar *base, const char *s, size_t n, int first, size_t pos, Frame *st, Out *o, int check_while) {
  size_t anchor = NONE;
  int loops = 0;
  char buf[512];
  g_text++;
  if (check_while) {
    st = check_whiles(base, s, n, &first, &pos, &anchor, st, o);
    g_text++;
  }
  Hit h;
  h.caps = NULL;
  for (;;) {
    size_t ms, me;
    int adv;
    if (++loops > 4000 || !scan_next(base, st, s, n, pos, anchor, first, &h)) {
      produce(o, st->content, n);
      break;
    }
    ms = h.caps[0];
    me = h.caps[1];
    adv = me > pos;
    if (h.rule == NULL) {	/* the end of the rule on top */
      Rule *popped = st->rule;
      Frame *top;
      size_t enter;
      produce(o, st->content, ms);
      st = fr_own(st);
      st->content = st->name;
      captures(base, s, n, first, st, popped->ecaps, popped->necaps, &h, o);
      produce(o, st->content, me);
      top = st;
      enter = enter_of(top);
      anchor = top->line == g_line ? top->anchor : NONE;
      if (!adv && enter == pos) {	/* pushed and popped without moving on: stop */
        produce(o, st->content, n);
        break;
      }
      st = fr_pop(top);
      if (st == NULL) break;
    }
    else {
      Rule *r = h.rule;
      Frame *before = st, *nf;
      Scope *nm;
      produce(o, st->content, ms);
      nm = scope_push(st->content, name_of(r->name, s, h.caps, h.ncap, buf, sizeof(buf)));
      nf = fr_push(st, r);	/* our reference to st is the new frame's */
      nf->line = g_line;
      nf->enter = pos;
      nf->anchor = anchor;
      nf->eol = me == n;
      nf->name = nm;
      nf->content = nm;
      st = nf;
      if (r->kind == RK_BEGIN_END || r->kind == RK_BEGIN_WHILE) {
        captures(base, s, n, first, st, r->bcaps, r->nbcaps, &h, o);
        produce(o, st->content, me);
        anchor = me;
        st->content = scope_push(nm, name_of(r->content, s, h.caps, h.ncap, buf, sizeof(buf)));
        if (r->end_back) st->end = end_with_back(r, s, h.caps, h.ncap);
        if (!adv && same_rule(before, st)) {	/* it would never end */
          st = fr_pop(st);
          produce(o, st->content, n);
          break;
        }
      }
      else {	/* a match: its scopes for its text, then off the stack */
        captures(base, s, n, first, st, r->caps, r->ncaps, &h, o);
        produce(o, st->content, me);
        st = fr_pop(st);
        if (!adv) {	/* no progress: stop the line */
          if (st->parent) st = fr_pop(st);
          produce(o, st->content, n);
          break;
        }
      }
    }
    if (me > pos) {
      pos = me;
      first = 0;
    }
  }
  hit_done(&h);
  g_text++;	/* what was cached for this string is not for the caller's */
  return st;
}


/* a whole line (its \n added, as VS Code does); st is kept, the stack after it returned */
static Frame *tok_line (Grammar *g, Frame *st, const char *s, size_t n, int first, Out *o) {
  static char *buf;
  static size_t cap;
  st->refs++;
  g_line++;
  if (n > MAX_LINE) {
    produce(o, st->content, n);
    return st;
  }
  if (n + 2 > cap) {
    cap = n + 256;
    buf = (char *)xrealloc(buf, cap);
  }
  memcpy(buf, s, n);
  buf[n] = '\n';
  buf[n + 1] = '\0';
  return tok_string(g, buf, n + 1, first, 0, st, o, 1);
}


static Frame *root_frame (Grammar *g) {
  Frame *f = fr_push(NULL, g->root);
  f->name = f->content = scope_push(NULL, g->scope);
  f->enter = f->anchor = NONE;
  return f;
}

/* }================================================================== */


/*
** {==================================================================
** A Doc's lines: the stacks they start with
** ===================================================================
*/

/*
** The lines tokenized lately, so that a line the screen draws, the brackets
** count and the colors ask for is only tokenized once. The line's number
** picks the slot; an edit drops the lines from it on, the ones above it
** stand.
*/
#define TC_N	128	/* slots: a screen and more */
#define TC_MAX	8192	/* a longer line is not kept */

typedef struct TCell {
  size_t y, len;
  unsigned long edits;
  int used;
  Out o;
  size_t cap;
} TCell;

typedef struct TDoc {
  const Syntax *sx;
  Grammar *g;
  Frame **st;	/* st[k]: the stack line k starts with */
  size_t n, cap;
  TCell c[TC_N];	/* the lines tokenized lately */
} TDoc;

static struct {	/* the last line tokenized for drawing */
  const Doc *d;
  size_t y, n, cap;
  unsigned long edits;
  Out o;
  struct TCell *hit;	/* the slot it is in, when it is kept */
} LAST;


static void td_clear (TDoc *td) {
  size_t k;
  for (k = 0; k < td->n; k++) fr_unref(td->st[k]);
  td->n = 0;
}


static void tc_free (TDoc *td) {
  int i;
  for (i = 0; i < TC_N; i++) {
    free(td->c[i].o.cls);
    free(td->c[i].o.fg);
    free(td->c[i].o.fs);
    free(td->c[i].o.sc);
    memset(&td->c[i], 0, sizeof(td->c[i]));
  }
}


/* the lines from y on are not what they were */
static void tc_drop (TDoc *td, size_t y) {
  int i;
  for (i = 0; i < TC_N; i++)
    if (td->c[i].used && td->c[i].y >= y) td->c[i].used = 0;
}


/* the slot of line y, when it holds that line as the text is now */
static TCell *tc_get (TDoc *td, const Doc *d, size_t y) {
  TCell *c = &td->c[y % TC_N];
  if (!c->used || c->y != y || c->edits != d->edits || c->len != d->row[y].len) return NULL;
  return c;
}


void tm_doc_free (Doc *d) {
  TDoc *td = (TDoc *)d->tm;
  if (td == NULL) return;
  td_clear(td);
  tc_free(td);
  free(td->st);
  free(td);
  d->tm = NULL;
  if (LAST.d == d) LAST.d = NULL;
}


static void out_size (Out *o, size_t n, size_t *cap) {
  if (n + 1 > *cap) {
    *cap = n + 256;
    o->cls = (unsigned char *)xrealloc(o->cls, *cap);
    o->fg = (uint32_t *)xrealloc(o->fg, *cap * sizeof(uint32_t));
    o->fs = (unsigned char *)xrealloc(o->fs, *cap);
    o->sc = (Scope **)xrealloc(o->sc, *cap * sizeof(Scope *));
  }
  o->n = n;
  o->last = 0;
}


/* the grammar of a language mme knows; NULL: esyntax does it */
static Grammar *grammar_for (const Syntax *sx) {
  if (!opt.textmate || sx == NULL) return NULL;
  return grammar_for_lang(syntax_lang(sx));
}


/*
** Line y of d, tokenized: tok gets its T_* classes. 0: no grammar, or the
** lines before it are still being tokenized (esyntax colors it meanwhile).
*/
int tm_line (Doc *d, const Syntax *sx, size_t y, unsigned char *tok) {
  static long long win, used;
  Grammar *g = grammar_for(sx);
  TDoc *td;
  const Row *r;
  Frame *after;
  long long t0;
  int count = 0;
  if (g == NULL || y >= d->n) return 0;
  theme_now();
  if ((td = (TDoc *)d->tm) == NULL) td = (TDoc *)(d->tm = calloc(1, sizeof(TDoc)));
  if (td->sx != sx || td->g != g) {
    td_clear(td);
    tc_free(td);
    td->sx = sx;
    td->g = g;
  }
  if (d->tm_from != (size_t)-1) tc_drop(td, d->tm_from);	/* an edit: its line and the ones after it */
  if (d->tm_from < td->n) {	/* an edit: the stacks after its line are made again */
    size_t k;
    for (k = d->tm_from + 1; k < td->n; k++) fr_unref(td->st[k]);
    td->n = d->tm_from + 1;
  }
  d->tm_from = (size_t)-1;
  {	/* tokenized already: that is all */
    TCell *c = tc_get(td, d, y);
    if (c) {
      memcpy(tok, c->o.cls, c->len);
      LAST.d = d;
      LAST.y = y;
      LAST.n = c->len;
      LAST.edits = d->edits;
      LAST.hit = c;
      return 1;
    }
  }
  if (td->n > d->n) {
    size_t k;
    for (k = d->n; k < td->n; k++) fr_unref(td->st[k]);
    td->n = d->n;
  }
  if (y + 2 > td->cap) {
    td->cap = td->cap ? td->cap : 1024;
    while (y + 2 > td->cap) td->cap *= 2;
    td->st = (Frame **)xrealloc(td->st, td->cap * sizeof(Frame *));
  }
  if (td->n == 0) {
    td->st[0] = root_frame(g);
    td->n = 1;
  }
  t0 = os_now_us();
  if (t0 - win > 150000) {	/* each 150 ms, 50 ms of catching up at most: the screen stays alive */
    win = t0;
    used = 0;
  }
  while (td->n <= y) {	/* the lines before, for their stacks */
    size_t k = td->n - 1;
    r = &d->row[k];
    td->st[k + 1] = tok_line(g, td->st[k], r->s, r->len, k == 0, NULL);
    td->n++;
    if (++count % 32 == 0 && used + (os_now_us() - t0) > 50000) {
      used += os_now_us() - t0;
      return 0;
    }
  }
  if (td->n == y + 1 && used + (os_now_us() - t0) > 50000) {	/* a new line when the time is spent: later */
    used += os_now_us() - t0;
    return 0;
  }
  r = &d->row[y];
  LAST.hit = NULL;
  if (r->len <= TC_MAX) {	/* into its slot, for whoever asks next */
    TCell *c = &td->c[y % TC_N];
    out_size(&c->o, r->len, &c->cap);
    after = tok_line(g, td->st[y], r->s, r->len, y == 0, &c->o);
    c->used = 1;
    c->y = y;
    c->len = r->len;
    c->edits = d->edits;
    LAST.hit = c;
    memcpy(tok, c->o.cls, r->len);
    if (td->n == y + 1) {	/* the frontier moved on: that counts */
      td->st[td->n++] = after;
      used += os_now_us() - t0;
    }
    else fr_unref(after);
    LAST.d = d;
    LAST.y = y;
    LAST.n = r->len;
    LAST.edits = d->edits;
    return 1;
  }
  out_size(&LAST.o, r->len, &LAST.cap);
  after = tok_line(g, td->st[y], r->s, r->len, y == 0, &LAST.o);
  if (td->n == y + 1) {	/* the frontier moved on: that counts */
    td->st[td->n++] = after;
    used += os_now_us() - t0;
  }
  else fr_unref(after);
  memcpy(tok, LAST.o.cls, r->len);
  LAST.d = d;
  LAST.y = y;
  LAST.n = r->len;
  LAST.edits = d->edits;
  return 1;
}


/* the theme's own colors of line y, when tm_line just did it: fg (0x1000000 | rgb, 0 none), its style, the classes */
int tm_colors (const Doc *d, size_t y, const uint32_t **fg, const unsigned char **fs, const unsigned char **cls) {
  const Out *o;
  if (LAST.d != d || LAST.y != y || LAST.edits != d->edits || !opt.textmate) return 0;
  o = LAST.hit ? &((TCell *)LAST.hit)->o : &LAST.o;
  *fg = o->fg;
  *fs = o->fs;
  *cls = o->cls;
  return 1;
}


/* Developer: Inspect Editor Tokens and Scopes: the scopes at x of line y, innermost last */
int tm_scopes (Doc *d, const Syntax *sx, size_t y, size_t x, char *out, size_t n) {
  unsigned char *tok;
  const Scope *s, *v[64];
  int k = 0;
  size_t o = 0;
  if (y >= d->n || x >= d->row[y].len) return 0;
  tok = (unsigned char *)xmalloc(d->row[y].len + 1);
  if (!tm_line(d, sx, y, tok)) {
    free(tok);
    return 0;
  }
  free(tok);
  {
    const Out *o = LAST.hit ? &((TCell *)LAST.hit)->o : &LAST.o;
    for (s = o->sc[x]; s && k < 64; s = s->parent) v[k++] = s;
  }
  out[0] = '\0';
  while (k-- > 0 && o + 2 < n) {
    int w = snprintf(out + o, n - o, "%s%s", o ? " " : "", g_atom[v[k]->atom]);
    if (w < 0) break;
    o += (size_t)w;
  }
  if (o >= n) out[n - 1] = '\0';
  return 1;
}


/* a file's language when mme has none but a VS Code extension has it and its grammar: a new Syntax */
const Syntax *tm_detect (const char *path, const Doc *d) {
  const char *base = path ? path_basename(path) : NULL;
  TLang *hit = NULL;
  size_t i, k, best = 0;
  if (!opt.textmate) return NULL;
  scan();
  for (i = 0; base && i < g_nlang && hit == NULL; i++)	/* its whole name */
    for (k = 0; k < g_lang[i].names.n; k++)
      if (m_fncmp(g_lang[i].names.v[k], base) == 0) hit = &g_lang[i];
  for (i = 0; base && hit == NULL && i < g_nlang; i++)	/* the longest ending: .d.ts before .ts */
    for (k = 0; k < g_lang[i].exts.n; k++) {
      const char *e = g_lang[i].exts.v[k];
      size_t el = strlen(e), bl = strlen(base);
      if (el > best && el <= bl && m_strnicmp(base + bl - el, e, el) == 0) {
        best = el;
        hit = &g_lang[i];
      }
    }
  if (hit == NULL && d && d->n > 0) {	/* firstLine */
    for (i = 0; i < g_nlang && hit == NULL; i++) {
      Onig *re;
      size_t caps[200];
      if (g_lang[i].first == NULL || (re = onig_compile(g_lang[i].first, NULL)) == NULL) continue;
      if (onig_groups(re) < 99 && onig_search(re, d->row[0].s, d->row[0].len, 0, (size_t)-1, 1, caps) && caps[0] == 0)
        hit = &g_lang[i];
      onig_free(re);
    }
  }
  if (hit == NULL || grammar_for_lang(hit->id) == NULL) return NULL;
  lang_config(hit);
  return syntax_extra(hit->alias ? hit->alias : hit->id, hit->id, hit->line, hit->open, hit->close);
}


/* where the grammars come from, for the readme and the About box */
const char *tm_vscode (void) {
  scan();
  return g_builtin;
}

/* }================================================================== */
