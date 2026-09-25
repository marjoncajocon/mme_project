/*
** etpty.c - tpty.c (mmc's pseudo console) with Vista's calls looked up
** when they are called, not when the program is loaded
**
** tpty.c starts the terminal panel's shell in a pseudo console (ConPTY,
** Windows 10 1809) with a thread attribute list (Vista). Called by name,
** those make Windows XP refuse to start the program at all; asked for
** with GetProcAddress, XP starts it and only the terminal panel says it
** has no pseudo console. tpty.c itself is compiled as it is.
*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef BOOL (WINAPI *InitList) (LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T);
typedef BOOL (WINAPI *UpdateList) (LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR, PVOID, SIZE_T, PVOID, PSIZE_T);
typedef VOID (WINAPI *DeleteList) (LPPROC_THREAD_ATTRIBUTE_LIST);

static FARPROC kernel32_fn (const char *name) {
  HMODULE k = GetModuleHandleA("kernel32.dll");
  return k ? GetProcAddress(k, name) : NULL;
}


static BOOL WINAPI init_list (LPPROC_THREAD_ATTRIBUTE_LIST l, DWORD n, DWORD flags, PSIZE_T size) {
  static InitList fn;
  if (fn == NULL) fn = (InitList)(void (*) (void))kernel32_fn("InitializeProcThreadAttributeList");
  if (fn == NULL) {	/* before Vista: none, and a size to go on with */
    if (size) *size = 64;
    return FALSE;
  }
  return fn(l, n, flags, size);
}


static BOOL WINAPI update_list (LPPROC_THREAD_ATTRIBUTE_LIST l, DWORD flags, DWORD_PTR attr, PVOID value, SIZE_T size,
                                PVOID prev, PSIZE_T ret) {
  static UpdateList fn;
  if (fn == NULL) fn = (UpdateList)(void (*) (void))kernel32_fn("UpdateProcThreadAttribute");
  return fn ? fn(l, flags, attr, value, size, prev, ret) : FALSE;
}


static VOID WINAPI delete_list (LPPROC_THREAD_ATTRIBUTE_LIST l) {
  static DeleteList fn;
  if (fn == NULL) fn = (DeleteList)(void (*) (void))kernel32_fn("DeleteProcThreadAttributeList");
  if (fn) fn(l);
}

#define InitializeProcThreadAttributeList	init_list
#define UpdateProcThreadAttribute	update_list
#define DeleteProcThreadAttributeList	delete_list
#endif

#include "../tpty.c"
