/*
** tcc/windows.h - what tcc 0.9.27's windows.h lacks and mme uses: winnls's
** UTF-8 conversions. Only the tcc build looks in this folder.
*/
#ifndef MME_TCC_WINDOWS_H
#define MME_TCC_WINDOWS_H
#include_next <windows.h>

#ifndef CP_UTF8
#define CP_ACP	0
#define CP_UTF8	65001
#define MB_ERR_INVALID_CHARS	0x08
WINBASEAPI int WINAPI MultiByteToWideChar (UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR w, int nw);
WINBASEAPI int WINAPI WideCharToMultiByte (UINT cp, DWORD flags, LPCWSTR w, int nw, LPSTR s, int n,
                                           LPCSTR def, LPBOOL used);
#endif

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION	0x1000
#endif

#ifndef EXTENDED_STARTUPINFO_PRESENT	/* Vista's: how tpty.c starts a program in a pseudo console */
#define EXTENDED_STARTUPINFO_PRESENT	0x00080000
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE	0x00020016
typedef struct _PROC_THREAD_ATTRIBUTE_LIST *LPPROC_THREAD_ATTRIBUTE_LIST;
typedef struct _STARTUPINFOEXW {
  STARTUPINFOW StartupInfo;
  LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList;
} STARTUPINFOEXW;
WINBASEAPI BOOL WINAPI InitializeProcThreadAttributeList (LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD count, DWORD flags,
                                                          SIZE_T *size);
WINBASEAPI BOOL WINAPI UpdateProcThreadAttribute (LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags, DWORD_PTR attr,
                                                  PVOID value, SIZE_T size, PVOID prev, SIZE_T *ret);
WINBASEAPI VOID WINAPI DeleteProcThreadAttributeList (LPPROC_THREAD_ATTRIBUTE_LIST list);
#endif

#endif
