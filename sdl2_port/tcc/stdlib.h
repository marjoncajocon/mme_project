/* tcc/stdlib.h - C99's strtoll and strtoull: msvcrt.dll has them as _strtoi64 and _strtoui64 */
#ifndef MME_TCC_STDLIB_H
#define MME_TCC_STDLIB_H
#include_next <stdlib.h>
#ifndef strtoll
#define strtoll	_strtoi64
#define strtoull	_strtoui64
__int64 __cdecl _strtoi64 (const char *s, char **end, int base);
unsigned __int64 __cdecl _strtoui64 (const char *s, char **end, int base);
#endif
#endif
