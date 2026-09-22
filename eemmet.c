/*
** eemmet.c - Emmet abbreviations, as VS Code expands them
**
** HTML: "div.box#main>ul>li*3>a[href=#]{Item $}" becomes the elements,
** with > for a child, + a sibling, ^ one level up, () groups, *n
** repeats, $ the number, [attr=value], {text}, and VS Code's aliases
** ("!", "link:css", "input:text" ...). CSS: "m10" becomes "margin: 10px;",
** "p10-20", "w100p", "df" ... The result is a snippet's body: its empty
** attributes and contents are tab stops ($1, $2 ...).
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** HTML
** ===================================================================
*/

typedef struct Attr {
  char *name, *value;	/* value NULL: none given (a tab stop) */
} Attr;

typedef struct ENode {
  char *name;	/* NULL: a group ( ... ) */
  char *id, *text;
  Buf cls;	/* "a b" */
  Attr *attr;
  int nattr, capattr;
  int mul;	/* *n */
  int *kid;
  int nkid, capkid;
  int parent;
} ENode;

typedef struct Parser {
  const char *s, *e;
  ENode *v;
  int n, cap;
  int bad;
} Parser;


static int new_node (Parser *p, const char *name, size_t len) {
  ENode *nd;
  if (p->n == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 16;
    p->v = (ENode *)xrealloc(p->v, (size_t)p->cap * sizeof(ENode));
  }
  nd = &p->v[p->n];
  memset(nd, 0, sizeof(*nd));
  buf_init(&nd->cls);
  if (name) {
    nd->name = (char *)xmalloc(len + 1);
    memcpy(nd->name, name, len);
    nd->name[len] = '\0';
  }
  nd->mul = 1;
  nd->parent = -1;
  return p->n++;
}


static void add_kid (Parser *p, int parent, int kid) {
  ENode *nd = &p->v[parent];
  if (nd->nkid == nd->capkid) {
    nd->capkid = nd->capkid ? nd->capkid * 2 : 4;
    nd->kid = (int *)xrealloc(nd->kid, (size_t)nd->capkid * sizeof(int));
  }
  nd->kid[nd->nkid++] = kid;
  p->v[kid].parent = parent;
}


static void add_attr (ENode *nd, const char *name, size_t nlen, const char *value, size_t vlen, int has) {
  Attr *a;
  int i;
  char *nm = (char *)xmalloc(nlen + 1);
  memcpy(nm, name, nlen);
  nm[nlen] = '\0';
  for (i = 0; i < nd->nattr; i++)	/* given again: the new value */
    if (strcmp(nd->attr[i].name, nm) == 0) {
      free(nm);
      if (has) {
        free(nd->attr[i].value);
        nd->attr[i].value = (char *)xmalloc(vlen + 1);
        memcpy(nd->attr[i].value, value, vlen);
        nd->attr[i].value[vlen] = '\0';
      }
      return;
    }
  if (nd->nattr == nd->capattr) {
    nd->capattr = nd->capattr ? nd->capattr * 2 : 4;
    nd->attr = (Attr *)xrealloc(nd->attr, (size_t)nd->capattr * sizeof(Attr));
  }
  a = &nd->attr[nd->nattr++];
  a->name = nm;
  a->value = NULL;
  if (has) {
    a->value = (char *)xmalloc(vlen + 1);
    memcpy(a->value, value, vlen);
    a->value[vlen] = '\0';
  }
}


static int name_char (int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == ':' || c == '-' || c == '_' || c == '!' || c == '$' || c == '@';
}


/* up to one of the characters in stop (or the end) */
static const char *until (const char *s, const char *e, const char *stop) {
  while (s < e && !strchr(stop, *s)) s++;
  return s;
}


/* [a b=c d="e f"] */
static void parse_attrs (Parser *p, ENode *nd) {
  const char *s = p->s + 1;
  while (s < p->e && *s != ']') {
    const char *n0, *v0;
    size_t nlen, vlen = 0;
    int has = 0;
    while (s < p->e && *s == ' ') s++;
    if (s >= p->e || *s == ']') break;
    n0 = s;
    while (s < p->e && *s != '=' && *s != ' ' && *s != ']') s++;
    nlen = (size_t)(s - n0);
    v0 = s;
    if (s < p->e && *s == '=') {
      has = 1;
      s++;
      if (s < p->e && (*s == '"' || *s == '\'')) {
        char q = *s++;
        v0 = s;
        while (s < p->e && *s != q) s++;
        vlen = (size_t)(s - v0);
        if (s < p->e) s++;
      }
      else {
        v0 = s;
        while (s < p->e && *s != ' ' && *s != ']') s++;
        vlen = (size_t)(s - v0);
      }
    }
    if (nlen) add_attr(nd, n0, nlen, v0, vlen, has);
  }
  if (s >= p->e) p->bad = 1;
  p->s = s < p->e ? s + 1 : s;
}


static int parse_list (Parser *p, int close);

