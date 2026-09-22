/*
** ewelcome.c - the Welcome page, VS Code's
**
** At startup when no file is opened (workbench.startupEditor), and from
** Help > Welcome: the name, Start (new file, open file or folder), the
** folders opened lately, and a walkthrough of the keys worth knowing. The
** checkbox at the bottom is workbench.startupEditor.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { W_START, W_RECENT, W_MORE, W_TIP, W_SHOW };

typedef struct Item {
  int kind, cmd;
  int recent;	/* W_RECENT: which */
  int x, y, w;	/* where it was drawn */
} Item;

#define MAXITEM	32
#define RECENT_SHOWN	5

static struct {
  Item it[MAXITEM];
  int n, sel;
} WL;


static const struct {
  int cmd;
  uint32_t icon;
} start[] = {
  {CMD_NEW, 0xEA7F},	/* codicon new-file */
  {CMD_OPEN_FILE, 0xEA94},	/* go-to-file */
  {CMD_OPEN_FOLDER, 0xEA83},	/* folder-opened */
  {CMD_OPEN_PROJECT, 0xEA82}	/* history */
};

/* the walkthrough: what VS Code's "Learn the Fundamentals" teaches */
static const struct {
  int cmd;
  const char *what;
} tips[] = {
  {CMD_PALETTE, "Find and run every command"},
  {CMD_QUICK_OPEN, "Open a file of the folder by its name"},
  {CMD_TERMINAL, "A terminal, under the editor"},
  {CMD_SPLIT, "Two files side by side"},
  {CMD_GIT, "Review and commit your changes"},
  {CMD_THEME, "Pick the colors you like"},
  {CMD_SETTINGS, "Make mme yours"},
  {CMD_KEYS, "Change any shortcut"}
};


static int add (int kind, int cmd, int recent, int x, int y, int w) {
  if (WL.n == MAXITEM) return 0;
  WL.it[WL.n].kind = kind;
  WL.it[WL.n].cmd = cmd;
  WL.it[WL.n].recent = recent;
  WL.it[WL.n].x = x;
  WL.it[WL.n].y = y;
  WL.it[WL.n].w = w;
  WL.n++;
  return 1;
}


static int bold (int x, int y, int w, const char *s, uint32_t fg) {
  uint32_t bg = ui_color(C_EDITOR_BG);
  size_t i = 0, n = strlen(s), len;
  int c = 0;
  while (i < n && c < w) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    c += scr_put_rgb(x + c, y, cp, fg, bg, RGB_BOLD);
    i += len;
  }
  return c;
}


/* a link: accent colored, the selected one underlined */
static void link (int x, int y, int w, const char *s, int on) {
  uint32_t fg = ui_color(C_ACCENT), bg = ui_color(C_EDITOR_BG);
  size_t i = 0, n = strlen(s), len;
  int c = 0;
  while (i < n && c < w) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    c += scr_put_rgb(x + c, y, cp, fg, bg, on ? RGB_UNDER : 0);
    i += len;
  }
}


