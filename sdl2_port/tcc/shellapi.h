/*
** tcc/shellapi.h - the shell calls mme uses (open with the system's
** program, the recycle bin, the command line as UTF-16), for tcc 0.9.27.
*/
#ifndef MME_TCC_SHELLAPI_H
#define MME_TCC_SHELLAPI_H
#include <windows.h>

#define FO_DELETE	3
#define FOF_SILENT	0x0004
#define FOF_NOCONFIRMATION	0x0010
#define FOF_ALLOWUNDO	0x0040
#define FOF_NOERRORUI	0x0400

typedef WORD FILEOP_FLAGS;

typedef struct _SHFILEOPSTRUCTW {
  HWND hwnd;
  UINT wFunc;
  LPCWSTR pFrom;
  LPCWSTR pTo;
  FILEOP_FLAGS fFlags;
  BOOL fAnyOperationsAborted;
  LPVOID hNameMappings;
  LPCWSTR lpszProgressTitle;
} SHFILEOPSTRUCTW;

HINSTANCE WINAPI ShellExecuteW (HWND hwnd, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show);
LPWSTR *WINAPI CommandLineToArgvW (LPCWSTR cmd, int *argc);
int WINAPI SHFileOperationW (SHFILEOPSTRUCTW *op);
#define SHFileOperation	SHFileOperationW

#endif
