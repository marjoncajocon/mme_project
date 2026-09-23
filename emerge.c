/*
** emerge.c - the merge editor: a conflicted file in three panes
**
** Incoming (theirs) and Current (ours) side by side on top, the Result
** under them, as VS Code shows them. The three versions come from git's
** index (:1: the base, :2: ours, :3: theirs) when it has them, else from
** the <<<<<<< ======= >>>>>>> markers the file already has.
**
** The conflicts are found the way diff3 finds them: the base is compared
** with each side, and where both sides changed the same lines there is a
** conflict. Everything else is merged at once, so the Result starts as
** the file with only the real conflicts left, each one still written with
** its markers. Accepting a side puts that side's lines in their place;
** the Result is a text of its own, typed in and undone like any other,
** and Complete Merge writes it to the file (and stages it, when git had
** the file as unmerged).
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct MBlock {	/* one conflict, in the lines of each version */
  size_t b0, b1;	/* the base's */
  size_t o0, o1;	/* ours (Current) */
  size_t t0, t1;	/* theirs (Incoming) */
  int done;	/* a side was taken: it is not in the Result any more */
} MBlock;

enum { PANE_INC, PANE_CUR, PANE_RES };

static struct {
  int open;
  char *path;	/* the file, native */
  char *rel;	/* from the repository's top, '/' between */
  int staged;	/* git has it as unmerged: Complete Merge stages it */
  int crlf;	/* its lines end with CR LF */
  int no_eol;	/* and there is no newline after the last one */
  char ours_name[64], theirs_name[64];	/* the branches: the titles and the markers */
  char *base, *ours, *theirs;	/* the three versions */
  size_t nbase, nours, ntheirs;
  Doc inc, cur, res;	/* what the panes show; res is the one typed in */
  const Syntax *sx;
  MBlock *blk;
  size_t nblk;
  size_t cb;	/* the conflict in view */
  int focus;	/* PANE_* */
  int run;	/* what the last key did: 0 nothing, 1 typing, 2 erasing (one undo step) */
  int follow;	/* the Result scrolls to its cursor (the wheel scrolls away from it) */
  Pos rcur;	/* the cursor in the Result */
  size_t top[3];	/* the first line each pane shows */
  int px[3], py[3], pw[3], ph[3];	/* where they were drawn, for the mouse */
  int act_y, act_x0[2], act_x1[2];	/* Accept Incoming, Accept Current */
  int res_y, both_x0, both_x1, done_x0, done_x1;	/* Accept Combination, Complete Merge */
} M;


/*
** {==================================================================
** The three versions
** ===================================================================
*/

static void sides_free (void) {
  free(M.base);
  free(M.ours);
  free(M.theirs);
  M.base = M.ours = M.theirs = NULL;
  M.nbase = M.nours = M.ntheirs = 0;
}


void merge_close (void) {
  if (!M.open) return;
  sides_free();
  doc_free(&M.inc);
  doc_free(&M.cur);
  doc_free(&M.res);
  free(M.blk);
  free(M.path);
  free(M.rel);
  memset(&M, 0, sizeof(M));
}


int merge_active (void) {
  return M.open;
}


const char *merge_file (void) {
  return M.open ? M.path : NULL;
}


/* "a.txt (Merging)", the tab's name */
const char *merge_tab_name (void) {
  static char name[280];
  if (!M.open) return "Merge";
  snprintf(name, sizeof(name), "%s (Merging)", path_basename(M.path));
  return name;
}


/* the text of .git/<name>, without its newline; NULL: it is not there */
static char *git_file (const char *name, size_t *len) {
  char *dir, *f, *s;
  if (git_root() == NULL) return NULL;
  dir = path_join(git_root(), ".git");
  f = path_join(dir, name);
  s = read_file(f, len);
  free(dir);
  free(f);
  if (s == NULL) return NULL;
  while (*len > 0 && (s[*len - 1] == '\n' || s[*len - 1] == '\r')) s[--(*len)] = '\0';
  return s;
}


/* the branch the commit in .git/<file> is on ("feature"), for the titles */
static int rev_name (const char *file, char *out, size_t n) {
  size_t len = 0;
  char *s = git_file(file, &len);
  Buf b;
  const char *a[] = {"name-rev", "--name-only", NULL, NULL};
  if (s == NULL || len < 7) {
    free(s);
    return 0;
  }
  snprintf(out, n, "%.8s", s);	/* its short hash, until git gives it a name */
  a[2] = s;
  buf_init(&b);
  if (git_exec(&b, 0, a) == 0 && b.len > 0) {
    while (b.len > 0 && (b.s[b.len - 1] == '\n' || b.s[b.len - 1] == '\r')) b.s[--b.len] = '\0';
    if (b.len > 0 && strcmp(b.s, "undefined") != 0) snprintf(out, n, "%s", b.s);
  }
  buf_free(&b);
  free(s);
  return 1;
}


static void names (void) {
  const char *br = git_branch();
  snprintf(M.ours_name, sizeof(M.ours_name), "%s", br[0] ? br : "HEAD");
  if (rev_name("MERGE_HEAD", M.theirs_name, sizeof(M.theirs_name))) return;
  if (rev_name("REBASE_HEAD", M.theirs_name, sizeof(M.theirs_name))) return;
  if (rev_name("CHERRY_PICK_HEAD", M.theirs_name, sizeof(M.theirs_name))) return;
  snprintf(M.theirs_name, sizeof(M.theirs_name), "Incoming");
}


/* a line that is seven of c, then its end or a space: a conflict marker */
static int marker (const char *s, size_t n, char c) {
  size_t i;
  if (n < 7) return 0;
  for (i = 0; i < 7; i++)
    if (s[i] != c) return 0;
  return n == 7 || s[7] == ' ' || s[7] == '\t';
}


