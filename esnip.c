/*
** esnip.c - snippets: VS Code's, from mme-data/snippets, and mme's own
**
** A language's snippets are mme-data/snippets/<language>.json (go.json,
** c.json ...) and every *.code-snippets there (with "scope": "c,cpp" or for
** all), written as VS Code writes them:
**   "For Loop": {"prefix": "for", "body": ["for ($1) {", "\t$0", "}"],
**                "description": "a for loop"}
** A body has tab stops ($1, ${1:default}, ${1|one,two|}, $0 the end) and
** variables ($TM_FILENAME, ${CURRENT_YEAR} ...); snip_expand makes the text.
** Extensions' snippet files come last (eext.c).
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/*
** {==================================================================
** mme's own
** ===================================================================
*/

typedef struct Builtin {
  const char *langs;	/* " c cpp " */
  const char *prefix, *body, *desc;
} Builtin;

static const Builtin builtin[] = {
  {" c cpp ", "for", "for (${1:size_t} ${2:i} = 0; $2 < ${3:n}; $2++) {\n\t$0\n}", "For Loop"},
  {" c cpp ", "if", "if (${1:condition}) {\n\t$0\n}", "If Statement"},
  {" c cpp ", "else", "else {\n\t$0\n}", "Else Statement"},
  {" c cpp ", "while", "while (${1:condition}) {\n\t$0\n}", "While Loop"},
  {" c cpp ", "do", "do {\n\t$0\n} while (${1:condition});", "Do-While Loop"},
  {" c cpp ", "switch", "switch (${1:expression}) {\n\tcase ${2:value}:\n\t\t$0\n\t\tbreak;\n\tdefault:\n\t\tbreak;\n}", "Switch Statement"},
  {" c cpp ", "struct", "typedef struct ${1:Name} {\n\t$0\n} $1;", "Typedef Struct"},
  {" c cpp ", "enum", "enum ${1:Name} {\n\t$0\n};", "Enum"},
  {" c cpp ", "main", "int main (int argc, char **argv) {\n\t$0\n\treturn 0;\n}", "Main Function"},
  {" c cpp ", "#include", "#include <${1:stdio.h}>", "Include"},
  {" c cpp ", "#ifndef", "#ifndef ${1:${TM_FILENAME_BASE/(.*)/${1:/upcase}/}_H}\n#define $1\n\n$0\n\n#endif", "Header Guard"},
  {" c cpp ", "printf", "printf(\"${1:%s}\\n\", $2);", "printf"},
  {" go ", "for", "for ${1:i} := 0; $1 < ${2:n}; $1++ {\n\t$0\n}", "For Loop"},
  {" go ", "forr", "for ${1:_}, ${2:v} := range ${3:list} {\n\t$0\n}", "For Range"},
  {" go ", "if", "if ${1:condition} {\n\t$0\n}", "If Statement"},
  {" go ", "iferr", "if err != nil {\n\treturn ${1:err}\n}", "If Error"},
  {" go ", "func", "func ${1:name}(${2}) ${3:error} {\n\t$0\n}", "Function"},
  {" go ", "meth", "func (${1:r} ${2:Type}) ${3:name}(${4}) ${5} {\n\t$0\n}", "Method"},
  {" go ", "main", "func main() {\n\t$0\n}", "Main Function"},
  {" go ", "struct", "type ${1:Name} struct {\n\t$0\n}", "Struct"},
  {" go ", "switch", "switch ${1:expression} {\ncase ${2:value}:\n\t$0\ndefault:\n}", "Switch"},
  {" go ", "pf", "fmt.Printf(\"${1:%v}\\n\", $2)", "fmt.Printf"},
  {" rust ", "fn", "fn ${1:name}(${2}) ${3:-> ${4:()}} {\n\t$0\n}", "Function"},
  {" rust ", "for", "for ${1:x} in ${2:iter} {\n\t$0\n}", "For Loop"},
  {" rust ", "if", "if ${1:condition} {\n\t$0\n}", "If"},
  {" rust ", "match", "match ${1:expression} {\n\t${2:pattern} => $0,\n}", "Match"},
  {" rust ", "struct", "struct ${1:Name} {\n\t$0\n}", "Struct"},
  {" rust ", "impl", "impl ${1:Type} {\n\t$0\n}", "Impl"},
  {" rust ", "test", "#[test]\nfn ${1:name}() {\n\t$0\n}", "Test Function"},
  {" rust ", "println", "println!(\"${1:{\\}}\", $2);", "println!"},
  {" python ", "def", "def ${1:name}(${2}):\n\t${0:pass}", "Function"},
  {" python ", "class", "class ${1:Name}:\n\tdef __init__(self${2}):\n\t\t${0:pass}", "Class"},
  {" python ", "if", "if ${1:condition}:\n\t${0:pass}", "If"},
  {" python ", "for", "for ${1:x} in ${2:items}:\n\t${0:pass}", "For Loop"},
  {" python ", "while", "while ${1:condition}:\n\t${0:pass}", "While Loop"},
  {" python ", "try", "try:\n\t${1:pass}\nexcept ${2:Exception} as ${3:e}:\n\t${0:raise}", "Try/Except"},
  {" python ", "main", "if __name__ == \"__main__\":\n\t${0:main()}", "Main Guard"},
  {" javascript typescript javascriptreact typescriptreact ", "for", "for (let ${1:i} = 0; $1 < ${2:array}.length; $1++) {\n\t$0\n}", "For Loop"},
  {" javascript typescript javascriptreact typescriptreact ", "forof", "for (const ${1:item} of ${2:items}) {\n\t$0\n}", "For-Of Loop"},
  {" javascript typescript javascriptreact typescriptreact ", "if", "if (${1:condition}) {\n\t$0\n}", "If Statement"},
  {" javascript typescript javascriptreact typescriptreact ", "function", "function ${1:name}(${2}) {\n\t$0\n}", "Function"},
  {" javascript typescript javascriptreact typescriptreact ", "log", "console.log($1);", "console.log"},
  {" javascript typescript javascriptreact typescriptreact ", "try", "try {\n\t$1\n} catch (${2:error}) {\n\t$0\n}", "Try-Catch"},
  {" lua ", "for", "for ${1:i} = ${2:1}, ${3:n} do\n\t$0\nend", "For Loop"},
  {" lua ", "fori", "for ${1:i}, ${2:v} in ipairs(${3:t}) do\n\t$0\nend", "For ipairs"},
  {" lua ", "forp", "for ${1:k}, ${2:v} in pairs(${3:t}) do\n\t$0\nend", "For pairs"},
  {" lua ", "if", "if ${1:condition} then\n\t$0\nend", "If"},
  {" lua ", "function", "function ${1:name}(${2})\n\t$0\nend", "Function"},
  {" lua ", "local", "local ${1:name} = ${0:value}", "Local"},
  {" zig ", "fn", "fn ${1:name}(${2}) ${3:void} {\n\t$0\n}", "Function"},
  {" zig ", "for", "for (${1:items}) |${2:item}| {\n\t$0\n}", "For Loop"},
  {" zig ", "if", "if (${1:condition}) {\n\t$0\n}", "If"},
  {" zig ", "test", "test \"${1:name}\" {\n\t$0\n}", "Test"},
  {" shellscript ", "if", "if [ ${1:condition} ]; then\n\t$0\nfi", "If"},
  {" shellscript ", "for", "for ${1:x} in ${2:list}; do\n\t$0\ndone", "For Loop"},
  {" shellscript ", "func", "${1:name}() {\n\t$0\n}", "Function"},
  {" markdown ", "code", "```${1:language}\n$0\n```", "Code Block"},
  {" markdown ", "link", "[${1:text}](${2:https://})", "Link"},
  {" html ", "html5", "<!DOCTYPE html>\n<html lang=\"${1:en}\">\n<head>\n\t<meta charset=\"UTF-8\">\n\t<title>${2:Document}</title>\n</head>\n<body>\n\t$0\n</body>\n</html>", "HTML5"}
};

