/*
** eterm.c - the terminal: raw mode, the alternate screen, keys, mouse
**
** Keys come in as bytes and escape sequences (VT / xterm), on Windows
** too: mos.c turns on ENABLE_VIRTUAL_TERMINAL_INPUT in raw mode. Mouse
** reports are SGR (1006), pastes are bracketed (2004).
*/

#include "mme.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


Mouse term_mouse;

static int g_open = 0;

#define ESC_WAIT	30	/* ms: an ESC with nothing after it is the Esc key */


/*
** Zoom In / Out / Reset: the font is the terminal's, so mme can only ask
** for another size, with xterm's OSC 50 ("#+1" one step up, "#-1" down,
** "#0" the configured one). A terminal that does not know it keeps its
** font (mmc-term zooms with its own Ctrl+= / Ctrl+- / Ctrl+0 instead).
*/
void term_font (int delta) {
  char s[32];
  int n = snprintf(s, sizeof(s), "\033]50;#%s\033\\", delta > 0 ? "+1" : delta < 0 ? "-1" : "0");
  term_write(s, (size_t)n);
}


void term_write (const char *s, size_t n) {
  os_write(1, s, n);
}


static void term_puts (const char *s) {
  term_write(s, strlen(s));
}


int term_open (void) {
  if (!os_is_tty(0) || !os_is_tty(1)) return -1;
  if (os_tty_raw(1) != 0) return -1;
  g_open = 1;
  term_puts("\033[?1049h"	/* the alternate screen */
            "\033[?1000h\033[?1003h\033[?1006h"	/* mouse: clicks, drags, moves (hover), SGR */
            "\033[?2004h"	/* bracketed paste */
            "\033[>1u"	/* kitty keys: Ctrl+Shift+P is not Ctrl+P */
            "\033[5 q"	/* a blinking bar, like VS Code */
            "\033[?2;1;0S");	/* how many pixels a picture may have (sixel) */
  return 0;
}


void term_close (void) {
  if (!g_open) return;
  g_open = 0;
  term_puts("\033]22;default\033\\"	/* the mouse pointer back to the terminal's own */
            "\033[0m\033[0 q\033[<u\033[?2004l"
            "\033[?1006l\033[?1003l\033[?1000l"
            "\033[?25h\033[?1049l");
  os_tty_raw(0);
}


void term_size (int *cols, int *rows) {
  *cols = os_term_cols();
  *rows = os_term_rows();
  if (*cols < 1) *cols = 80;
  if (*rows < 1) *rows = 24;
}


/* 1: a byte can be read now, 0: nothing came in ms (ms < 0: wait) */
static int ready (int ms) {
#ifdef _WIN32
  /* the console handle is also signaled by focus and key-up events, and
  ** ReadConsole would then block: drop those, look only for typed keys
  ** (with VT input every key comes as characters, PgDn as ESC [ 6 ~) */
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD start = GetTickCount();
  for (;;) {
    INPUT_RECORD rec[32];
    DWORD n = 0, i, wait = INFINITE;
    if (!PeekConsoleInputW(h, rec, 32, &n)) return 1;
    for (i = 0; i < n; i++) {
      KEY_EVENT_RECORD *k = &rec[i].Event.KeyEvent;
      if (rec[i].EventType == KEY_EVENT && k->bKeyDown && k->uChar.UnicodeChar != 0)
        return 1;
    }
    if (n > 0) {
      ReadConsoleInputW(h, rec, n, &n);
      continue;
    }
    if (ms >= 0) {
      DWORD gone = GetTickCount() - start;
      if (gone >= (DWORD)ms) return 0;
      wait = (DWORD)ms - gone;
    }
    if (WaitForSingleObject(h, wait) != WAIT_OBJECT_0) return 0;
  }
#else
  return os_wait_readable(0, ms) == 1;
#endif
}


static int getb (int ms) {
  int c;
  if (ms >= 0 && !ready(ms)) return -1;
  c = os_tty_getbyte();
#ifdef _WIN32
  /* a lone Ctrl-Z is "end of input" to mos.c: -1 first, then the 26 */
  if (c < 0) c = os_tty_getbyte();
#endif
  if (c < 0) exit(1);	/* the terminal is gone */
  return c;
}


