/*
** emos.c - mos.c (the operating system's calls, as mmc has them) for a
** program with a window and no console
**
** mme in a terminal starts git, the language servers, the tests ... in the
** terminal's console, which they share. A window has no console, so each of
** those console programs would open one of its own - a black window popping
** up at every git status. Here they are started with CREATE_NO_WINDOW: a
** console that is never shown (what they read and write goes through pipes
** anyway). mos.c itself is compiled as it is.
*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static BOOL WINAPI spawn_no_window (LPCWSTR app, LPWSTR cl, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                    BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si,
                                    LPPROCESS_INFORMATION pi) {
  return CreateProcessW(app, cl, pa, ta, inherit, flags | CREATE_NO_WINDOW, env, dir, si, pi);
}

#define CreateProcessW	spawn_no_window
#endif

#include "../mos.c"