#define NBUILTIN	(sizeof(builtin) / sizeof(builtin[0]))

/* }================================================================== */


/*
** {==================================================================
** The files
** ===================================================================
*/

static struct {
  char *lang;
  Snip *v;
  size_t n, cap;
} g_cache[16];

static int g_ncache;


/* VS Code's language id of a syntax's name: the name of its snippets file */
const char *snip_lang (const char *syntax) {
  const char *id = syntax_id(syntax);
  if (id && strcmp(id, "rc") == 0) id = "c";	/* resource scripts take C's snippets */
  return id ? id : "plaintext";
}


char *snip_dir (void) {
  char *d = data_path("snippets");
  mkdir_p(d);
  return d;
}


void snip_reload (void) {
  int i;
  size_t k;
  for (i = 0; i < g_ncache; i++) {
    for (k = 0; k < g_cache[i].n; k++) {
      free(g_cache[i].v[k].name);
      free(g_cache[i].v[k].prefix);
      free(g_cache[i].v[k].body);
      free(g_cache[i].v[k].desc);
    }
    free(g_cache[i].v);
    free(g_cache[i].lang);
  }
  g_ncache = 0;
}


static void add (int c, const char *name, const char *prefix, const char *body, const char *desc) {
  Snip *s;
  if (g_cache[c].n == g_cache[c].cap) {
    g_cache[c].cap = g_cache[c].cap ? g_cache[c].cap * 2 : 32;
    g_cache[c].v = (Snip *)xrealloc(g_cache[c].v, g_cache[c].cap * sizeof(Snip));
  }
  s = &g_cache[c].v[g_cache[c].n++];
  s->name = xstrdup(name);
  s->prefix = xstrdup(prefix);
  s->body = xstrdup(body);
  s->desc = xstrdup(desc ? desc : name);
}


