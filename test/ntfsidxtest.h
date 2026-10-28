/*
 * Minimal kernel environment so src/ntfsidx.c (the directory B-tree) compiles
 * and runs in user mode against an image file - see test/ntfsbtree.c.
 */
#ifndef NTFSIDXTEST_H
#define NTFSIDXTEST_H
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "ntfsusr.h"
#include "ntfsdisk.h"
#include "ntfssup.h"

typedef LONG NTSTATUS;
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS               0
#define STATUS_DISK_FULL             ((NTSTATUS)0xC000007F)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_NOT_A_DIRECTORY       ((NTSTATUS)0xC0000103)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define STATUS_FILE_CORRUPT_ERROR    ((NTSTATUS)0xC0000102)
#define FIELD_OFFSET(t, f) ((ULONG)offsetof(t, f))
#define NonPagedPool 0
#define NTFS_TAG_BUF 0
#define NTFS_TAG_RUN 0
#define ExAllocatePoolWithTag(p, n, t) malloc(n)
#define ExFreePool(x) free(x)
#define NTFS_MAX_IDX_RUNS 256

typedef struct _NTFS_VCB { NTFS_GEOMETRY Geo; } NTFS_VCB, *PNTFS_VCB;

NTSTATUS NtfsReadMftRecord(PNTFS_VCB Vcb, ULONGLONG Rec, PVOID Buf);
NTSTATUS NtfsWriteMftRecord(PNTFS_VCB Vcb, ULONGLONG Rec, PVOID Buf);
NTSTATUS NtfsReadAttributeData(PNTFS_VCB Vcb, PNTFS_RUN R, ULONG C, ULONGLONG Off, ULONG Len, PVOID Buf);
NTSTATUS NtfsWriteAttributeData(PNTFS_VCB Vcb, PNTFS_RUN R, ULONG C, ULONGLONG Off, ULONG Len, PVOID Buf);
NTSTATUS NtfsAllocClusters(PNTFS_VCB Vcb, ULONG Count, ULONGLONG *First);
NTSTATUS NtfsFreeClusters(PNTFS_VCB Vcb, ULONGLONG First, ULONG Count);
NTSTATUS NtfsAllocClustersNear(PNTFS_VCB Vcb, ULONG Count, ULONGLONG Hint, ULONGLONG *First);

/* journal is kernel-only; the user-mode index test never runs in a transaction */
static __inline BOOLEAN NtfsJnlInTx(PNTFS_VCB Vcb) { (void)Vcb; return FALSE; }
static __inline void NtfsJnlRange(PNTFS_VCB Vcb, ULONG K, ULONGLONG T, ULONGLONG V,
                                  PVOID O, PVOID N, ULONG L)
{ (void)Vcb;(void)K;(void)T;(void)V;(void)O;(void)N;(void)L; }
#endif