/* one element, or a group; its index */
static int parse_item (Parser *p) {
  int k;
  ENode *nd;
  if (p->s < p->e && *p->s == '(') {
    int inner;
    p->s++;
    k = new_node(p, NULL, 0);
    inner = parse_list(p, ')');
    if (p->s >= p->e || *p->s != ')') {
      p->bad = 1;
      return k;
    }
    p->s++;
    {	/* the group takes the list's items */
      int i;
      for (i = 0; i < p->v[inner].nkid; i++) add_kid(p, k, p->v[inner].kid[i]);
    }
  }
  else {
    const char *n0 = p->s;
    while (p->s < p->e && name_char((unsigned char)*p->s)) p->s++;
    k = new_node(p, n0, (size_t)(p->s - n0));
    for (;;) {
      const char *v0;
      nd = &p->v[k];
      if (p->s >= p->e) break;
      if (*p->s == '#') {
        v0 = ++p->s;
        p->s = until(p->s, p->e, ".#[{*>+^()");
        free(nd->id);
        nd->id = (char *)xmalloc((size_t)(p->s - v0) + 1);
        memcpy(nd->id, v0, (size_t)(p->s - v0));
        nd->id[p->s - v0] = '\0';
      }
      else if (*p->s == '.') {
        v0 = ++p->s;
        p->s = until(p->s, p->e, ".#[{*>+^()");
        if (nd->cls.len) buf_putc(&nd->cls, ' ');
        buf_putn(&nd->cls, v0, (size_t)(p->s - v0));
      }
      else if (*p->s == '[') parse_attrs(p, nd);
      else if (*p->s == '{') {	/* the text, with \} and nested {} */
        int depth = 0;
        Buf t;
        buf_init(&t);
        p->s++;
        while (p->s < p->e) {
          if (*p->s == '\\' && p->s + 1 < p->e) {
            buf_putc(&t, p->s[1]);
            p->s += 2;
            continue;
          }
          if (*p->s == '{') depth++;
          if (*p->s == '}' && depth-- == 0) break;
          buf_putc(&t, *p->s++);
        }
        if (p->s >= p->e) p->bad = 1;
        else p->s++;
        buf_putc(&t, '\0');
        free(nd->text);
        nd->text = buf_take(&t);
      }
      else break;
    }
  }
  nd = &p->v[k];
  if (p->s < p->e && *p->s == '*') {
    int m = 0;
    p->s++;
    for (; p->s < p->e && *p->s >= '0' && *p->s <= '9'; p->s++)
      if (m < 100000) m = m * 10 + (*p->s - '0');	/* li*99999999999: no overflow */
    nd->mul = m > 0 ? (m > 1000 ? 1000 : m) : 1;
  }
  if (nd->name && nd->name[0] == '\0' && !nd->id && !nd->cls.len && !nd->nattr && !nd->text)
    p->bad = 1;	/* nothing at all */
  return k;
}


/* items joined by > + ^ until close (or the end); a group node that holds them */
static int parse_list (Parser *p, int close) {
  int root = new_node(p, NULL, 0), parent = root, last = -1;
  while (p->s < p->e && *p->s != close && !p->bad) {
    int it = parse_item(p);
    add_kid(p, parent, it);
    last = it;
    if (p->s >= p->e || *p->s == close) break;
    if (*p->s == '>') {
      p->s++;
      parent = last;
    }
    else if (*p->s == '+') p->s++;
    else if (*p->s == '^') {
      while (p->s < p->e && *p->s == '^') {
        p->s++;
        if (p->v[parent].parent >= 0 && parent != root) parent = p->v[parent].parent;
      }
    }
    else {
      p->bad = 1;
      break;
    }
  }
  if (last < 0) p->bad = 1;
  return root;
}


static void free_nodes (Parser *p) {
  int i, j;
  for (i = 0; i < p->n; i++) {
    ENode *nd = &p->v[i];
    free(nd->name);
    free(nd->id);
    free(nd->text);
    buf_free(&nd->cls);
    for (j = 0; j < nd->nattr; j++) {
      free(nd->attr[j].name);
      free(nd->attr[j].value);
    }
    free(nd->attr);
    free(nd->kid);
  }
  free(p->v);
}