/* is lang in "c,cpp" (a scope)? an empty scope is every language */
static int in_scope (const char *scope, const char *lang) {
  size_t n = strlen(lang);
  const char *p = scope;
  if (scope == NULL || *scope == '\0') return 1;
  while (*p) {
    while (*p == ',' || *p == ' ') p++;
    if (strncmp(p, lang, n) == 0 && (p[n] == '\0' || p[n] == ',' || p[n] == ' ')) return 1;
    while (*p && *p != ',') p++;
  }
  return 0;
}


/* one file's snippets: {"name": {"prefix", "body", "description", "scope"}} */
static void load_file (int c, const char *path, const char *lang, int scoped) {
  size_t len, i, k;
  char *s = read_file(path, &len);
  Json *j;
  if (s == NULL) return;
  j = json_parse(s, len);
  free(s);
  if (j == NULL || j->type != J_OBJ) {
    if (j == NULL) toast(1, "%s is not valid JSON", path_basename(path));
    json_free(j);
    return;
  }
  for (i = 0; i < j->n; i++) {
    const Json *e = j->kid[i], *pre = json_get(e, "prefix"), *body = json_get(e, "body");
    const char *desc = json_str(json_get(e, "description"), NULL);
    Buf b;
    if (pre == NULL || body == NULL) continue;
    if (scoped && !in_scope(json_str(json_get(e, "scope"), NULL), lang)) continue;
    buf_init(&b);
    if (body->type == J_ARR)	/* the lines */
      for (k = 0; k < body->n; k++) {
        if (k) buf_putc(&b, '\n');
        buf_puts(&b, json_str(body->kid[k], ""));
      }
    else buf_puts(&b, json_str(body, ""));
    buf_putc(&b, '\0');
    if (pre->type == J_ARR) {
      for (k = 0; k < pre->n; k++)
        if (pre->kid[k]->type == J_STR) add(c, e->key, pre->kid[k]->str, b.s, desc);
    }
    else if (pre->type == J_STR) add(c, e->key, pre->str, b.s, desc);
    buf_free(&b);
  }
  json_free(j);
}


/* lang's snippets ("go", "c" ...: the protocol's language ids) */
const Snip *snip_list (const char *lang, size_t *n) {
  int c;
  size_t i;
  char *dir, *f, name[80], key[40];
  Vec ls;
  if (lang == NULL) lang = "plaintext";
  for (c = 0; c < g_ncache; c++)
    if (strcmp(g_cache[c].lang, lang) == 0) {
      *n = g_cache[c].n;
      return g_cache[c].v;
    }
  if (g_ncache == (int)(sizeof(g_cache) / sizeof(g_cache[0]))) snip_reload();
  c = g_ncache++;
  memset(&g_cache[c], 0, sizeof(g_cache[c]));
  g_cache[c].lang = xstrdup(lang);
  snprintf(key, sizeof(key), " %s ", lang);
  for (i = 0; i < NBUILTIN; i++)
    if (strstr(builtin[i].langs, key)) add(c, builtin[i].desc, builtin[i].prefix, builtin[i].body, builtin[i].desc);
  dir = snip_dir();
  if (dir) {
    snprintf(name, sizeof(name), "%s.json", lang);
    f = path_join(dir, name);
    load_file(c, f, lang, 0);
    free(f);
    vec_init(&ls);
    os_listdir(dir, &ls);
    vec_sort(&ls);
    for (i = 0; i < ls.n; i++) {	/* the global ones */
      size_t el = strlen(ls.v[i]);
      if (el > 14 && strcmp(ls.v[i] + el - 14, ".code-snippets") == 0) {
        f = path_join(dir, ls.v[i]);
        load_file(c, f, lang, 1);
        free(f);
      }
    }
    vec_free(&ls);
    free(dir);
  }
  {	/* the extensions' */
    size_t ne;
    const char *const *ep = ext_snippets(lang, &ne);
    for (i = 0; i < ne; i++) {
      size_t pl = strlen(ep[i]);
      load_file(c, ep[i], lang, pl > 14 && strcmp(ep[i] + pl - 14, ".code-snippets") == 0);
    }
  }
  *n = g_cache[c].n;
  return g_cache[c].v;
}