void welcome_draw (int x, int y, int w, int h, int focus, const Vec *recent) {
  int cw, lx, rx, ly, ry, i, two, sel_kept = WL.sel;
  scr_box(x, y, w, h, S_TEXT);
  WL.n = 0;
  if (w < 24 || h < 6) return;
  two = w >= 90;
  cw = two ? (w - 8) / 2 : w - 6;
  if (cw > 56) cw = 56;
  lx = x + (two ? (w - 2 * cw - 6) / 2 : (w - cw) / 2);
  if (lx < x + 2) lx = x + 2;
  rx = lx + cw + 6;
  ly = y + (h > 30 ? 3 : 1);
  bold(lx, ly, cw, MME_NAME, ui_color(C_EDITOR_FG));
  scr_puts(lx + 4, ly, MME_VERSION, S_CRUMB);
  scr_putsw(lx, ly + 1, cw, "Editing evolved, in the terminal", S_CRUMB);
  ly += 4;
  bold(lx, ly++, cw, "Start", ui_color(C_EDITOR_FG));
  for (i = 0; i < (int)(sizeof(start) / sizeof(start[0])) && ly < y + h; i++, ly++) {
    int on = focus && WL.n == sel_kept;
    scr_put_rgb(lx, ly, start[i].icon, ui_color(C_ACCENT), ui_color(C_EDITOR_BG), 0);
    link(lx + 2, ly, cw - 2, cmd_name(start[i].cmd), on);
    add(W_START, start[i].cmd, -1, lx, ly, cw);
  }
  if (import_offered() && ly < y + h) {	/* VS Code is there: its settings can come */
    int on = focus && WL.n == sel_kept;
    scr_put_rgb(lx, ly, 0xEB59, ui_color(C_ACCENT), ui_color(C_EDITOR_BG), 0);	/* codicon settings-sync */
    link(lx + 2, ly, cw - 2, "Import VS Code Settings...", on);
    add(W_START, CMD_IMPORT_VSCODE, -1, lx, ly++, cw);
  }
  ly++;
  if (ly < y + h) bold(lx, ly++, cw, "Recent", ui_color(C_EDITOR_FG));
  if (recent->n == 0 && ly < y + h) scr_putsw(lx, ly++, cw, "You have no recent folders, open a folder to start.", S_CRUMB);
  for (i = 0; i < (int)recent->n && i < RECENT_SHOWN && ly < y + h; i++, ly++) {
    const char *p = recent->v[i], *name = path_basename(p);
    int on = focus && WL.n == sel_kept, nw = (int)str_cols(name);
    link(lx, ly, cw, name, on);
    if (nw + 3 < cw) scr_putsw(lx + nw + 3, ly, cw - nw - 3, p, S_CRUMB);
    add(W_RECENT, 0, i, lx, ly, cw);
  }
  if ((int)recent->n > RECENT_SHOWN && ly < y + h) {
    link(lx, ly, cw, "More...", focus && WL.n == sel_kept);
    add(W_MORE, CMD_OPEN_PROJECT, -1, lx, ly++, cw);
  }
  /* the walkthrough: on the right, or under it all when narrow */
  if (two) ry = y + (h > 30 ? 3 : 1) + 4;
  else {
    ry = ly + 1;
    rx = lx;
  }
  if (ry < y + h) bold(rx, ry++, cw, "Walkthrough: Learn the Fundamentals", ui_color(C_EDITOR_FG));
  for (i = 0; i < (int)(sizeof(tips) / sizeof(tips[0])) && ry < y + h; i++, ry++) {
    const char *keys = cmd_keys(tips[i].cmd);
    int on = focus && WL.n == sel_kept, c;
    scr_put_rgb(rx, ry, 0xEB32, ui_color(C_ACCENT), ui_color(C_EDITOR_BG), 0);	/* codicon star-empty */
    link(rx + 2, ry, cw - 2, cmd_name(tips[i].cmd), on);
    c = 2 + (int)str_cols(cmd_name(tips[i].cmd)) + 2;
    if (keys[0] && c + (int)strlen(keys) + 2 < cw) {
      scr_fill(rx + c, ry, (int)strlen(keys) + 2, S_TAB);
      scr_puts(rx + c + 1, ry, keys, S_TAB);
      c += (int)strlen(keys) + 3;
    }
    if (c < cw) scr_putsw(rx + c, ry, cw - c, tips[i].what, S_CRUMB);
    add(W_TIP, tips[i].cmd, -1, rx, ry, cw);
  }
  if (h > 8) {	/* the checkbox, at the bottom in the middle */
    const char *t = "Show welcome page on startup";
    int bx = x + (w - (int)strlen(t) - 2) / 2, by = y + h - 2, on = focus && WL.n == sel_kept;
    if (by > ry && by > ly) {
      scr_put_rgb(bx, by, opt.startup_welcome ? 0xEAB2 : ' ', ui_color(C_INPUT_FG), ui_color(C_INPUT_BG), 0);
      if (on) link(bx + 2, by, (int)strlen(t), t, 1);
      else scr_puts(bx + 2, by, t, S_TEXT);
      add(W_SHOW, 0, -1, bx, by, (int)strlen(t) + 2);
    }
  }
  if (WL.sel >= WL.n) WL.sel = WL.n ? WL.n - 1 : 0;
}


static void use (int i, const Vec *recent, PageAct *a) {
  const Item *it = &WL.it[i];
  a->what = PA_NONE;
  switch (it->kind) {
    case W_RECENT:
      if (it->recent < (int)recent->n) {
        a->what = PA_FOLDER;
        a->text = recent->v[it->recent];
      }
      break;
    case W_SHOW:
      settings_put("workbench.startupEditor", opt.startup_welcome ? "none" : "welcomePage");
      a->what = PA_APPLY;
      break;
    default:
      a->what = PA_CMD;
      a->cmd = it->cmd;
  }
}


void welcome_key (int k, const Vec *recent, PageAct *a) {
  int code = KEY_CODE(k);
  a->what = PA_NONE;
  if (WL.n == 0) return;
  if (code == K_UP || (code == K_TAB && (k & KM_SHIFT))) WL.sel = (WL.sel + WL.n - 1) % WL.n;
  else if (code == K_DOWN || code == K_TAB) WL.sel = (WL.sel + 1) % WL.n;
  else if (code == K_HOME) WL.sel = 0;
  else if (code == K_END) WL.sel = WL.n - 1;
  else if (code == K_ENTER || code == ' ') use(WL.sel, recent, a);
}


void welcome_mouse (const Mouse *m, const Vec *recent, PageAct *a) {
  int i;
  a->what = PA_NONE;
  if (!(m->button == 0 && m->press && !m->drag)) return;
  for (i = 0; i < WL.n; i++)
    if (m->y == WL.it[i].y && m->x >= WL.it[i].x && m->x < WL.it[i].x + WL.it[i].w) {
      WL.sel = i;
      use(i, recent, a);
      return;
    }
}
