/*
** tcc/stdio.h - C99's snprintf and vsnprintf for tcc 0.9.27, whose come
** from msvcrt.dll: those return -1 and leave the text unterminated when
** it is cut, and mme cuts text into fixed buffers all the time.
*/
#ifndef MME_TCC_STDIO_H
#define MME_TCC_STDIO_H
#include_next <stdio.h>
#include <stdarg.h>

static int mme_vsnprintf (char *b, size_t n, const char *f, va_list ap) {
  va_list ap2;
  int r;
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