/* }================================================================== */


/*
** {==================================================================
** A body made text
** ===================================================================
*/

typedef struct Ex {
  const SnipCtx *ctx;
  Buf out;
  SnipStop *stop;
  size_t nstop, cap;
  char *def[100];	/* a tab stop's placeholder: its mirrors ($2 after ${2:i}) show it too */
} Ex;




static void emit (Ex *x, const char *s, size_t n) {	/* new lines take the line's indent */
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] == '\n') {
      buf_putc(&x->out, '\n');
      buf_puts(&x->out, x->ctx->indent);
    }
    else if (s[i] == '\t') buf_puts(&x->out, x->ctx->tab);
    else buf_putc(&x->out, s[i]);
  }
}


static void add_stop (Ex *x, int idx, size_t a, size_t b) {
  if (x->nstop == x->cap) {
    x->cap = x->cap ? x->cap * 2 : 16;
    x->stop = (SnipStop *)xrealloc(x->stop, x->cap * sizeof(SnipStop));
  }
  x->stop[x->nstop].idx = idx;
  x->stop[x->nstop].a = a;
  x->stop[x->nstop].b = b;
  x->stop[x->nstop].choices = NULL;
  x->nstop++;
}


/* a bare $n (or ${n}): the placeholder's text when there is one */
static void mirror (Ex *x, int idx) {
  size_t a = x->out.len;
  if (idx < 100 && x->def[idx]) buf_puts(&x->out, x->def[idx]);
  add_stop(x, idx, a, x->out.len);
}


static int is_var (int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}


/* a variable's value; NULL: not known */
static char *var_value (const SnipCtx *c, const char *name) {
  char b[64];
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
  static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  if (strcmp(name, "TM_SELECTED_TEXT") == 0) return xstrdup(c->selected ? c->selected : "");
  if (strcmp(name, "TM_CURRENT_LINE") == 0) return xstrdup(c->line ? c->line : "");
  if (strcmp(name, "TM_CURRENT_WORD") == 0) return xstrdup(c->word ? c->word : "");
  if (strcmp(name, "TM_LINE_INDEX") == 0 || strcmp(name, "TM_LINE_NUMBER") == 0) {
    snprintf(b, sizeof(b), "%lu", (unsigned long)(c->line_no + (name[8] == 'N' ? 1 : 0)));
    return xstrdup(b);
  }
  if (strcmp(name, "TM_FILENAME") == 0) return xstrdup(c->path ? path_basename(c->path) : "Untitled-1");
  if (strcmp(name, "TM_FILENAME_BASE") == 0) {
    char *s = xstrdup(c->path ? path_basename(c->path) : "Untitled-1"), *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = '\0';
    return s;
  }
  if (strcmp(name, "TM_DIRECTORY") == 0) return c->path ? path_dirname(c->path) : xstrdup("");
  if (strcmp(name, "TM_FILEPATH") == 0) return xstrdup(c->path ? c->path : "");
  if (strcmp(name, "CLIPBOARD") == 0) return xstrdup(c->clipboard ? c->clipboard : "");
  if (strcmp(name, "LINE_COMMENT") == 0) return xstrdup(c->line_comment ? c->line_comment : "//");
  if (strcmp(name, "BLOCK_COMMENT_START") == 0) return xstrdup(c->block_open ? c->block_open : "/*");
  if (strcmp(name, "BLOCK_COMMENT_END") == 0) return xstrdup(c->block_close ? c->block_close : "*/");
  if (tm == NULL) return NULL;
  if (strcmp(name, "CURRENT_YEAR") == 0) snprintf(b, sizeof(b), "%d", tm->tm_year + 1900);
  else if (strcmp(name, "CURRENT_YEAR_SHORT") == 0) snprintf(b, sizeof(b), "%02d", tm->tm_year % 100);
  else if (strcmp(name, "CURRENT_MONTH") == 0) snprintf(b, sizeof(b), "%02d", tm->tm_mon + 1);
  else if (strcmp(name, "CURRENT_MONTH_NAME") == 0) snprintf(b, sizeof(b), "%s", months[tm->tm_mon]);
  else if (strcmp(name, "CURRENT_MONTH_NAME_SHORT") == 0) snprintf(b, sizeof(b), "%.3s", months[tm->tm_mon]);
  else if (strcmp(name, "CURRENT_DATE") == 0) snprintf(b, sizeof(b), "%02d", tm->tm_mday);
  else if (strcmp(name, "CURRENT_DAY_NAME") == 0) snprintf(b, sizeof(b), "%s", days[tm->tm_wday]);
  else if (strcmp(name, "CURRENT_DAY_NAME_SHORT") == 0) snprintf(b, sizeof(b), "%.3s", days[tm->tm_wday]);
  else if (strcmp(name, "CURRENT_HOUR") == 0) snprintf(b, sizeof(b), "%02d", tm->tm_hour);
  else if (strcmp(name, "CURRENT_MINUTE") == 0) snprintf(b, sizeof(b), "%02d", tm->tm_min);
  else if (strcmp(name, "CURRENT_SECOND") == 0) snprintf(b, sizeof(b), "%02d", tm->tm_sec);
  else if (strcmp(name, "CURRENT_SECONDS_UNIX") == 0) snprintf(b, sizeof(b), "%lld", (long long)now);
  else if (strcmp(name, "RANDOM") == 0) snprintf(b, sizeof(b), "%06d", rand() % 1000000);
  else if (strcmp(name, "RANDOM_HEX") == 0) snprintf(b, sizeof(b), "%06x", rand() % 0x1000000);
  else return NULL;
  return xstrdup(b);
}