/* one side of a file as its markers have it: which 0 ours, 1 theirs */
static char *from_markers (const char *s, size_t len, int which, size_t *out) {
  Buf b;
  size_t i, from = 0;
  int in = 0, keep = 1;
  buf_init(&b);
  for (i = 0; i <= len; i++) {
    const char *line;
    size_t n;
    if (i < len && s[i] != '\n') continue;
    line = s + from;
    n = i - from;
    if (n > 0 && line[n - 1] == '\r') n--;
    if (marker(line, n, '<')) in = 1, keep = (which == 0);
    else if (in && marker(line, n, '|')) keep = 0;
    else if (in && marker(line, n, '=')) keep = (which == 1);
    else if (in && marker(line, n, '>')) in = 0, keep = 1;
    else if (keep) {
      buf_putn(&b, line, n);
      buf_putc(&b, '\n');
    }
    from = i + 1;
  }
  *out = b.len;
  return buf_take(&b);
}

/* }================================================================== */


/*
** {==================================================================
** The conflicts: what diff3 does
** ===================================================================
*/

/* the side's line the base's line b became; end: the line after the last */
static size_t map_line (const QHunk *h, size_t nh, size_t b, int end) {
  size_t i;
  long d = 0;
  for (i = 0; i < nh; i++) {
    if (h[i].o0 + h[i].on <= b) {
      d += (long)h[i].nn - (long)h[i].on;
      continue;
    }
    if (h[i].o0 <= b) return end ? h[i].n0 + h[i].nn : h[i].n0;	/* b is inside it */
    break;
  }
  return (size_t)((long)b + d);
}


/*
** The base against each side gives the lines each one changed; where the
** two touch, the change belongs to both and is a conflict, and the rest
** merges by itself.
*/
static void build_blocks (void) {
  QHunk *ho, *ht;
  size_t nho, nht, i = 0, j = 0, cap = 0;
  ho = text_hunks(M.base, M.nbase, M.ours, M.nours, &nho);
  ht = text_hunks(M.base, M.nbase, M.theirs, M.ntheirs, &nht);
  M.nblk = 0;
  while (i < nho || j < nht) {
    size_t b0, b1;
    int from_o = 0, from_t = 0;
    if (j >= nht || (i < nho && ho[i].o0 <= ht[j].o0)) {
      b0 = ho[i].o0;
      b1 = b0 + ho[i].on;
      from_o = 1;
      i++;
    }
    else {
      b0 = ht[j].o0;
      b1 = b0 + ht[j].on;
      from_t = 1;
      j++;
    }
    for (;;) {	/* what else reaches these lines belongs to the same region */
      if (i < nho && ho[i].o0 <= b1) {
        if (ho[i].o0 + ho[i].on > b1) b1 = ho[i].o0 + ho[i].on;
        from_o = 1;
        i++;
        continue;
      }
      if (j < nht && ht[j].o0 <= b1) {
        if (ht[j].o0 + ht[j].on > b1) b1 = ht[j].o0 + ht[j].on;
        from_t = 1;
        j++;
        continue;
      }
      break;
    }
    if (!from_o || !from_t) continue;	/* one side only: it merges cleanly */
    if (M.nblk == cap) M.blk = (MBlock *)xrealloc(M.blk, (cap = cap ? cap * 2 : 8) * sizeof(MBlock));
    M.blk[M.nblk].b0 = b0;
    M.blk[M.nblk].b1 = b1;
    M.blk[M.nblk].o0 = map_line(ho, nho, b0, 0);
    M.blk[M.nblk].o1 = map_line(ho, nho, b1, 1);
    M.blk[M.nblk].t0 = map_line(ht, nht, b0, 0);
    M.blk[M.nblk].t1 = map_line(ht, nht, b1, 1);
    M.blk[M.nblk].done = 0;
    M.nblk++;
  }
  free(ho);
  free(ht);
}


/*
** The lines of d: a text ending with a newline leaves an empty row after
** it, which is not a line of the file (nor one text_hunks counts).
*/
static size_t nlines (const Doc *d) {
  return d->n > 1 && d->row[d->n - 1].len == 0 ? d->n - 1 : d->n;
}


/* the lines [a, b) of d, each one ending with a newline */
static void put_lines (Buf *out, const Doc *d, size_t a, size_t b) {
  size_t y, n = nlines(d);
  if (b > n) b = n;
  for (y = a; y < b; y++) {
    buf_putn(out, d->row[y].s, d->row[y].len);
    buf_putc(out, '\n');
  }
}


/*
** The Result as it starts: what both sides kept, what only one of them
** changed already put in, and every conflict still with its markers.
*/
static char *build_result (size_t *len) {
  Buf b;
  Doc bd;
  QHunk *ho, *ht;
  size_t nho, nht, y = 0, k = 0, i = 0, j = 0;
  doc_init(&bd);
  doc_set_text(&bd, M.base, M.nbase);
  ho = text_hunks(M.base, M.nbase, M.ours, M.nours, &nho);
  ht = text_hunks(M.base, M.nbase, M.theirs, M.ntheirs, &nht);
  buf_init(&b);
  for (;;) {
    size_t nb = nlines(&bd), next = nb, py = y, pk = k, pi = i, pj = j;
    while (i < nho && ho[i].o0 < y && ho[i].o0 + ho[i].on <= y) i++;	/* passed already */
    while (j < nht && ht[j].o0 < y && ht[j].o0 + ht[j].on <= y) j++;
    if (k < M.nblk && M.blk[k].b0 < next) next = M.blk[k].b0;
    if (i < nho && ho[i].o0 < next) next = ho[i].o0;
    if (j < nht && ht[j].o0 < next) next = ht[j].o0;
    for (; y < next && y < nb; y++) {	/* the lines nobody touched */
      buf_putn(&b, bd.row[y].s, bd.row[y].len);
      buf_putc(&b, '\n');
    }
    if (k < M.nblk && y == M.blk[k].b0) {	/* a conflict: both sides, between markers */
      buf_printf(&b, "<<<<<<< %s\n", M.ours_name);
      put_lines(&b, &M.cur, M.blk[k].o0, M.blk[k].o1);
      buf_puts(&b, "=======\n");
      put_lines(&b, &M.inc, M.blk[k].t0, M.blk[k].t1);
      buf_printf(&b, ">>>>>>> %s\n", M.theirs_name);
      y = M.blk[k].b1;
      k++;
    }
    else if (i < nho && ho[i].o0 == y) {	/* only we changed these lines */
      put_lines(&b, &M.cur, ho[i].n0, ho[i].n0 + ho[i].nn);
      y += ho[i].on;
      i++;
    }
    else if (j < nht && ht[j].o0 == y) {	/* only they did */
      put_lines(&b, &M.inc, ht[j].n0, ht[j].n0 + ht[j].nn);
      y += ht[j].on;
      j++;
    }
    if (y == py && k == pk && i == pi && j == pj) break;	/* nothing left to take */
  }
  *len = b.len;
  doc_free(&bd);
  free(ho);
  free(ht);
  return buf_take(&b);
}