/* VS Code's aliases: the tag and its usual attributes */
static const struct {
  const char *abbr, *tag, *attrs;
} alias[] = {
  {"a", "a", "href"}, {"a:link", "a", "href=http://"}, {"a:mail", "a", "href=mailto:"},
  {"a:tel", "a", "href=tel:+"}, {"abbr", "abbr", "title"}, {"base", "base", "href"},
  {"bdo", "bdo", "dir"}, {"bdo:r", "bdo", "dir=rtl"}, {"bdo:l", "bdo", "dir=ltr"},
  {"link", "link", "rel=stylesheet href"}, {"link:css", "link", "rel=stylesheet href=style.css"},
  {"link:favicon", "link", "rel=icon type=image/x-icon href=favicon.ico"},
  {"link:print", "link", "rel=stylesheet href=print.css media=print"},
  {"meta", "meta", ""}, {"meta:utf", "meta", "http-equiv=Content-Type content=text/html;charset=UTF-8"},
  {"meta:vp", "meta", "name=viewport content=width=device-width,\\ initial-scale=1.0"},
  {"meta:compat", "meta", "http-equiv=X-UA-Compatible content=IE=edge"},
  {"script", "script", ""}, {"script:src", "script", "src"}, {"img", "img", "src alt"},
  {"iframe", "iframe", "src frameborder=0"}, {"ifr", "iframe", "src frameborder=0"},
  {"embed", "embed", "src type"}, {"emb", "embed", "src type"},
  {"object", "object", "data type"}, {"obj", "object", "data type"},
  {"area", "area", "shape coords href alt"}, {"form", "form", "action"},
  {"form:get", "form", "action method=get"}, {"form:post", "form", "action method=post"},
  {"label", "label", "for"}, {"input", "input", "type=text"},
  {"inp", "input", "type=text name id"}, {"input:hidden", "input", "type=hidden name"},
  {"input:h", "input", "type=hidden name"}, {"input:text", "input", "type=text name id"},
  {"input:t", "input", "type=text name id"}, {"input:search", "input", "type=search name id"},
  {"input:email", "input", "type=email name id"}, {"input:url", "input", "type=url name id"},
  {"input:password", "input", "type=password name id"}, {"input:p", "input", "type=password name id"},
  {"input:number", "input", "type=number name id"}, {"input:date", "input", "type=date name id"},
  {"input:checkbox", "input", "type=checkbox name id"}, {"input:c", "input", "type=checkbox name id"},
  {"input:radio", "input", "type=radio name id"}, {"input:r", "input", "type=radio name id"},
  {"input:file", "input", "type=file name id"}, {"input:f", "input", "type=file name id"},
  {"input:submit", "input", "type=submit value"}, {"input:s", "input", "type=submit value"},
  {"input:button", "input", "type=button value"}, {"input:b", "input", "type=button value"},
  {"input:reset", "input", "type=reset value"}, {"input:range", "input", "type=range name id"},
  {"input:color", "input", "type=color name id"}, {"select", "select", "name id"},
  {"option", "option", "value"}, {"opt", "option", "value"}, {"optg", "optgroup", ""},
  {"textarea", "textarea", "name id cols=30 rows=10"}, {"tarea", "textarea", "name id cols=30 rows=10"},
  {"video", "video", "src"}, {"audio", "audio", "src"}, {"source", "source", "src type"},
  {"src", "source", "src type"}, {"btn", "button", ""}, {"btn:s", "button", "type=submit"},
  {"btn:r", "button", "type=reset"}, {"btn:b", "button", "type=button"},
  {"bq", "blockquote", ""}, {"hdr", "header", ""}, {"ftr", "footer", ""}, {"adr", "address", ""},
  {"dlg", "dialog", ""}, {"str", "strong", ""}, {"prog", "progress", ""}, {"fset", "fieldset", ""},
  {"fst", "fieldset", ""}, {"leg", "legend", ""}, {"sect", "section", ""}, {"art", "article", ""},
  {"tem", "template", ""}, {"fig", "figure", ""}, {"figc", "figcaption", ""},
  {"pic", "picture", ""}, {"cap", "caption", ""}, {"colg", "colgroup", ""}, {"mn", "main", ""},
  {"out", "output", ""}, {"det", "details", ""}, {"sum", "summary", ""}, {"datag", "datagrid", ""},
  {"datal", "datalist", ""}, {"kg", "keygen", ""}, {"ri:d", "img", "srcset alt"},
  {"html", "html", "lang=en"}, {"style", "style", ""}, {"map", "map", "name"}
};

static const char *const html_tags[] = {
  "a", "abbr", "address", "area", "article", "aside", "audio", "b", "base", "bdi", "bdo",
  "blockquote", "body", "br", "button", "canvas", "caption", "cite", "code", "col",
  "colgroup", "data", "datalist", "dd", "del", "details", "dfn", "dialog", "div", "dl",
  "dt", "em", "embed", "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2",
  "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "i", "iframe", "img",
  "input", "ins", "kbd", "label", "legend", "li", "link", "main", "map", "mark", "menu",
  "meta", "meter", "nav", "noscript", "object", "ol", "optgroup", "option", "output", "p",
  "param", "picture", "pre", "progress", "q", "rp", "rt", "ruby", "s", "samp", "script",
  "search", "section", "select", "slot", "small", "source", "span", "strong", "style",
  "sub", "summary", "sup", "table", "tbody", "td", "template", "textarea", "tfoot", "th",
  "thead", "time", "title", "tr", "track", "u", "ul", "var", "video", "wbr", "svg", NULL
};

static const char *const void_tags[] = {
  "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
  "source", "track", "wbr", "keygen", NULL
};

static const char *const inline_tags[] = {
  "a", "abbr", "acronym", "b", "bdi", "bdo", "big", "br", "button", "cite", "code", "data",
  "del", "dfn", "em", "font", "i", "img", "input", "ins", "kbd", "label", "map", "mark",
  "meter", "object", "output", "picture", "progress", "q", "s", "samp", "select", "small",
  "span", "strike", "strong", "sub", "sup", "textarea", "time", "tt", "u", "var", "wbr", NULL
};


