/*
 * NTFS3NT - base integer types for building the shared, kernel-free NTFS logic
 * (ntfssup.c) outside the kernel: the user-mode parser, the format/check tools
 * and the test harness. The kernel build uses ntifs.h instead (see ntfs.h).
 */

#ifndef _NTFSUSR_H_
#define _NTFSUSR_H_

#ifdef _WIN32

#include <windows.h>

#else

#include <stdint.h>
typedef void            VOID, *PVOID;
typedef unsigned char   UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN;
typedef char            CHAR, *PCHAR;            /* signed - used for run headers */
typedef uint16_t        USHORT, *PUSHORT, WCHAR, *PWCHAR, *PWSTR;
typedef int16_t         SHORT, CSHORT;
typedef uint32_t        ULONG, *PULONG;
typedef int32_t         LONG, *PLONG;
typedef uint64_t        ULONGLONG, *PULONGLONG;
typedef int64_t         LONGLONG, *PLONGLONG;

#endif /* _WIN32 */

#include <string.h>

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef RtlZeroMemory
#define RtlZeroMemory(d, l)    memset((d), 0, (l))
#define RtlCopyMemory(d, s, l) memcpy((d), (s), (l))
#endif

#endif /* _NTFSUSR_H_ */