/*
** ${VAR/(.*)/${1:/upcase}/}: of a transform only /upcase, /downcase and
** /capitalize are done, to the whole value (*mode 1, 2, 3); s is at the
** first '/', the result is after the last one
*/
static const char *transform (const char *s, int *mode) {
  int slashes = 0;
  *mode = 0;
  while (*s && slashes < 3) {
    if (*s == '\\' && s[1]) s++;
    else if (*s == '/') slashes++;
    else if (*s == '$' && s[1] == '{') {	/* a format inside */
      int depth = 0;
      const char *f = s;
      for (; *s; s++) {
        if (*s == '{') depth++;
        else if (*s == '}' && --depth == 0) break;
      }
      if (*s == '\0') return s;
      if (strstr(f, "/upcase") && strstr(f, "/upcase") < s) *mode = 1;
      else if (strstr(f, "/downcase") && strstr(f, "/downcase") < s) *mode = 2;
      else if (strstr(f, "/capitalize") && strstr(f, "/capitalize") < s) *mode = 3;
    }
    s++;
  }
  return s;
}


static void apply_case (char *v, int mode) {
  for (; v && *v; v++) {
    if (mode == 1 || mode == 3) {
      if (*v >= 'a' && *v <= 'z') *v = (char)(*v - 32);
      if (mode == 3) return;
    }
    else if (mode == 2 && *v >= 'A' && *v <= 'Z') *v = (char)(*v + 32);
  }
}