static int in (const char *const *list, const char *w) {
  for (; *list; list++)
    if (strcmp(*list, w) == 0) return 1;
  return 0;
}


#define EMMET_MAX	(256 << 10)	/* an expansion longer than this is not made */

typedef struct Out {
  Buf b;
  int stop;	/* the last tab stop number */
  int mode;	/* EMMET_HTML, EMMET_JSX, EMMET_XML */
} Out;


/* text for a snippet body: $ } \ escaped; $$$ numbers were already done */
static void put_lit (Out *o, const char *s) {
  for (; *s; s++) {
    if (*s == '$' || *s == '}' || *s == '\\') buf_putc(&o->b, '\\');
    buf_putc(&o->b, *s);
  }
}


/* s with its $ (and $$$, $@-, $@3) numbered: item i of n (from 1) */
static char *number (const char *s, int i, int n) {
  Buf b;
  buf_init(&b);
  while (*s) {
    if (*s == '$') {
      int w = 0, rev = 0, base = 1, v;
      char f[16];
      while (*s == '$') {
        w++;
        s++;
      }
      if (*s == '@') {
        s++;
        if (*s == '-') {
          rev = 1;
          s++;
        }
        if (*s >= '0' && *s <= '9') {
          base = 0;
          for (; *s >= '0' && *s <= '9'; s++)
            if (base < 100000) base = base * 10 + (*s - '0');
        }
      }
      v = (rev ? n - i + 1 : i) + base - 1;
      snprintf(f, sizeof(f), "%0*d", w > 9 ? 9 : w, v);
      buf_puts(&b, f);
    }
    else buf_putc(&b, *s++);
  }
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/* what an element without a name is, from its parent: li in ul, td in tr ... */
static const char *implicit (const char *parent) {
  if (parent == NULL) return "div";
  if (strcmp(parent, "ul") == 0 || strcmp(parent, "ol") == 0 || strcmp(parent, "menu") == 0) return "li";
  if (strcmp(parent, "table") == 0 || strcmp(parent, "tbody") == 0 || strcmp(parent, "thead") == 0 ||
      strcmp(parent, "tfoot") == 0)
    return "tr";
  if (strcmp(parent, "tr") == 0) return "td";
  if (strcmp(parent, "select") == 0 || strcmp(parent, "optgroup") == 0) return "option";
  if (strcmp(parent, "dl") == 0) return "dt";
  if (strcmp(parent, "audio") == 0 || strcmp(parent, "video") == 0 || strcmp(parent, "picture") == 0)
    return "source";
  if (in(inline_tags, parent)) return "span";
  return "div";
}


static void indent (Out *o, int depth) {
  int i;
  for (i = 0; i < depth; i++) buf_putc(&o->b, '\t');
}


static int is_block (Parser *p, int k, const char *parent);

/* the expanded items of node k's children (groups flattened), for layout */
static int kids_block (Parser *p, int k, const char *tag) {
  int i;
  for (i = 0; i < p->v[k].nkid; i++)
    if (is_block(p, p->v[k].kid[i], tag)) return 1;
  return 0;
}


static int is_block (Parser *p, int k, const char *parent) {
  const ENode *nd = &p->v[k];
  const char *tag;
  if (nd->name == NULL) return kids_block(p, k, parent) || nd->mul > 1;
  tag = nd->name[0] ? nd->name : implicit(parent);
  if (strchr(tag, ':')) return 0;
  return !in(inline_tags, tag) || nd->mul > 1 || kids_block(p, k, tag);
}


static void emit_list (Parser *p, Out *o, const int *kids, int nkid, const char *parent, int depth,
                       int idx, int total, int multiline, int *first);

/* node k, copy i of n */
static void emit_node (Parser *p, Out *o, int k, const char *parent, int depth, int i, int n,
                       int multiline, int *first) {
  const ENode *nd = &p->v[k];
  const char *tag = nd->name && nd->name[0] ? nd->name : implicit(parent);
  char *name, *attrs = NULL;
  int a, j, isvoid, block, xml = o->mode != EMMET_HTML;
  size_t ai;
  if (nd->name == NULL) {	/* a group: its items */
    emit_list(p, o, nd->kid, nd->nkid, parent, depth, i, n, multiline, first);
    return;
  }
  if (multiline && !*first) {
    buf_putc(&o->b, '\n');
    indent(o, depth);
  }
  *first = 0;
  for (ai = 0; ai < sizeof(alias) / sizeof(alias[0]); ai++)
    if (strcmp(alias[ai].abbr, tag) == 0) {
      tag = alias[ai].tag;
      attrs = (char *)alias[ai].attrs;
      break;
    }
  name = number(tag, i, n);
  buf_putc(&o->b, '<');
  put_lit(o, name);
  if (nd->id) {
    char *v = number(nd->id, i, n);
    buf_puts(&o->b, " id=\"");
    put_lit(o, v);
    buf_putc(&o->b, '"');
    free(v);
  }
  if (nd->cls.len) {
    char *c = (char *)xmalloc(nd->cls.len + 1), *v;
    memcpy(c, nd->cls.s, nd->cls.len);
    c[nd->cls.len] = '\0';
    v = number(c, i, n);
    buf_puts(&o->b, o->mode == EMMET_JSX ? " className=\"" : " class=\"");
    put_lit(o, v);
    buf_putc(&o->b, '"');
    free(v);
    free(c);
  }
  if (attrs) {	/* the alias's: "type=text name id" (a \ keeps a space) */
    const char *s = attrs;
    while (*s) {
      Buf an, av;
      int has = 0, given = 0;
      buf_init(&an);
      buf_init(&av);
      while (*s == ' ') s++;
      while (*s && *s != ' ' && *s != '=') buf_putc(&an, *s++);
      if (*s == '=') {
        has = 1;
        s++;
        while (*s && *s != ' ') {
          if (*s == '\\' && s[1]) s++;
          buf_putc(&av, *s++);
        }
      }
      buf_putc(&an, '\0');
      buf_putc(&av, '\0');
      for (j = 0; j < nd->nattr; j++)	/* [attr=x] wins */
        if (strcmp(nd->attr[j].name, an.s) == 0) given = 1;
      if (an.len > 1 && !given) {
        buf_putc(&o->b, ' ');
        put_lit(o, o->mode == EMMET_JSX && strcmp(an.s, "for") == 0 ? "htmlFor" : an.s);
        buf_puts(&o->b, "=\"");
        if (has) put_lit(o, av.s);
        else buf_printf(&o->b, "${%d}", ++o->stop);
        buf_putc(&o->b, '"');
      }
      buf_free(&an);
      buf_free(&av);
    }
  }
  for (a = 0; a < nd->nattr; a++) {
    const Attr *at = &nd->attr[a];
    char *an = number(at->name, i, n);
    buf_putc(&o->b, ' ');
    put_lit(o, o->mode == EMMET_JSX && strcmp(an, "class") == 0 ? "className" :
               o->mode == EMMET_JSX && strcmp(an, "for") == 0 ? "htmlFor" : an);
    buf_puts(&o->b, "=\"");
    if (at->value) {
      char *v = number(at->value, i, n);
      put_lit(o, v);
      free(v);
    }
    else buf_printf(&o->b, "${%d}", ++o->stop);
    buf_putc(&o->b, '"');
    free(an);
  }
  isvoid = in(void_tags, name) && !nd->text && nd->nkid == 0;
  if (isvoid) {
    buf_puts(&o->b, xml ? " />" : ">");
    free(name);
    return;
  }
  buf_putc(&o->b, '>');
  if (nd->text) {
    char *t = number(nd->text, i, n);
    put_lit(o, t);
    free(t);
  }
  block = kids_block(p, k, name);
  if (nd->nkid) {
    int f = 1;
    if (block) {
      buf_putc(&o->b, '\n');
      indent(o, depth + 1);
    }
    emit_list(p, o, nd->kid, nd->nkid, name, depth + 1, i, n, block, &f);
    if (block) {
      buf_putc(&o->b, '\n');
      indent(o, depth);
    }
  }
  else if (!nd->text) buf_printf(&o->b, "${%d}", ++o->stop);
  buf_puts(&o->b, "</");
  put_lit(o, name);
  buf_putc(&o->b, '>');
  free(name);
}


/* a list of siblings, each repeated; idx/total: the enclosing copy, for $ */
static void emit_list (Parser *p, Out *o, const int *kids, int nkid, const char *parent, int depth,
                       int idx, int total, int multiline, int *first) {
  int i, c;
  for (i = 0; i < nkid; i++) {
    const ENode *nd = &p->v[kids[i]];
    for (c = 1; c <= nd->mul && o->b.len < EMMET_MAX; c++)	/* a*1000>b*1000>c*1000: it stops */
      emit_node(p, o, kids[i], parent, depth, nd->mul > 1 ? c : idx, nd->mul > 1 ? nd->mul : total,
                multiline, first);
  }
}


static const char html5[] =
  "<!DOCTYPE html>\n<html lang=\"${1:en}\">\n<head>\n\t<meta charset=\"UTF-8\">\n"
  "\t<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
  "\t<title>${2:Document}</title>\n</head>\n<body>\n\t$0\n</body>\n</html>";


/* the abbreviation's snippet body (malloc'd), NULL: not an abbreviation */
static char *expand_html (const char *abbr, size_t n, int mode) {
  Parser p;
  Out o;
  int root, first = 1;
  if (n == 1 && abbr[0] == '!') return xstrdup(html5);
  if (n >= 6 && strncmp(abbr, "html:5", 6) == 0 && n == 6) return xstrdup(html5);
  memset(&p, 0, sizeof(p));
  p.s = abbr;
  p.e = abbr + n;
  root = parse_list(&p, 0);
  if (p.bad || p.s != p.e || p.v[root].nkid == 0) {
    free_nodes(&p);
    return NULL;
  }
  memset(&o, 0, sizeof(o));
  buf_init(&o.b);
  o.mode = mode;
  emit_list(&p, &o, p.v[root].kid, p.v[root].nkid, NULL, 0, 1, 1, kids_block(&p, root, NULL), &first);
  free_nodes(&p);
  if (o.b.len >= EMMET_MAX) {	/* too big to be meant */
    buf_free(&o.b);
    return NULL;
  }
  buf_putc(&o.b, '\0');
  return buf_take(&o.b);
}


/* worth expanding by Tab? a known tag or alias alone, or something with Emmet's marks */
static int html_worth (const char *abbr, size_t n) {
  char w[32];
  size_t i;
  if (n == 1 && abbr[0] == '!') return 1;
  for (i = 0; i < n; i++)
    if (strchr(".#>+^*[{(", abbr[i])) return 1;
  if (n >= sizeof(w)) return 0;
  memcpy(w, abbr, n);
  w[n] = '\0';
  if (in(html_tags, w)) return 1;
  for (i = 0; i < sizeof(alias) / sizeof(alias[0]); i++)
    if (strcmp(alias[i].abbr, w) == 0) return 1;
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** CSS
** ===================================================================
*/

enum { V_LEN, V_NUM, V_COLOR, V_ANY };

static const struct {
  const char *abbr, *prop;
  int kind;
} props[] = {
  {"m", "margin", V_LEN}, {"mt", "margin-top", V_LEN}, {"mr", "margin-right", V_LEN},
  {"mb", "margin-bottom", V_LEN}, {"ml", "margin-left", V_LEN}, {"p", "padding", V_LEN},
  {"pt", "padding-top", V_LEN}, {"pr", "padding-right", V_LEN}, {"pb", "padding-bottom", V_LEN},
  {"pl", "padding-left", V_LEN}, {"w", "width", V_LEN}, {"h", "height", V_LEN},
  {"maw", "max-width", V_LEN}, {"mah", "max-height", V_LEN}, {"miw", "min-width", V_LEN},
  {"mih", "min-height", V_LEN}, {"t", "top", V_LEN}, {"r", "right", V_LEN},
  {"b", "bottom", V_LEN}, {"l", "left", V_LEN}, {"z", "z-index", V_NUM},
  {"fz", "font-size", V_LEN}, {"fw", "font-weight", V_NUM}, {"lh", "line-height", V_NUM},
  {"op", "opacity", V_NUM}, {"o", "opacity", V_NUM}, {"bd", "border", V_LEN},
  {"bdt", "border-top", V_LEN}, {"bdr", "border-right", V_LEN}, {"bdb", "border-bottom", V_LEN},
  {"bdl", "border-left", V_LEN}, {"bdrs", "border-radius", V_LEN}, {"bdw", "border-width", V_LEN},
  {"bdc", "border-color", V_COLOR}, {"c", "color", V_COLOR}, {"bgc", "background-color", V_COLOR},
  {"bg", "background", V_COLOR}, {"g", "gap", V_LEN}, {"gap", "gap", V_LEN},
  {"rg", "row-gap", V_LEN}, {"cg", "column-gap", V_LEN}, {"fx", "flex", V_NUM},
  {"fxg", "flex-grow", V_NUM}, {"fxs", "flex-shrink", V_NUM}, {"fxb", "flex-basis", V_LEN},
  {"ord", "order", V_NUM}, {"ti", "text-indent", V_LEN}, {"lts", "letter-spacing", V_LEN},
  {"ff", "font-family", V_ANY}, {"d", "display", V_ANY}, {"pos", "position", V_ANY},
  {"ta", "text-align", V_ANY}, {"ov", "overflow", V_ANY}, {"cur", "cursor", V_ANY},
  {"trs", "transition", V_ANY}, {"trf", "transform", V_ANY}, {"ol", "outline", V_LEN},
  {"bxsh", "box-shadow", V_ANY}, {"tsh", "text-shadow", V_ANY}, {"va", "vertical-align", V_ANY},
  {"ws", "white-space", V_ANY}, {"wob", "word-break", V_ANY}, {"gtc", "grid-template-columns", V_ANY},
  {"gtr", "grid-template-rows", V_ANY}, {"ai", "align-items", V_ANY}, {"jc", "justify-content", V_ANY},
  {"ac", "align-content", V_ANY}, {"as", "align-self", V_ANY}, {"fxd", "flex-direction", V_ANY},
  {"fxw", "flex-wrap", V_ANY}, {"lis", "list-style", V_ANY}, {"anim", "animation", V_ANY},
  {"bxz", "box-sizing", V_ANY}, {"tt", "text-transform", V_ANY}, {"td", "text-decoration", V_ANY},
  {"fs", "font-style", V_ANY}, {"fl", "float", V_ANY}, {"cl", "clear", V_ANY},
  {"v", "visibility", V_ANY}, {"con", "content", V_ANY}, {"ins", "inset", V_LEN},
  {"ar", "aspect-ratio", V_ANY}, {"ow", "overflow-wrap", V_ANY}
};

static const struct {
  const char *abbr, *decl;
} words[] = {
  {"dn", "display: none"}, {"db", "display: block"}, {"di", "display: inline"},
  {"dib", "display: inline-block"}, {"df", "display: flex"}, {"dif", "display: inline-flex"},
  {"dg", "display: grid"}, {"dig", "display: inline-grid"}, {"dt", "display: table"},
  {"dtc", "display: table-cell"}, {"posa", "position: absolute"}, {"posr", "position: relative"},
  {"posf", "position: fixed"}, {"poss", "position: static"}, {"posst", "position: sticky"},
  {"tac", "text-align: center"}, {"tal", "text-align: left"}, {"tar", "text-align: right"},
  {"taj", "text-align: justify"}, {"tdn", "text-decoration: none"}, {"tdu", "text-decoration: underline"},
  {"tdl", "text-decoration: line-through"}, {"fwb", "font-weight: bold"}, {"fwn", "font-weight: normal"},
  {"fsi", "font-style: italic"}, {"fsn", "font-style: normal"}, {"ttu", "text-transform: uppercase"},
  {"ttl", "text-transform: lowercase"}, {"ttc", "text-transform: capitalize"},
  {"ttn", "text-transform: none"}, {"fll", "float: left"}, {"flr", "float: right"},
  {"fln", "float: none"}, {"ovh", "overflow: hidden"}, {"ova", "overflow: auto"},
  {"ovs", "overflow: scroll"}, {"ovv", "overflow: visible"}, {"cup", "cursor: pointer"},
  {"cud", "cursor: default"}, {"bxzbb", "box-sizing: border-box"}, {"bxzcb", "box-sizing: content-box"},
  {"jcc", "justify-content: center"}, {"jcsb", "justify-content: space-between"},
  {"jcsa", "justify-content: space-around"}, {"jcse", "justify-content: space-evenly"},
  {"jcfs", "justify-content: flex-start"}, {"jcfe", "justify-content: flex-end"},
  {"aic", "align-items: center"}, {"aifs", "align-items: flex-start"}, {"aife", "align-items: flex-end"},
  {"ais", "align-items: stretch"}, {"aib", "align-items: baseline"}, {"fxdc", "flex-direction: column"},
  {"fxdr", "flex-direction: row"}, {"fxdcr", "flex-direction: column-reverse"},
  {"fxdrr", "flex-direction: row-reverse"}, {"fxww", "flex-wrap: wrap"}, {"fxwn", "flex-wrap: nowrap"},
  {"bdn", "border: none"}, {"lisn", "list-style: none"}, {"wsnw", "white-space: nowrap"},
  {"wsp", "white-space: pre"}, {"vh", "visibility: hidden"}, {"vv", "visibility: visible"},
  {"cb", "clear: both"}, {"vam", "vertical-align: middle"}, {"vat", "vertical-align: top"},
  {"vab", "vertical-align: bottom"}, {"oln", "outline: none"}, {"bgn", "background: none"},
  {"m0", "margin: 0"}, {"p0", "padding: 0"}, {"mia", "margin: 0 auto"}
};


/* one value: 10 -> 10px, 10p -> 10%, 1.5e -> 1.5em, a -> auto, #f -> #fff; 0 not one */
static int css_value (const char *s, size_t n, int kind, Buf *b) {
  size_t i = 0, d;
  int neg = 0;
  if (n == 0) return 0;
  if (s[0] == '#' && (kind == V_COLOR || kind == V_LEN)) {	/* #f -> #fff, #e0 -> #e0e0e0 */
    for (i = 1; i < n; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || ((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'f'))) return 0;
    buf_putc(b, '#');
    if (n == 2) for (i = 0; i < 3; i++) buf_putc(b, s[1]);
    else if (n == 3) for (i = 0; i < 3; i++) buf_putn(b, s + 1, 2);
    else if (n == 4 || n == 7) buf_putn(b, s + 1, n - 1);
    else return 0;
    return 1;
  }
  if (n == 1 && s[0] == 'a' && kind == V_LEN) {
    buf_puts(b, "auto");
    return 1;
  }
  if (s[0] == '-') {
    neg = 1;
    i = 1;
  }
  d = i;
  while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) i++;
  if (i == d) return 0;
  if (neg) buf_putc(b, '-');
  buf_putn(b, s + d, i - d);
  if (i == n) {	/* no unit: px for lengths but 0 */
    if (kind == V_LEN && !(i - d == 1 && s[d] == '0')) buf_puts(b, "px");
    return 1;
  }
  if (n - i == 1) {
    switch (s[i]) {
      case 'p': buf_putc(b, '%'); return 1;
      case 'e': buf_puts(b, "em"); return 1;
      case 'r': buf_puts(b, "rem"); return 1;
      case 'x': buf_puts(b, "ex"); return 1;
      case 'w': buf_puts(b, "vw"); return 1;
      case 'h': buf_puts(b, "vh"); return 1;
    }
    return 0;
  }
  if ((n - i == 2 && (memcmp(s + i, "px", 2) == 0 || memcmp(s + i, "em", 2) == 0 ||
                      memcmp(s + i, "vw", 2) == 0 || memcmp(s + i, "vh", 2) == 0 ||
                      memcmp(s + i, "fr", 2) == 0 || memcmp(s + i, "ch", 2) == 0)) ||
      (n - i == 3 && memcmp(s + i, "rem", 3) == 0) || (n - i == 1 && s[i] == '%')) {
    buf_putn(b, s + i, n - i);
    return 1;
  }
  return 0;
}


static char *expand_css (const char *abbr, size_t n) {
  size_t i, k;
  int imp = 0;
  Buf b;
  char w[32];
  if (n > 0 && abbr[n - 1] == '!') {	/* !important */
    imp = 1;
    n--;
  }
  if (n == 0 || n >= sizeof(w)) return NULL;
  memcpy(w, abbr, n);
  w[n] = '\0';
  buf_init(&b);
  for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    if (strcmp(words[i].abbr, w) == 0) {
      buf_puts(&b, words[i].decl);
      buf_puts(&b, imp ? " !important;" : ";");
      buf_putc(&b, '\0');
      return buf_take(&b);
    }
  for (k = n; k > 0; k--) {	/* the longest property that leaves a good value */
    for (i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
      const char *v = abbr + k;
      size_t vn = n - k, j, from;
      int ok = 1, count = 0;
      if (strlen(props[i].abbr) != k || strncmp(props[i].abbr, abbr, k) != 0) continue;
      b.len = 0;
      buf_puts(&b, props[i].prop);
      buf_puts(&b, ": ");
      if (vn == 0) buf_puts(&b, "${1}");
      else if (props[i].kind == V_ANY) ok = 0;
      else {	/* 10-20 (a - after a number separates; one first is a minus) */
        for (j = 0, from = 0; j <= vn && ok; j++) {
          if (j == vn || (v[j] == '-' && j > from)) {
            if (count++) buf_putc(&b, ' ');
            ok = css_value(v + from, j - from, props[i].kind, &b);
            from = j + 1;
          }
        }
      }
      if (!ok) continue;
      buf_puts(&b, imp ? " !important;" : ";");
      buf_putc(&b, '\0');
      return buf_take(&b);
    }
  }
  buf_free(&b);
  return NULL;
}

/* }================================================================== */


/*
** {==================================================================
** For the editor
** ===================================================================
*/

/* Emmet's language for a file: its language id, and the file's name for JSX */
int emmet_mode (const char *lang, const char *path) {
  static const char *const html[] = {"html", "xml", "vue", "svelte", "php", "xsl", "handlebars",
                                     "razor", "haml", "markdown", NULL};
  static const char *const css[] = {"css", "scss", "less", "sass", "stylus", NULL};
  const char *dot = path ? strrchr(path, '.') : NULL;
  if (lang == NULL) return 0;
  if (in(css, lang)) return EMMET_CSS;
  if (strcmp(lang, "xml") == 0) return EMMET_XML;
  if (in(html, lang)) return EMMET_HTML;
  if (strcmp(lang, "javascriptreact") == 0 || strcmp(lang, "typescriptreact") == 0 ||
      (dot && (strcmp(dot, ".jsx") == 0 || strcmp(dot, ".tsx") == 0)))
    return EMMET_JSX;
  return 0;
}


/*
** Where the abbreviation that ends at x starts in s: back over its
** characters, a {text} or [attributes] whole (they may have spaces).
*/
size_t emmet_start (const char *s, size_t x, int mode) {
  size_t i = x;
  while (i > 0) {
    char c = s[i - 1];
    if (c == '}' || c == ']') {	/* back to its opening */
      char open = c == '}' ? '{' : '[';
      size_t j = i - 1;
      while (j > 0 && s[j - 1] != open) j--;
      if (j == 0) break;
      i = j - 1;
      continue;
    }
    if (mode == EMMET_CSS ? ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                             c == '-' || c == '#' || c == '!' || c == '%' || (c >= 'A' && c <= 'F'))
                          : ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             strchr(".#>+^*$@:-_!()", c) != NULL)) {
      i--;
      continue;
    }
    break;
  }
  while (i < x && strchr(">+^*)", s[i])) i++;	/* it cannot start with an operator */
  return i;
}


/* abbr's snippet body (malloc'd); NULL: not one. worth: Tab would expand it */
char *emmet_expand (const char *abbr, size_t n, int mode, int *worth) {
  char *r;
  *worth = 0;
  if (n == 0 || mode == 0) return NULL;
  if (mode == EMMET_CSS) {
    r = expand_css(abbr, n);
    *worth = r != NULL;
    return r;
  }
  if (abbr[0] >= '0' && abbr[0] <= '9') return NULL;
  r = expand_html(abbr, n, mode);
  if (r) *worth = html_worth(abbr, n);
  return r;
}

/* }================================================================== */