/* the modifier number of "CSI 1 ; m X": 1 + shift 1, alt 2, ctrl 4 */
static int mods_of (int m) {
  int k = 0;
  if (m < 2) return 0;
  m--;
  if (m & 1) k |= KM_SHIFT;
  if (m & 2) k |= KM_ALT;
  if (m & 4) k |= KM_CTRL;
  return k;
}


static int tilde_key (int n) {
  switch (n) {
    case 1: case 7: return K_HOME;
    case 2: return K_INS;
    case 3: return K_DEL;
    case 4: case 8: return K_END;
    case 5: return K_PGUP;
    case 6: return K_PGDN;
    case 11: return K_F1;
    case 12: return K_F2;
    case 13: return K_F3;
    case 14: return K_F4;
    case 15: return K_F5;
    case 17: return K_F6;
    case 18: return K_F7;
    case 19: return K_F8;
    case 20: return K_F9;
    case 21: return K_F10;
    case 23: return K_F11;
    case 24: return K_F12;
    case 200: return K_PASTE;
  }
  return K_NONE;
}


static int mouse_key (int *p, int np, int final) {
  int b;
  if (np < 3) return K_NONE;
  b = p[0];
  memset(&term_mouse, 0, sizeof(term_mouse));
  term_mouse.x = p[1] - 1;
  term_mouse.y = p[2] - 1;
  if (b & 4) term_mouse.mods |= KM_SHIFT;
  if (b & 8) term_mouse.mods |= KM_ALT;
  if (b & 16) term_mouse.mods |= KM_CTRL;
  term_mouse.drag = (b & 32) != 0;
  if (b & 64) {
    term_mouse.wheel = (b & 1) ? 1 : -1;
    term_mouse.button = 3;
  }
  else {
    term_mouse.button = b & 3;
    term_mouse.press = (final == 'M');
  }
  return K_MOUSE;
}


/*
** CSI code ; mods u: the kitty keyboard protocol, which tells Ctrl+Shift+P
** from Ctrl+P. What the old encoding can say comes back the old way, so
** Ctrl+S is still CTRL('s'); only what it can't gets the KM_ bits.
*/
static int kitty_key (int code, int k) {
  int ctrl = k & KM_CTRL, shift = k & KM_SHIFT;
  if (code == 27) return K_ESC | k;
  if (code == 13) return K_ENTER | k;
  if (code == 9) return K_TAB | k;
  if (code == 127 || code == 8) return K_BS | k;
  if (code >= 'A' && code <= 'Z') code += 32;
  if (ctrl && !shift && code >= 'a' && code <= 'z' && code != 'm' && code != 'i')	/* Ctrl+M is not Enter here (Ctrl+K Ctrl+M), Ctrl+I not Tab (Inline Chat) */
    return CTRL(code) | (k & KM_ALT);
  if (!ctrl && !(k & KM_ALT)) return (shift && code >= 'a' && code <= 'z') ? code - 32 : code;
  return code | k;
}


/* after ESC [ or ESC O */
static int g_px_w, g_px_h;	/* the screen in pixels, as XTSMGRAPHICS answered */


/*
** How big a cell is: the terminal says how many pixels a full screen of
** images may be (CSI ? 2 ; 1 ; 0 S), which over the columns and rows is
** the size of one cell. 0 when it did not answer (no pictures then).
*/
void term_ask_pixels (void) {
  term_puts("\033[?2;1;0S");
}


int term_cell_px (int *w, int *h) {
  int cols, rows;
  term_size(&cols, &rows);
  if (g_px_w <= 0 || g_px_h <= 0 || cols <= 0 || rows <= 0) return 0;
  *w = g_px_w / cols;
  *h = g_px_h / rows;
  return *w > 0 && *h > 0;
}