/* }================================================================== */


/*
** {==================================================================
** Opening
** ===================================================================
*/

static int binary (const char *s, size_t n) {
  size_t i, k = n < 8000 ? n : 8000;
  for (i = 0; i < k; i++)
    if (s[i] == '\0') return 1;
  return 0;
}


/*
** CR LF: the editor's text has every line ending with LF, so the bytes on
** disk are the ones to ask.
*/
static void eol_of (const char *path) {
  size_t n = 0, i;
  char *raw = read_file(path, &n);
  if (raw == NULL) return;
  M.no_eol = n > 0 && raw[n - 1] != '\n';
  for (i = 0; i + 1 < n; i++)
    if (raw[i] == '\r' && raw[i + 1] == '\n') {
      M.crlf = 1;
      break;
    }
  free(raw);
}


/* a text with no newline after its last line */
static int unterminated (const char *s, size_t n) {
  return n > 0 && s[n - 1] != '\n';
}


/* the file's text: the editor's when it has it open, else the disk's */
static char *file_text (const char *path, size_t *len) {
  char *s = open_doc_text(path, len);
  return s != NULL ? s : read_file(path, len);
}


/* the text has a <<<<<<< line of its own */
static int has_markers (const char *s, size_t len) {
  size_t i, from = 0;
  for (i = 0; i <= len; i++) {
    if (i < len && s[i] != '\n') continue;
    if (marker(s + from, i - from, '<')) return 1;
    from = i + 1;
  }
  return 0;
}


/* the file is one the merge editor can take: unmerged, or full of markers */
int merge_candidate (const char *path) {
  char *s;
  size_t len = 0;
  int ok;
  if (path == NULL) return 0;
  if (git_conflicted(path)) return 1;
  s = file_text(path, &len);
  if (s == NULL) return 0;
  ok = !binary(s, len) && has_markers(s, len);
  free(s);
  return ok;
}


