/*
** rundll32.c - a stand-in for Windows' rundll32, for the regression suite.
**
** mme opens a URL in the browser with
**
**     rundll32 url.dll,FileProtocolHandler <url>
**
** and find_program() takes the first rundll32 on the PATH. The suite puts
** this directory in front of the PATH for the scenarios that drive the
** GitHub Copilot device flow, so signing in never opens a real browser
** window. The URL is appended to the file named by MME_URLLOG, which is
** how a scenario can be checked by hand to have handed over the right one.
*/
#include <stdio.h>
#include <stdlib.h>

int main (int argc, char **argv) {
  const char *log = getenv("MME_URLLOG");
  FILE *f;
  int i;
  if (log == NULL || (f = fopen(log, "a")) == NULL) return 0;
  for (i = 1; i < argc; i++) fprintf(f, "%s%s", i > 1 ? " " : "", argv[i]);
  fputc('\n', f);
  fclose(f);
  return 0;
}
