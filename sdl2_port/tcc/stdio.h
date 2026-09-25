/*
** tcc/stdio.h - C99's snprintf and vsnprintf for tcc 0.9.27, whose come
** from msvcrt.dll: those return -1 and leave the text unterminated when
** it is cut, and mme cuts text into fixed buffers all the time. Windows
** XP's msvcrt.dll does not know %lld either: it is written %I64d there.
*/
#ifndef MME_TCC_STDIO_H
#define MME_TCC_STDIO_H
#include_next <stdio.h>
#include <stdarg.h>
#include <string.h>

/* f with C99's "ll" as msvcrt's "I64" (in out, n bytes); f itself when it has none or is too long */
static const char *mme_fmt (const char *f, char *out, size_t n) {
  size_t i = 0, o = 0;
  if (strstr(f, "ll") == NULL) return f;
  while (f[i] && o + 4 < n) {
    if (f[i] != '%') {
      out[o++] = f[i++];
      continue;
    }
    out[o++] = f[i++];	/* the % */
    while (f[i] && strchr("-+ #0123456789.*", f[i]) && o + 4 < n) out[o++] = f[i++];
    if (f[i] == 'l' && f[i + 1] == 'l') {
      memcpy(out + o, "I64", 3);
      o += 3;
      i += 2;
    }
  }
  if (f[i]) return f;	/* too long for out: as it is */
  out[o] = '\0';
  return out;
}


static int mme_vsnprintf (char *b, size_t n, const char *f, va_list ap) {
  char fb[512];
  va_list ap2;
  int r;
  f = mme_fmt(f, fb, sizeof(fb));
  va_copy(ap2, ap);
  r = _vscprintf(f, ap2);	/* how long the whole text is */
  va_end(ap2);
  if (n > 0) {
    _vsnprintf(b, n, f, ap);
    if (r < 0) b[0] = '\0';
    else if ((size_t)r >= n) b[n - 1] = '\0';
  }
  return r;
}


static int mme_snprintf (char *b, size_t n, const char *f, ...) {
  va_list ap;
  int r;
  va_start(ap, f);
  r = mme_vsnprintf(b, n, f, ap);
  va_end(ap);
  return r;
}

#undef snprintf
#undef vsnprintf
#define snprintf	mme_snprintf
#define vsnprintf	mme_vsnprintf

#endif
