/*
** fake.c - stand-ins for wsl.exe and docker.exe, for the regression suite.
**
** The Remote Explorer (eremote.c) lists WSL's distros with `wsl.exe -l -q`
** and the containers with `docker ps -a --format ...`, both found on the
** PATH. The suite builds this twice, as remote\wsl.exe and remote\docker.exe,
** and puts that directory in front of the PATH for the remote-explorer-*
** scenarios, so what they list is fixed and no real WSL or Docker is asked.
** What each says comes from the environment:
**
**   MME_FAKE_WSL      "Ubuntu,Debian": the distros, UTF-16 as wsl.exe
**                     writes them; "none": WSL is not installed; "empty":
**                     WSL has no distribution (both exit 1, as wsl.exe does)
**   MME_FAKE_DOCKER   the lines of docker ps, ';' between lines and '|'
**                     for the tabs of its --format; "down": the daemon is
**                     not running (its error, exit 1)
**
** Each run appends its arguments to MME_FAKE_LOG when that is set.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif


static void utf16 (const char *s) {	/* as wsl.exe writes: UTF-16LE, CRLF */
  for (; *s; s++) {
    if (*s == '\n') {
      putchar('\r');
      putchar(0);
    }
    putchar(*s);
    putchar(0);
  }
}


int main (int argc, char **argv) {
  const char *me = argv[0], *log = getenv("MME_FAKE_LOG"), *p;
  int wsl, i;
  for (p = argv[0]; *p; p++)
    if (*p == '/' || *p == '\\') me = p + 1;
  wsl = (me[0] == 'w' || me[0] == 'W');
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  if (log) {
    FILE *f = fopen(log, "a");
    if (f) {
      fputs(wsl ? "wsl" : "docker", f);
      for (i = 1; i < argc; i++) fprintf(f, " %s", argv[i]);
      fputc('\n', f);
      fclose(f);
    }
  }
  if (wsl) {
    const char *d = getenv("MME_FAKE_WSL");
    if (d == NULL || strcmp(d, "none") == 0) {
      utf16("The Windows Subsystem for Linux is not installed. You can install by running 'wsl.exe --install'.\n"
            "For more information please visit https://aka.ms/wslinstall\n");
      return 1;
    }
    if (strcmp(d, "empty") == 0) {
      utf16("Windows Subsystem for Linux has no installed distributions.\n");
      return 1;
    }
    for (; *d; d++) {
      char c[2] = {*d == ',' ? '\n' : *d, 0};
      utf16(c);
    }
    utf16("\n");
    return 0;
  }
  p = getenv("MME_FAKE_DOCKER");
  if (p == NULL || strcmp(p, "down") == 0) {
    fputs("error during connect: this error may indicate that the docker daemon is not running\n", stderr);
    return 1;
  }
  for (; *p; p++) putchar(*p == ';' ? '\n' : *p == '|' ? '\t' : *p);
  putchar('\n');
  return 0;
}
