/* tcc/tlhelp32.h - the process list mme reads (its parent's id), for tcc 0.9.27 */
#ifndef MME_TCC_TLHELP32_H
#define MME_TCC_TLHELP32_H
#include <windows.h>

#define TH32CS_SNAPPROCESS	0x00000002

typedef struct tagPROCESSENTRY32 {
  DWORD dwSize;
  DWORD cntUsage;
  DWORD th32ProcessID;
  ULONG_PTR th32DefaultHeapID;
  DWORD th32ModuleID;
  DWORD cntThreads;
  DWORD th32ParentProcessID;
  LONG pcPriClassBase;
  DWORD dwFlags;
  CHAR szExeFile[MAX_PATH];
} PROCESSENTRY32;

HANDLE WINAPI CreateToolhelp32Snapshot (DWORD flags, DWORD pid);
BOOL WINAPI Process32First (HANDLE snap, PROCESSENTRY32 *pe);
BOOL WINAPI Process32Next (HANDLE snap, PROCESSENTRY32 *pe);

#endif