/* the merge editor for path: 0 it opened, -1 it could not (a toast says why) */
int merge_open (const char *path) {
  char *disk;
  size_t dn = 0;
  int stages = 0;
  if (path == NULL) return -1;
  merge_close();
  disk = file_text(path, &dn);
  if (disk == NULL) {
    toast(1, "Unable to read '%s'", path_basename(path));
    return -1;
  }
  if (binary(disk, dn)) {
    toast(1, "'%s' is a binary file and cannot be merged here", path_basename(path));
    free(disk);
    return -1;
  }
  M.path = xstrdup(path);
  M.rel = git_rel_of(path);
  M.staged = git_conflicted(path);
  eol_of(path);

  if (M.rel != NULL) {	/* git's three versions, when the index has them */
    char *spec = xstrcat3(":1:", M.rel, "");
    M.base = git_blob(spec, &M.nbase);
    free(spec);
    spec = xstrcat3(":2:", M.rel, "");
    M.ours = git_blob(spec, &M.nours);
    free(spec);
    spec = xstrcat3(":3:", M.rel, "");
    M.theirs = git_blob(spec, &M.ntheirs);
    free(spec);
    stages = M.ours != NULL && M.theirs != NULL;
    if (!stages && (M.ours != NULL || M.theirs != NULL)) {	/* one side deleted it */
      int by_us = M.ours == NULL;
      toast(1, "'%s' was deleted in the %s changes and modified in the %s ones: keep it or "
               "delete it from the Source Control view", path_basename(path),
            by_us ? "current" : "incoming", by_us ? "incoming" : "current");
      free(disk);
      merge_close();
      return -1;
    }
  }
  if (!stages) {	/* no index to ask: what the markers in the file say */
    if (!has_markers(disk, dn)) {
      toast(0, "No merge conflicts in '%s'", path_basename(path));
      free(disk);
      merge_close();
      return -1;
    }
    sides_free();
    M.ours = from_markers(disk, dn, 0, &M.nours);
    M.theirs = from_markers(disk, dn, 1, &M.ntheirs);
  }
  if (stages)	/* git ends its last >>>>>>> line: the versions know better */
    M.no_eol = unterminated(M.ours, M.nours) && unterminated(M.theirs, M.ntheirs);
  names();
  if (M.base == NULL) {	/* added on both sides: there is no common version */
    M.base = xstrdup("");
    M.nbase = 0;
  }
  doc_init(&M.inc);
  doc_init(&M.cur);
  doc_init(&M.res);
  doc_set_text(&M.inc, M.theirs, M.ntheirs);
  doc_set_text(&M.cur, M.ours, M.nours);
  build_blocks();
  {
    size_t rn = 0;
    char *r = build_result(&rn);
    doc_set_text(&M.res, r, rn);
    free(r);
  }
  M.res.path = xstrdup(path);
  M.inc.path = xstrdup(path);
  M.cur.path = xstrdup(path);
  M.sx = syntax_detect(path, &M.res);
  M.open = 1;
  M.focus = PANE_RES;
  M.follow = 1;
  free(disk);
  if (M.nblk == 0) toast(0, "'%s' has no conflicts left to resolve", path_basename(path));
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** The conflicts still in the Result
** ===================================================================
*/

/* how many are left: the marker regions the Result still has */
int merge_left (void) {
  size_t n = 0;
  if (!M.open) return 0;
  conflicts(&M.res, &n);
  return (int)n;
}


/* which marker region block bi is; -1: it is resolved, or gone */
static long region_of (size_t bi) {
  size_t i, k = 0, n = 0;
  conflicts(&M.res, &n);
  if (bi >= M.nblk || M.blk[bi].done) return -1;
  for (i = 0; i < bi; i++)
    if (!M.blk[i].done) k++;
  return k < n ? (long)k : -1;
}


/* the lines [a, b) of the Result are the lines [c, e) of d */
static int same_lines (size_t a, size_t b, const Doc *d, size_t c, size_t e) {
  size_t i;
  if (b - a != e - c) return 0;
  for (i = 0; i + a < b; i++) {
    const Row *x = &M.res.row[a + i], *y = &d->row[c + i];
    if (c + i >= d->n || x->len != y->len || memcmp(x->s, y->s, x->len) != 0) return 0;
  }
  return 1;
}


/* the marker region v holds both sides of block k, as they were written */
static int region_is (const Conflict *v, size_t k) {
  size_t mid = v->base != (size_t)-1 ? v->base : v->mid;
  return same_lines(v->start + 1, mid, &M.cur, M.blk[k].o0, M.blk[k].o1) &&
         same_lines(v->mid + 1, v->end, &M.inc, M.blk[k].t0, M.blk[k].t1);
}


/*
** Which blocks are still in the Result: undo puts a region back, redo
** takes it away again, and each one says in its markers which block it
** is. A region the Result no longer holds is resolved.
*/
static void resync (void) {
  size_t n = 0, i, k = 0;
  const Conflict *v = conflicts(&M.res, &n);
  for (i = 0; i < M.nblk; i++) M.blk[i].done = 1;
  for (i = 0; i < n && k < M.nblk; i++) {
    while (k < M.nblk && !region_is(&v[i], k)) k++;
    if (k < M.nblk) M.blk[k++].done = 0;
  }
}


/* the first conflict still open from bi on, round again; back: before it */
static long next_open (long bi, int back) {
  long i, n = (long)M.nblk;
  if (n == 0) return -1;
  for (i = 0; i < n; i++) {
    long k = back ? ((bi - i) % n + n) % n : ((bi + i) % n + n) % n;
    if (!M.blk[k].done) return k;
  }
  return -1;
}


/* the Result's lines [a, b) that marker region r takes */
static void region_rows (long r, size_t *a, size_t *b) {
  size_t n = 0;
  const Conflict *v = conflicts(&M.res, &n);
  *a = *b = 0;
  if (r < 0 || (size_t)r >= n) return;
  *a = v[r].start;
  *b = v[r].end + 1;
}


static void scroll_to (int pane, size_t line) {
  int h = M.ph[pane] > 0 ? M.ph[pane] : 10;
  size_t top = M.top[pane];
  if (line < top + 2) top = line > 2 ? line - 2 : 0;
  else if (line + 3 >= top + (size_t)h) top = line + 3 > (size_t)h ? line + 3 - (size_t)h : 0;
  M.top[pane] = top;
}


/* the three panes go to the conflict in view */
static void show_block (void) {
  long r;
  size_t a, b;
  if (M.cb >= M.nblk) return;
  scroll_to(PANE_INC, M.blk[M.cb].t0);
  scroll_to(PANE_CUR, M.blk[M.cb].o0);
  r = region_of(M.cb);
  if (r < 0) return;
  region_rows(r, &a, &b);
  scroll_to(PANE_RES, a);
  M.rcur.y = a;
  M.rcur.x = 0;
  M.follow = 1;
}


/*
** The two sides of block bi say the same thing but for their spaces and
** tabs: VS Code calls these whitespace-only conflicts.
*/
static int ws_only (size_t bi) {
  size_t a = M.blk[bi].o0, b = M.blk[bi].t0, i = 0, j = 0;
  for (;;) {
    while (a < M.blk[bi].o1 && (i >= M.cur.row[a].len || M.cur.row[a].s[i] == ' ' ||
                                M.cur.row[a].s[i] == '\t'))
      if (++i > M.cur.row[a].len) a++, i = 0;
    while (b < M.blk[bi].t1 && (j >= M.inc.row[b].len || M.inc.row[b].s[j] == ' ' ||
                                M.inc.row[b].s[j] == '\t'))
      if (++j > M.inc.row[b].len) b++, j = 0;
    if (a >= M.blk[bi].o1 || b >= M.blk[bi].t1) return a >= M.blk[bi].o1 && b >= M.blk[bi].t1;
    if (M.cur.row[a].s[i] != M.inc.row[b].s[j]) return 0;
    i++;
    j++;
  }
}


/* one side of block bi as lines: which 0 current, 1 incoming, 2 both */
static void side_text (Buf *b, size_t bi, int which) {
  if (which != 1) put_lines(b, &M.cur, M.blk[bi].o0, M.blk[bi].o1);
  if (which != 0) put_lines(b, &M.inc, M.blk[bi].t0, M.blk[bi].t1);
}


/* block bi is resolved: its markers and both sides give way to 'which' */
static void accept_block (size_t bi, int which) {
  long r = region_of(bi), nx;
  size_t a, e;
  Buf t;
  Pos p0, p1;
  if (r < 0) return;
  region_rows(r, &a, &e);
  buf_init(&t);
  side_text(&t, bi, which);
  p0.y = a;
  p0.x = 0;
  if (e < M.res.n) {	/* the lines go in front of the line after the region */
    p1.y = e;
    p1.x = 0;
  }
  else {	/* the region reaches the end: the last newline goes with it */
    if (t.len > 0) t.len--;
    p1.y = M.res.n > 0 ? M.res.n - 1 : 0;
    p1.x = M.res.n > 0 ? M.res.row[p1.y].len : 0;
  }
  doc_group(&M.res);
  doc_delete(&M.res, p0, p1);
  if (t.len > 0) doc_insert(&M.res, p0, t.s, t.len);
  doc_group(&M.res);
  buf_free(&t);
  M.blk[bi].done = 1;
  M.run = 0;
  M.rcur = doc_clamp(&M.res, p0);
  nx = next_open((long)bi, 0);
  if (nx >= 0) {
    M.cb = (size_t)nx;
    show_block();
  }
}


/* Go to Next / Previous Conflict */
void merge_next (int back) {
  long nx;
  if (!M.open || M.nblk == 0) return;
  nx = next_open(back ? (long)M.cb - 1 : (long)M.cb + 1, back);
  if (nx < 0) {
    toast(0, "No merge conflicts left");
    return;
  }
  M.cb = (size_t)nx;
  show_block();
}

/* }================================================================== */


/*
** {==================================================================
** Finishing
** ===================================================================
*/

/* the Result as one text, with the line ends the file had */
static char *result_text (size_t *len) {
  Buf b;
  size_t y, n;
  buf_init(&b);
  n = nlines(&M.res);
  for (y = 0; y < n; y++) {
    buf_putn(&b, M.res.row[y].s, M.res.row[y].len);
    if (y + 1 == n && M.no_eol) break;	/* it had no newline at the end */
    if (M.crlf) buf_putc(&b, '\r');
    buf_putc(&b, '\n');
  }
  *len = b.len;
  return buf_take(&b);
}


/*
** Complete Merge: the Result is written to the file and, when git had it
** as unmerged, staged. 1 when it was done and the tab can close.
*/
int merge_complete (void) {
  static const char *const bt[] = {"Complete", "Cancel"};
  int left, fd;
  char *s;
  size_t n = 0;
  if (!M.open) return 0;
  left = merge_left();
  if (left > 0) {
    char msg[200];
    snprintf(msg, sizeof(msg), "%d conflict%s still unresolved in '%s'.", left,
             left == 1 ? "" : "s", path_basename(M.path));
    if (dialog(msg, "The file is written with its conflict markers still in it.", bt, 2) != 0)
      return 0;
  }
  s = result_text(&n);
  fd = os_open(M.path, OS_WRITE);
  if (fd < 0) {
    toast(1, "Unable to write '%s'", path_basename(M.path));
    free(s);
    return 0;
  }
  os_write(fd, s, n);
  os_close(fd);
  free(s);
  if (M.staged && M.rel != NULL && left == 0) {
    Buf b;
    const char *a[] = {"add", "--", NULL, NULL};
    a[2] = M.rel;
    buf_init(&b);
    if (git_exec(&b, 1, a) != 0) git_error(&b, "Unable to stage the file");
    buf_free(&b);
  }
  toast(0, left == 0 ? "Merge completed: %s" : "%s written, its conflicts still in it",
        path_basename(M.path));
  merge_close();
  on_disk_changed();
  git_refresh();
  return 1;
}


/* closing the tab: 1 when it may go */
int merge_may_close (void) {
  static const char *const bt[] = {"Complete Merge", "Discard", "Cancel"};
  int left;
  char msg[200];
  if (!M.open) return 1;
  left = merge_left();
  if (left == 0) return merge_complete();
  snprintf(msg, sizeof(msg), "'%s' has %d unresolved conflict%s.", path_basename(M.path), left,
           left == 1 ? "" : "s");
  switch (dialog(msg, "Complete the merge anyway, or leave the file as it is on disk?", bt, 3)) {
    case 0: return merge_complete();
    case 1: merge_close(); return 1;
    default: return 0;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Drawing
** ===================================================================
*/

#define COL_CUR		0x40C8AE	/* VS Code's merge.currentContentBackground */
#define COL_INC		0x40A6FF	/* merge.incomingContentBackground */
#define COL_BASE	0x909090	/* the common version, between ||||||| and ======= */


/* c over bg, pct per cent of it */
static uint32_t mix (uint32_t bg, uint32_t c, int pct) {
  uint32_t r = 0;
  int sh;
  for (sh = 0; sh <= 16; sh += 8) {
    uint32_t a = (bg >> sh) & 255, b = (c >> sh) & 255;
    r |= ((a * (uint32_t)(100 - pct) + b * (uint32_t)pct) / 100) << sh;
  }
  return r;
}


static int num_width (const Doc *d) {
  size_t m = d->n;
  int nw = 2;
  while (m >= 10) {
    m /= 10;
    nw++;
  }
  return nw;
}


/* a line of a pane: its number, its text in the language's colors, a tint over it */
static void draw_line (int x, int y, int w, Doc *d, size_t ln, int nw, uint32_t bg, int cur) {
  static unsigned char *tok;
  static size_t cap;
  char num[24];
  const Row *r;
  int i;
  scr_fill(x, y, w, S_TEXT);
  if (ln >= d->n) {
    scr_fill(x, y, nw, S_GUTTER);
    return;
  }
  r = &d->row[ln];
  snprintf(num, sizeof(num), "%*lu ", nw - 1, (unsigned long)(ln + 1));
  scr_fill(x, y, nw, cur ? S_GUTTER_CUR : S_GUTTER);
  scr_puts(x, y, num, cur ? S_GUTTER_CUR : S_GUTTER);
  if (r->len + 1 > cap) {
    cap = r->len + 256;
    tok = (unsigned char *)xrealloc(tok, cap);
  }
  syntax_line(d, M.sx, ln, tok);
  scr_code(x + nw, y, w - nw, r->s, r->len, 0, tok, B_EDITOR, 0, 0, B_EDITOR);
  for (i = nw; i < w; i++) scr_set_bg(x + i, y, bg);
}


/* the conflict of a side pane that its line ln belongs to; -1: none */
static long block_at (int pane, size_t ln) {
  size_t i;
  for (i = 0; i < M.nblk; i++) {
    size_t a = pane == PANE_INC ? M.blk[i].t0 : M.blk[i].o0;
    size_t b = pane == PANE_INC ? M.blk[i].t1 : M.blk[i].o1;
    if (ln >= a && ln < b) return (long)i;
  }
  return -1;
}


/* " Accept Incoming " ending at x; x0 -1 when there is no room for it */
static void link (int *x0, int *x1, int x, int y, const char *s, int room) {
  int tw = (int)str_cols(s) + 2;
  if (tw > room) {
    *x0 = *x1 = -1;
    return;
  }
  *x0 = x - tw;
  *x1 = x;
  scr_fill(*x0, y, tw, S_TOGGLE_ON);
  scr_puts(*x0 + 1, y, s, S_TOGGLE_ON);
}


static void draw_side_pane (int x, int y, int w, int h, int pane, int focus) {
  Doc *d = pane == PANE_INC ? &M.inc : &M.cur;
  uint32_t base = ui_color(C_EDITOR_BG), col = pane == PANE_INC ? COL_INC : COL_CUR;
  int nw = num_width(d), row, st = focus ? S_TAB_ON : S_TAB;
  char t[160];
  size_t top;
  scr_fill(x, y, w, st);
  snprintf(t, sizeof(t), "%s (%s)", pane == PANE_INC ? "Incoming" : "Current",
           pane == PANE_INC ? M.theirs_name : M.ours_name);
  scr_putsw(x + 1, y, w - 2, t, st);
  M.act_y = y;
  M.act_x0[pane] = M.act_x1[pane] = -1;
  if (M.cb < M.nblk && !M.blk[M.cb].done)
    link(&M.act_x0[pane], &M.act_x1[pane], x + w, y,
         pane == PANE_INC ? "Accept Incoming" : "Accept Current", w - (int)str_cols(t) - 3);
  M.px[pane] = x;
  M.py[pane] = y + 1;
  M.pw[pane] = w;
  M.ph[pane] = h - 1;
  top = M.top[pane];
  if (top + (size_t)(h - 1) > d->n) top = d->n > (size_t)(h - 1) ? d->n - (size_t)(h - 1) : 0;
  M.top[pane] = top;
  for (row = 0; row < h - 1; row++) {
    size_t ln = top + (size_t)row;
    long bi = ln < d->n ? block_at(pane, ln) : -1;
    draw_line(x, y + 1 + row, w, d, ln, nw,
              bi < 0 ? base : mix(base, col, (size_t)bi == M.cb ? 32 : 14), 0);
  }
}


static void draw_result (int x, int y, int w, int h, int focus) {
  uint32_t base = ui_color(C_EDITOR_BG);
  int nw = num_width(&M.res), row, st = focus ? S_TAB_ON : S_TAB, left = merge_left(), room;
  char t[220];
  size_t top, n = 0, i;
  const Conflict *v = conflicts(&M.res, &n);
  scr_fill(x, y, w, st);
  if (left == 0)
    snprintf(t, sizeof(t), "%s \xE2\x80\x94 All conflicts resolved", path_basename(M.path));
  else if (M.cb < M.nblk)
    snprintf(t, sizeof(t), "%s \xE2\x80\x94 %d Conflict%s Remaining (%lu of %lu%s)",
             path_basename(M.path), left, left == 1 ? "" : "s", (unsigned long)(M.cb + 1),
             (unsigned long)M.nblk, ws_only(M.cb) ? ", whitespace only" : "");
  else
    snprintf(t, sizeof(t), "%s \xE2\x80\x94 %d Conflict%s Remaining", path_basename(M.path), left,
             left == 1 ? "" : "s");
  scr_putsw(x + 1, y, w - 2, t, st);
  M.res_y = y;
  room = w - (int)str_cols(t) - 3;
  link(&M.done_x0, &M.done_x1, x + w, y, "Complete Merge", room);
  M.both_x0 = M.both_x1 = -1;
  if (M.done_x0 >= 0 && M.cb < M.nblk && !M.blk[M.cb].done)
    link(&M.both_x0, &M.both_x1, M.done_x0, y, "Accept Combination",
         room - (M.done_x1 - M.done_x0));
  M.px[PANE_RES] = x;
  M.py[PANE_RES] = y + 1;
  M.pw[PANE_RES] = w;
  M.ph[PANE_RES] = h - 1;
  M.rcur = doc_clamp(&M.res, M.rcur);
  if (focus && M.follow) {	/* the cursor stays in view, until the wheel is turned */
    if (M.rcur.y < M.top[PANE_RES]) M.top[PANE_RES] = M.rcur.y;
    else if (M.rcur.y >= M.top[PANE_RES] + (size_t)(h - 1))
      M.top[PANE_RES] = M.rcur.y + 1 - (size_t)(h - 1);
  }
  top = M.top[PANE_RES];
  if (top + (size_t)(h - 1) > M.res.n) top = M.res.n > (size_t)(h - 1) ? M.res.n - (size_t)(h - 1) : 0;
  M.top[PANE_RES] = top;
  for (row = 0; row < h - 1; row++) {
    size_t ln = top + (size_t)row;
    uint32_t bg = base;
    int cur = focus && ln == M.rcur.y;
    for (i = 0; i < n; i++) {
      size_t bm = v[i].base != (size_t)-1 ? v[i].base : v[i].mid;
      if (ln < v[i].start || ln > v[i].end) continue;
      bg = mix(base, ln < bm ? COL_CUR : ln > v[i].mid ? COL_INC : COL_BASE,
               ln == v[i].start || ln == v[i].mid || ln == v[i].end || ln == bm ? 32 : 14);
      break;
    }
    if (cur && bg == base) bg = ui_color(C_LINE_BG);
    draw_line(x, y + 1 + row, w, &M.res, ln, nw, bg, cur);
  }
  if (focus && M.rcur.y >= top && M.rcur.y < top + (size_t)(h - 1)) {
    size_t c = 0, k;
    const Row *r = &M.res.row[M.rcur.y];
    for (k = 0; k < M.rcur.x && k < r->len; k++)
      if (((unsigned char)r->s[k] & 0xC0) != 0x80) c++;
    scr_cursor(x + nw + (int)c, y + 1 + (int)(M.rcur.y - top));
  }
}


void merge_draw (int x, int y, int w, int h, int focus) {
  int top_h, hw, row;
  if (!M.open) return;
  if (w < 44 || h < 12) {
    scr_box(x, y, w, h, S_TEXT);
    scr_putsw(x + 1, y + h / 2, w - 2, "The merge editor needs a larger window", S_TEXT);
    return;
  }
  top_h = h / 2;
  if (h - top_h < 6) top_h = h - 6;
  hw = (w - 1) / 2;
  draw_side_pane(x, y, hw, top_h, PANE_INC, focus && M.focus == PANE_INC);
  for (row = 0; row < top_h; row++) scr_put(x + hw, y + row, 0x2502, S_DIFF_FILL);
  draw_side_pane(x + hw + 1, y, w - hw - 1, top_h, PANE_CUR, focus && M.focus == PANE_CUR);
  draw_result(x, y + top_h, w, h - top_h, focus && M.focus == PANE_RES);
}

/* }================================================================== */


/*
** {==================================================================
** Keys
** ===================================================================
*/

static void res_backspace (void) {
  Pos a = M.rcur;
  if (a.x > 0) {
    a.x--;
    while (a.x > 0 && ((unsigned char)M.res.row[a.y].s[a.x] & 0xC0) == 0x80) a.x--;
  }
  else if (a.y > 0) {
    a.y--;
    a.x = M.res.row[a.y].len;
  }
  else return;
  doc_delete(&M.res, a, M.rcur);
  M.rcur = a;
}


static void res_delete (void) {
  Pos b = M.rcur;
  const Row *r = &M.res.row[b.y];
  if (b.x < r->len) {
    size_t len = 1;
    utf8_decode(r->s + b.x, r->len - b.x, &len);
    b.x += len;
  }
  else if (b.y + 1 < M.res.n) {
    b.y++;
    b.x = 0;
  }
  else return;
  doc_delete(&M.res, M.rcur, b);
}


static void res_key (int k) {
  int code = KEY_CODE(k);
  const Row *r = &M.res.row[M.rcur.y];
  int page = M.ph[PANE_RES] > 1 ? M.ph[PANE_RES] - 1 : 1;
  if (IS_TEXT(k)) {
    char u[4];
    int n = utf8_encode((uint32_t)k, u);
    if (M.run != 1) doc_group(&M.res);	/* a run of typing is one step */
    M.run = 1;
    M.rcur = doc_insert(&M.res, M.rcur, u, (size_t)n);
    return;
  }
  if (code == K_BS || code == K_DEL) {
    if (M.run != 2) doc_group(&M.res);
    M.run = 2;
  }
  else M.run = 0;
  switch (code) {
    case K_ENTER:
      M.rcur = doc_insert(&M.res, M.rcur, "\n", 1);
      break;
    case K_BS: res_backspace(); break;
    case K_DEL: res_delete(); break;
    case K_LEFT:
      if (M.rcur.x > 0) {
        M.rcur.x--;
        while (M.rcur.x > 0 && ((unsigned char)r->s[M.rcur.x] & 0xC0) == 0x80) M.rcur.x--;
      }
      else if (M.rcur.y > 0) {
        M.rcur.y--;
        M.rcur.x = M.res.row[M.rcur.y].len;
      }
      break;
    case K_RIGHT:
      if (M.rcur.x < r->len) {
        size_t len = 1;
        utf8_decode(r->s + M.rcur.x, r->len - M.rcur.x, &len);
        M.rcur.x += len;
      }
      else if (M.rcur.y + 1 < M.res.n) {
        M.rcur.y++;
        M.rcur.x = 0;
      }
      break;
    case K_UP: if (M.rcur.y > 0) M.rcur.y--; break;
    case K_DOWN: M.rcur.y++; break;
    case K_HOME: M.rcur.x = 0; break;
    case K_END: M.rcur.x = r->len; break;
    case K_PGUP: M.rcur.y = M.rcur.y > (size_t)page ? M.rcur.y - (size_t)page : 0; break;
    case K_PGDN: M.rcur.y += (size_t)page; break;
    default: break;
  }
  M.rcur = doc_clamp(&M.res, M.rcur);
}


/* a key while the merge editor is in front */
void merge_key (int k, PageAct *a) {
  int code = KEY_CODE(k);
  a->what = PA_NONE;
  if (!M.open) return;
  if (M.focus != PANE_RES) M.run = 0;
  if (code == K_TAB) {	/* round the panes */
    M.focus = (k & KM_SHIFT) ? (M.focus + 2) % 3 : (M.focus + 1) % 3;
    return;
  }
  if (code == K_F7) {
    merge_next((k & KM_SHIFT) != 0);
    return;
  }
  if (k == (K_ENTER | KM_CTRL) || k == CTRL('s')) {
    if (merge_complete()) {
      a->what = PA_CMD;
      a->cmd = CMD_CLOSE;
    }
    return;
  }
  if (code == K_ESC) {
    if (merge_may_close()) {
      a->what = PA_CMD;
      a->cmd = CMD_CLOSE;
    }
    return;
  }
  if (M.focus != PANE_RES) {	/* a side: Enter takes it, the arrows scroll */
    int pane = M.focus, page = M.ph[pane] > 1 ? M.ph[pane] - 1 : 1;
    if (code == K_ENTER || k == ' ') {
      if (M.cb < M.nblk && !M.blk[M.cb].done) accept_block(M.cb, pane == PANE_INC ? 1 : 0);
      return;
    }
    switch (code) {
      case K_UP: if (M.top[pane] > 0) M.top[pane]--; break;
      case K_DOWN: M.top[pane]++; break;
      case K_PGUP: M.top[pane] = M.top[pane] > (size_t)page ? M.top[pane] - (size_t)page : 0; break;
      case K_PGDN: M.top[pane] += (size_t)page; break;
      case K_HOME: M.top[pane] = 0; break;
      default: break;
    }
    return;
  }
  if (k == CTRL('z') || k == CTRL('y')) {
    if (k == CTRL('z')) doc_undo(&M.res, &M.rcur);
    else doc_redo(&M.res, &M.rcur);
    M.rcur = doc_clamp(&M.res, M.rcur);
    M.run = 0;
    resync();
    if (M.cb < M.nblk && M.blk[M.cb].done) {
      long nx = next_open(0, 0);
      if (nx >= 0) M.cb = (size_t)nx;
    }
    return;
  }
  M.follow = 1;
  res_key(k);
}


/* the merge commands, while the merge editor is the one in front */
void merge_command (int cmd, PageAct *a) {
  a->what = PA_NONE;
  if (!M.open) return;
  switch (cmd) {
    case CMD_MERGE_NEXT: merge_next(0); break;
    case CMD_MERGE_PREV: merge_next(1); break;
    case CMD_MERGE_CURRENT: if (M.cb < M.nblk) accept_block(M.cb, 0); break;
    case CMD_MERGE_INCOMING: if (M.cb < M.nblk) accept_block(M.cb, 1); break;
    case CMD_MERGE_BOTH: if (M.cb < M.nblk) accept_block(M.cb, 2); break;
    case CMD_MERGE_ALL_CURRENT:
    case CMD_MERGE_ALL_INCOMING:
    case CMD_MERGE_ALL_BOTH: {
      int which = cmd == CMD_MERGE_ALL_CURRENT ? 0 : cmd == CMD_MERGE_ALL_INCOMING ? 1 : 2;
      size_t i;
      for (i = M.nblk; i-- > 0;)	/* from the last: the regions before it keep their lines */
        if (!M.blk[i].done) accept_block(i, which);
      M.cb = 0;
      break;
    }
    case CMD_MERGE_COMPLETE:
      if (merge_complete()) {
        a->what = PA_CMD;
        a->cmd = CMD_CLOSE;
      }
      break;
    case CMD_MERGE_COMPARE: {	/* the two sides of this conflict, in the diff editor */
      Buf o, t;
      char *os, *ts, title[300];
      size_t no, nt;
      if (M.cb >= M.nblk) break;
      buf_init(&o);
      buf_init(&t);
      put_lines(&o, &M.cur, M.blk[M.cb].o0, M.blk[M.cb].o1);
      put_lines(&t, &M.inc, M.blk[M.cb].t0, M.blk[M.cb].t1);
      no = o.len;
      nt = t.len;
      os = buf_take(&o);
      ts = buf_take(&t);
      snprintf(title, sizeof(title), "%s (Current \xE2\x86\x94 Incoming)", path_basename(M.path));
      diff_open_texts(M.path, os, no, ts, nt, title);
      free(os);
      free(ts);
      break;
    }
    default: break;
  }
}

/* }================================================================== */


/*
** {==================================================================
** The mouse
** ===================================================================
*/

static int in_pane (const Mouse *m, int pane) {
  return m->x >= M.px[pane] && m->x < M.px[pane] + M.pw[pane] &&
         m->y >= M.py[pane] && m->y < M.py[pane] + M.ph[pane];
}


void merge_mouse (const Mouse *m, PageAct *a) {
  int pane;
  a->what = PA_NONE;
  if (!M.open) return;
  if (m->wheel) {
    int step = wheel_step(m->mods);
    for (pane = 0; pane < 3; pane++) {
      if (!in_pane(m, pane)) continue;
      if (m->wheel < 0) M.top[pane] = M.top[pane] > (size_t)step ? M.top[pane] - (size_t)step : 0;
      else M.top[pane] += (size_t)step;
      M.follow = 0;
      return;
    }
    return;
  }
  if (m->button != 0 || !m->press || m->drag) return;
  if (m->y == M.res_y) {
    if (M.done_x0 >= 0 && m->x >= M.done_x0 && m->x < M.done_x1) {
      if (merge_complete()) {
        a->what = PA_CMD;
        a->cmd = CMD_CLOSE;
      }
      return;
    }
    if (M.both_x0 >= 0 && m->x >= M.both_x0 && m->x < M.both_x1) {
      if (M.cb < M.nblk && !M.blk[M.cb].done) accept_block(M.cb, 2);
      return;
    }
  }
  if (m->y == M.act_y)
    for (pane = 0; pane < 2; pane++)
      if (M.act_x0[pane] >= 0 && m->x >= M.act_x0[pane] && m->x < M.act_x1[pane]) {
        if (M.cb < M.nblk && !M.blk[M.cb].done) accept_block(M.cb, pane == PANE_INC ? 1 : 0);
        return;
      }
  for (pane = 0; pane < 3; pane++) {
    if (!in_pane(m, pane)) continue;
    M.focus = pane;
    if (pane != PANE_RES) {
      long bi = block_at(pane, M.top[pane] + (size_t)(m->y - M.py[pane]));
      if (bi >= 0) M.cb = (size_t)bi;
      return;
    }
    {	/* the Result: the cursor goes there, and a click in a conflict picks it */
      size_t n = 0, i, k = 0, bi;
      const Conflict *v;
      Pos p;
      int nw = num_width(&M.res);
      p.y = M.top[PANE_RES] + (size_t)(m->y - M.py[PANE_RES]);
      p.x = m->x > M.px[PANE_RES] + nw ? (size_t)(m->x - M.px[PANE_RES] - nw) : 0;
      M.rcur = doc_clamp(&M.res, p);
      M.follow = 1;
      v = conflicts(&M.res, &n);
      for (i = 0; i < n; i++) {
        if (M.rcur.y < v[i].start || M.rcur.y > v[i].end) continue;
        for (bi = 0; bi < M.nblk; bi++) {
          if (M.blk[bi].done) continue;
          if (k == i) {
            M.cb = bi;
            break;
          }
          k++;
        }
        break;
      }
    }
    return;
  }
}

/* }================================================================== */