/* the body from s until a '}' (inside a placeholder) or its end; returns where it stopped */
static const char *expand (Ex *x, const char *s, int inside) {
  while (*s) {
    if (*s == '\\' && (s[1] == '$' || s[1] == '}' || s[1] == '\\' || s[1] == ',' || s[1] == '|')) {
      emit(x, s + 1, 1);
      s += 2;
      continue;
    }
    if (inside && *s == '}') return s;
    if (*s == '$' && s[1] >= '0' && s[1] <= '9') {	/* $1 */
      int idx = 0;
      s++;
      for (; *s >= '0' && *s <= '9'; s++)
        if (idx < 100000) idx = idx * 10 + (*s - '0');	/* $99999999999: no overflow */
      mirror(x, idx);
      continue;
    }
    if (*s == '$' && s[1] == '{' && s[2] >= '0' && s[2] <= '9') {	/* ${1}, ${1:...}, ${1|a,b|} */
      int idx = 0;
      size_t a = x->out.len, at;
      s += 2;
      for (; *s >= '0' && *s <= '9'; s++)
        if (idx < 100000) idx = idx * 10 + (*s - '0');	/* $99999999999: no overflow */
      if (*s == '}') {	/* ${1} */
        mirror(x, idx);
        s++;
        continue;
      }
      at = x->nstop;
      add_stop(x, idx, a, a);
      if (*s == ':') s = expand(x, s + 1, 1);
      else if (*s == '|') {	/* a choice: the first one; all of them kept for a list */
        const char *p = ++s;
        int first = 1;
        Buf ch;
        buf_init(&ch);
        while (*p && !(*p == '|' && p[1] == '}')) {
          if (*p == '\\' && p[1]) {
            if (first) emit(x, p + 1, 1);
            buf_putc(&ch, p[1]);
            p += 2;
            continue;
          }
          if (*p == ',') {
            first = 0;
            buf_putc(&ch, '\n');
          }
          else {
            if (first) emit(x, p, 1);
            buf_putc(&ch, *p);
          }
          p++;
        }
        buf_putc(&ch, '\0');
        free(x->stop[at].choices);
        x->stop[at].choices = buf_take(&ch);
        s = *p ? p + 1 : p;
      }
      else if (*s == '/') {
        int mode;
        s = transform(s, &mode);
      }
      x->stop[at].b = x->out.len;
      if (idx < 100 && x->def[idx] == NULL && x->out.len > a) {	/* the first placeholder of it */
        x->def[idx] = (char *)xmalloc(x->out.len - a + 1);
        memcpy(x->def[idx], x->out.s + a, x->out.len - a);
        x->def[idx][x->out.len - a] = '\0';
      }
      if (*s == '}') s++;
      continue;
    }
    if (*s == '$' && (s[1] == '{' || is_var((unsigned char)s[1])) && !(s[1] >= '0' && s[1] <= '9')) {	/* a variable */
      char name[64];
      size_t n = 0;
      int braced = s[1] == '{';
      char *v;
      s += braced ? 2 : 1;
      while (is_var((unsigned char)*s) && n + 1 < sizeof(name)) name[n++] = *s++;
      name[n] = '\0';
      v = var_value(x->ctx, name);
      if (braced && *s == '/') {
        int mode;
        s = transform(s, &mode);
        apply_case(v, mode);
      }
      if (v) emit(x, v, strlen(v));
      if (braced) {
        if (*s == ':') {	/* ${VAR:default} */
          if (v == NULL || *v == '\0') s = expand(x, s + 1, 1);
          else {
            Ex skip = *x;	/* expanded aside, then dropped */
            int i;
            buf_init(&skip.out);
            skip.stop = NULL;
            skip.nstop = skip.cap = 0;
            s = expand(&skip, s + 1, 1);
            buf_free(&skip.out);
            while (skip.nstop > 0) free(skip.stop[--skip.nstop].choices);
            free(skip.stop);
            for (i = 0; i < 100; i++)	/* what it learnt, kept */
              if (skip.def[i] != x->def[i]) {
                if (x->def[i] == NULL) x->def[i] = skip.def[i];
                else free(skip.def[i]);
              }
          }
        }
        else if (v == NULL) emit(x, name, n);	/* VS Code: an unknown one is its name */
        if (*s == '}') s++;
      }
      else if (v == NULL) emit(x, name, n);
      free(v);
      continue;
    }
    emit(x, s, 1);
    s++;
  }
  return s;
}


/* the text of body, and where its tab stops are in it (byte offsets) */
char *snip_expand (const char *body, const SnipCtx *ctx, SnipStop **stop, size_t *nstop, size_t *len) {
  Ex x;
  int i;
  memset(&x, 0, sizeof(x));
  x.ctx = ctx;
  buf_init(&x.out);
  expand(&x, body, 0);	/* once for the placeholders, as a mirror can come first */
  buf_free(&x.out);
  while (x.nstop > 0) free(x.stop[--x.nstop].choices);
  free(x.stop);
  x.stop = NULL;
  x.nstop = x.cap = 0;
  buf_init(&x.out);
  expand(&x, body, 0);
  for (i = 0; i < 100; i++) free(x.def[i]);
  buf_putc(&x.out, '\0');
  *len = x.out.len - 1;
  *stop = x.stop;
  *nstop = x.nstop;
  return buf_take(&x.out);
}

/* }================================================================== */