static int read_csi (void) {
  int p[8], np = 0, c, lt = 0, qm = 0, k;
  memset(p, 0, sizeof(p));
  for (;;) {
    c = getb(ESC_WAIT);
    if (c < 0) return K_NONE;
    if (c == '<') lt = 1;
    else if (c == '?') qm = 1;
    else if (c >= '0' && c <= '9') {
      if (np == 0) np = 1;
      /* CSI 99999999999H: a parameter of any length must not run the int
      ** over, and a terminal's are far smaller than this */
      if (np <= 8 && p[np - 1] < 100000) p[np - 1] = p[np - 1] * 10 + (c - '0');
    }
    else if (c == ';') {
      if (np == 0) np = 1;
      np++;
    }
    else if (c >= 0x40 && c <= 0x7E) break;
  }
  if (np > 8) np = 8;
  if (lt) return mouse_key(p, np, c);
  if (qm && c == 'S') {	/* CSI ? 2 ; 0 ; w ; h S: the pixels a picture may have */
    if (np >= 4 && p[0] == 2 && p[1] == 0) {
      g_px_w = p[2];
      g_px_h = p[3];
    }
    return K_NONE;
  }
  if (qm) return K_NONE;	/* another answer of the terminal, not a key */
  k = (np >= 2) ? mods_of(p[1]) : 0;
  switch (c) {
    case 'A': return K_UP | k;
    case 'B': return K_DOWN | k;
    case 'C': return K_RIGHT | k;
    case 'D': return K_LEFT | k;
    case 'H': return K_HOME | k;
    case 'F': return K_END | k;
    case 'P': return K_F1 | k;
    case 'Q': return K_F2 | k;
    case 'R': return K_F3 | k;
    case 'S': return K_F4 | k;
    case 'Z': return K_TAB | KM_SHIFT;
    case 'u': return kitty_key(p[0], k);
    case '~': {
      int key = tilde_key(p[0]);
      return key == K_NONE ? K_NONE : key | k;
    }
  }
  return K_NONE;
}


/* the rest of a UTF-8 character that started with c */
static int read_utf8 (int c) {
  char s[4];
  size_t n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1, i, len;
  uint32_t cp;
  s[0] = (char)c;
  for (i = 1; i < n; i++) {
    int b = os_tty_getbyte();
    if (b < 0) return 0xFFFD;
    s[i] = (char)b;
  }
  cp = utf8_decode(s, n, &len);
  return (int)cp;
}


/*
** A wheel notch is several events in some terminals (mmc-term sends three),
** so one notch would scroll three times as far as in another. The events of
** a notch come together: the ones already waiting are read and counted as
** one, and the key read past them is kept for the next call.
*/
static int g_pending = K_NONE;	/* the key read while draining a notch */
static Mouse g_pending_mouse;


static int read_key (int ms);

int term_key (int ms) {
  int k;
  if (g_pending != K_NONE) {	/* what was read ahead */
    k = g_pending;
    g_pending = K_NONE;
    if (KEY_CODE(k) == K_MOUSE) term_mouse = g_pending_mouse;
    return k;
  }
  k = read_key(ms);
  if (KEY_CODE(k) == K_MOUSE && term_mouse.wheel) {
    Mouse notch = term_mouse;
    while (ready(0)) {
      int n = read_key(0);
      if (n == K_NONE) break;
      if (KEY_CODE(n) == K_MOUSE && term_mouse.wheel == notch.wheel) continue;	/* the same notch */
      g_pending = n;
      g_pending_mouse = term_mouse;
      break;
    }
    term_mouse = notch;
  }
  return k;
}


static int read_key (int ms) {
  int c = getb(ms);
  if (c < 0) return K_NONE;
  if (c == 27) {
    c = getb(ESC_WAIT);
    if (c < 0) return K_ESC;
    if (c == '[' || c == 'O') return read_csi();
    if (c == 27) return K_ESC;
    if (c == 127 || c == 8) return K_BS | KM_ALT;
    if (c >= 0x80) return read_utf8(c) | KM_ALT;
    return c | KM_ALT;
  }
  if (c == 8) return K_BS | KM_CTRL;	/* Ctrl-Backspace sends ^H */
  if (c == 10) return K_ENTER;
  if (c >= 0x80) return read_utf8(c);
  return c;
}


void term_paste (Buf *b) {
  static const char end[] = "\033[201~";
  size_t match = 0;
  int c, cr = 0;
  while (match < sizeof(end) - 1 && (c = os_tty_getbyte()) >= 0) {
    if (c == end[match]) {
      match++;
      continue;
    }
    if (match > 0) {	/* it was not the end after all */
      buf_putn(b, end, match);
      match = (c == end[0]);
      if (match) continue;
    }
    if (c == '\r') {
      buf_putc(b, '\n');
      cr = 1;
      continue;
    }
    if (!(c == '\n' && cr)) buf_putc(b, (char)c);	/* CR LF: one newline */
    cr = 0;
  }
}
