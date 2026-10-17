/*
 * NTFS3NT - NTFS file system driver for Windows NT 3.1 - 4.0 (one universal
 * ntfs.sys). Kernel header: NT-version compatibility, the on-disk/engine
 * headers (shared with the tools), and the in-memory VCB / FCB / CCB.
 *
 *   NTFS_CROSS  GCC/clang cross build vs the mingw-w64 DDK headers (implies the
 *               NT 3.1 import profile NTFS_NT31 and the NT4-era paths), shimming
 *               the SEH keywords GCC-for-NT lacks: __finally still runs and
 *               __leave still unwinds; only catching CPU exceptions is dropped.
 */

#ifndef _NTFS_H_
#define _NTFS_H_

#ifdef NTFS_CROSS
#ifndef NTFS_NT31
#define NTFS_NT31
#endif
#ifndef NTFS_NT4
#define NTFS_NT4
#endif
#define __try
#define __finally               __ntfs_leave: __attribute__((unused));
#define __except(x)             if (0)
#define __leave                 goto __ntfs_leave
#ifndef AbnormalTermination
#define AbnormalTermination()   0
#endif
#ifndef GetExceptionCode
#define GetExceptionCode()      0
#endif
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif
#endif /* NTFS_CROSS */

#include <ntifs.h>
#include <ntdddisk.h>

#include "ntfsdisk.h"
#include "ntfssup.h"

/* ------------------------------------------------------------------ */
/* NT 3.1 import profile: resolve the executive-resource API at load   */
/* (Lite on 3.51+, legacy on 3.1), and hold notify requests pending.   */
/* (See ntfsres.c / fatres.c in FATNT for the rationale and the proof.) */
/* ------------------------------------------------------------------ */

#ifdef NTFS_NT31

#undef ExAcquireResourceExclusiveLite
#undef ExAcquireResourceSharedLite
#undef ExReleaseResourceForThreadLite
#undef ExInitializeResourceLite
#undef ExDeleteResourceLite

typedef NTSTATUS (NTAPI *NTFS_PINITRES)(PERESOURCE);
typedef BOOLEAN  (NTAPI *NTFS_PACQRES)(PERESOURCE, BOOLEAN);
typedef VOID     (NTAPI *NTFS_PRELRES)(PERESOURCE, ERESOURCE_THREAD);
typedef NTSTATUS (NTAPI *NTFS_PDELRES)(PERESOURCE);

extern NTFS_PINITRES NtfspInitRes;
extern NTFS_PACQRES  NtfspAcqExcl;
extern NTFS_PACQRES  NtfspAcqShared;
extern NTFS_PDELRES  NtfspDelRes;
extern NTFS_PRELRES  NtfspRelForThread;
BOOLEAN NtfsResolveResourceApi(VOID);

#define ExInitializeResourceLite(r)          NtfspInitRes(r)
#define ExAcquireResourceExclusiveLite(r,w)  NtfspAcqExcl((r),(w))
#define ExAcquireResourceSharedLite(r,w)     NtfspAcqShared((r),(w))
#define ExDeleteResourceLite(r)              NtfspDelRes(r)
#define ExReleaseResourceForThreadLite(r,t)  NtfspRelForThread((r),(t))

/* NT 4.0 fast-call Iof/Obf forms are not on NT 3.1; use the stdcall originals. */
#undef IoCallDriver
#undef IoCompleteRequest
#undef ObDereferenceObject
__declspec(dllimport) NTSTATUS NTAPI IoCallDriver(PDEVICE_OBJECT, PIRP);
__declspec(dllimport) VOID     NTAPI IoCompleteRequest(PIRP, CCHAR);
__declspec(dllimport) VOID     NTAPI ObDereferenceObject(PVOID);

#define MmCanFileBeTruncated(S, N)      TRUE

/* NT 3.1 exports neither ExAllocatePoolWithTag nor the Ke critical-region
 * calls (later builds reach the latter inline through the thread). Drop the
 * pool tag and make the critical region a no-op pair - which also makes
 * FsRtlEnterFileSystem/ExitFileSystem (inline wrappers over them) no-ops. */
#define ExAllocatePoolWithTag(Type, Size, Tag)  ExAllocatePool((Type), (Size))
#define KeEnterCriticalRegion()                 ((void)0)
#define KeLeaveCriticalRegion()                 ((void)0)

#endif /* NTFS_NT31 */

#ifdef NTFS_CROSS
typedef IO_STACK_LOCATION EXTENDED_IO_STACK_LOCATION, *PEXTENDED_IO_STACK_LOCATION;
#endif
#define NtfsXSp(IrpSp)   ((PEXTENDED_IO_STACK_LOCATION)(IrpSp))

/* Embedded-ERESOURCE padding: NT 3.1's ERESOURCE is larger than later headers
 * describe, so over-allocate every embedded one (see FATNT's NT 3.1 notes). */
#define NTFS_ERESOURCE_PAD   0x40

/* ------------------------------------------------------------------ */
/* Node type codes                                                     */
/* ------------------------------------------------------------------ */

#define NTFS_NTC_DATA   0x0E01
#define NTFS_NTC_VCB    0x0E02
#define NTFS_NTC_FCB    0x0E03
#define NTFS_NTC_CCB    0x0E04

#define NTFS_TAG_VCB    'VftN'
#define NTFS_TAG_FCB    'FftN'
#define NTFS_TAG_CCB    'CftN'
#define NTFS_TAG_BUF    'BftN'
#define NTFS_TAG_RUN    'RftN'

/* Max run-list entries decoded into a pool scratch buffer. Never put run
 * arrays on the kernel stack: 512 * sizeof(NTFS_RUN) is 16 KB and NT's
 * kernel stack is only 12 KB, so a stack array overflows it (STOP 0x50). */
#define NTFS_MAX_IDX_RUNS  256
#define NTFS_MAX_MFT_RUNS  512

/* ------------------------------------------------------------------ */
/* Global driver data                                                  */
/* ------------------------------------------------------------------ */

typedef struct _NTFS_DATA {
    CSHORT          NodeTypeCode;
    CSHORT          NodeByteSize;
    PDRIVER_OBJECT  DriverObject;
    PDEVICE_OBJECT  FileSystemDeviceObject;
    LIST_ENTRY      VcbList;
    ERESOURCE       Resource;
    UCHAR           ResourcePad[NTFS_ERESOURCE_PAD];
    FAST_IO_DISPATCH FastIoDispatch;
    BOOLEAN         WriteEnabled;
} NTFS_DATA, *PNTFS_DATA;

/* ------------------------------------------------------------------ */
/* Volume control block - one per mounted NTFS volume                  */
/* ------------------------------------------------------------------ */

#define VCB_STATE_MOUNTED       0x0001
#define VCB_STATE_DISMOUNTED    0x0002
#define VCB_STATE_READ_ONLY     0x0004
#define VCB_STATE_SHUTDOWN      0x0008
#define VCB_STATE_LOCKED        0x0010   /* FSCTL_LOCK_VOLUME holder: LockFileObject */

typedef struct _NTFS_VCB {
    CSHORT          NodeTypeCode;
    CSHORT          NodeByteSize;
    LIST_ENTRY      VcbLinks;
    PDEVICE_OBJECT  VolumeDeviceObject;   /* our mounted device        */
    PDEVICE_OBJECT  TargetDeviceObject;   /* the disk below us          */
    PVPB            Vpb;
    ULONG           VcbState;
    ULONG           OpenCount;
    ERESOURCE       Resource;
    UCHAR           ResourcePad[NTFS_ERESOURCE_PAD];

    NTFS_GEOMETRY   Geo;                  /* parsed from $Boot          */

    /* $MFT's own $DATA run list, the bootstrap for reading any record  */
    PNTFS_RUN       MftRuns;
    ULONG           MftRunCount;
    ULONGLONG       MftRecordCount;
    ULONGLONG       FreeClusters;         /* counted from $Bitmap at mount, kept by alloc/free */
    WCHAR           VolumeLabel[32];      /* $Volume:$VOLUME_NAME */
    USHORT          VolumeLabelLength;    /* bytes */

    struct _NTFS_FCB *RootFcb;            /* root directory (record 5)  */
    LIST_ENTRY      FcbList;
    KEVENT          MetaEvent;            /* serializes MFT/index/bitmap updates: a recursive
                                             lock (owner + depth) over a synchronization event */
    PKTHREAD        MetaOwner;
    ULONG           MetaDepth;
    PFILE_OBJECT    LockFileObject;       /* volume handle holding the lock */
    UCHAR           NtfsMajor, NtfsMinor; /* $Volume:$VOLUME_INFORMATION version */
} NTFS_VCB, *PNTFS_VCB;

static __inline VOID NtfsLockMeta(PNTFS_VCB Vcb)
{
    PKTHREAD Me = KeGetCurrentThread();
    KeEnterCriticalRegion();
    if (Vcb->MetaOwner == Me) { Vcb->MetaDepth++; return; }
    KeWaitForSingleObject(&Vcb->MetaEvent, Executive, KernelMode, FALSE, NULL);
    Vcb->MetaOwner = Me;
    Vcb->MetaDepth = 1;
}
static __inline VOID NtfsUnlockMeta(PNTFS_VCB Vcb)
{
    if (--Vcb->MetaDepth == 0) {
        Vcb->MetaOwner = NULL;
        KeSetEvent(&Vcb->MetaEvent, 0, FALSE);
    }
    KeLeaveCriticalRegion();
}

/* a handle opened on the volume itself (DASD): FsContext is the VCB */
#define NTFS_IS_VOLUME_FO(c) ((c)->Vcb != NULL && (c)->IrpSp->FileObject != NULL && \
                              (c)->IrpSp->FileObject->FsContext == (PVOID)(c)->Vcb)
/* Not a KMUTEX: NT 3.1 enforces mutex levels (STOP 0x0D MUTEX_LEVEL_NUMBER_VIOLATION
 * when a thread that owns a system mutex - e.g. inside the Memory or Cache Manager -
 * comes here). Same thread: nesting; other threads: wait on the event. */
#define NTFS_LOCK_META(v)   NtfsLockMeta(v)
#define NTFS_UNLOCK_META(v) NtfsUnlockMeta(v)

/* ------------------------------------------------------------------ */
/* File control block - one per opened MFT record (file or directory)  */
/* ------------------------------------------------------------------ */

#define FCB_STATE_DIRECTORY     0x0001
#define FCB_STATE_ROOT          0x0002
#define FCB_STATE_PAGING_FILE   0x0004
#define FCB_STATE_INFO_LOADED   0x0008   /* LoadFcbInfo already ran (kept FCB) */
#define FCB_STATE_DELETE_PENDING 0x0010  /* FileDispositionInformation marked delete */
#define FCB_STATE_MODIFIED      0x0020   /* data/size changed: refresh dup info at cleanup */
#define NTFS_DEAD_RECORD        ((ULONGLONG)-2)  /* FCB of a deleted file (never matches) */

typedef struct _NTFS_FCB {
    FSRTL_COMMON_FCB_HEADER Header;       /* must be first (Cache/FsRtl) */
    SECTION_OBJECT_POINTERS SectionObjectPointers;

    CSHORT          NodeTypeCode;
    CSHORT          NodeByteSize;
    LIST_ENTRY      FcbLinks;
    PNTFS_VCB       Vcb;

    ULONGLONG       MftRecord;            /* this object's MFT record #  */
    USHORT          Sequence;
    ULONG           FcbState;

    ERESOURCE       Resource;
    UCHAR           ResourcePad[NTFS_ERESOURCE_PAD];
    ERESOURCE       PagingIoResource;
    UCHAR           PagingIoResourcePad[NTFS_ERESOURCE_PAD];

    /* unnamed $DATA run list (files) - cached after first open */
    PNTFS_RUN       DataRuns;
    ULONG           DataRunCount;
    BOOLEAN         DataResident;         /* $DATA held inside the record */
    PUCHAR          ResidentData;         /* resident $DATA bytes, cached at open */
    ULONG           ResidentLength;       /* valid length of ResidentData     */
    BOOLEAN         DataCompressed;       /* $DATA uses NTFS (LZNT1) compression */
    ULONG           CuClusters;           /* clusters per compression unit    */
    ULONG           FileAttributes;       /* FILE_ATTRIBUTE_* (from $STD_INFO) */
    ULONG           ReparseTag;           /* reparse tag if REPARSE_POINT set  */
    ULONGLONG       CreationTime;         /* $STANDARD_INFORMATION timestamps  */
    ULONGLONG       LastAccessTime;
    ULONGLONG       LastWriteTime;
    ULONGLONG       ChangeTime;
    WCHAR           FileName[256];        /* leaf name (for FileNameInformation) */
    USHORT          FileNameLength;       /* bytes in FileName                */

    ULONG           RefCount;
    ULONG           UncleanCount;
} NTFS_FCB, *PNTFS_FCB;

#ifndef FILE_ATTRIBUTE_REPARSE_POINT
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#endif
/* externally meaningful DOS attributes we surface from $STANDARD_INFORMATION */
#define NTFS_DOS_ATTR_MASK  0x00000E27  /* RO|HID|SYS|ARC|SPARSE|REPARSE|COMPRESSED */

/* ------------------------------------------------------------------ */
/* Context control block - per open handle                             */
/* ------------------------------------------------------------------ */

typedef struct _NTFS_CCB {
    CSHORT          NodeTypeCode;
    CSHORT          NodeByteSize;
    ULONG           Flags;
    /* directory enumeration state */
    ULONG           IndexState;           /* 0 = root node, 1 = INDX blocks, 2 = done */
    ULONGLONG       IndexVcn;             /* current INDX block */
    ULONG           IndexOffset;          /* offset of next entry in the node */
    UNICODE_STRING  Pattern;
    WCHAR           PatternBuf[256];      /* backing store for Pattern */
    BOOLEAN         PatternSet;
    BOOLEAN         MatchAll;             /* pattern is empty / "*" */
    BOOLEAN         HasWild;              /* pattern holds wildcards (FsRtl match) */
    PUCHAR          Snap;                 /* in-order index snapshot (paged pool) */
    ULONG           SnapLen;              /* bytes used in Snap */
    ULONG           SnapOff;              /* cursor: byte offset of entry SnapIdx */
    ULONG           SnapIdx;              /* cursor: visible-entry number */

    /* named $DATA stream (ADS) this handle opened, if any. When NamedStream is
     * set the read path serves data from here instead of the FCB's unnamed
     * $DATA; these streams are read directly (not through the Cache Manager). */
    BOOLEAN         NamedStream;
    BOOLEAN         StreamResident;
    BOOLEAN         StreamCompressed;
    ULONG           StreamCuClusters;
    ULONGLONG       StreamSize;
    PUCHAR          StreamResidentData;
    ULONG           StreamResidentLength;
    PNTFS_RUN       StreamRuns;
    ULONG           StreamRunCount;
    WCHAR           StreamName[64];       /* named $DATA name (for writes) */
    ULONG           StreamNameLen;        /* chars */
} NTFS_CCB, *PNTFS_CCB;

/* ------------------------------------------------------------------ */
/* Per-request context                                                 */
/* ------------------------------------------------------------------ */

#define NTFS_CTX_TOP_LEVEL    0x0001
#define NTFS_CTX_NO_COMPLETE  0x0002

typedef struct _NTFS_IRP_CONTEXT {
    PIRP                Irp;
    PIO_STACK_LOCATION  IrpSp;
    PDEVICE_OBJECT      DeviceObject;
    PNTFS_VCB           Vcb;
    ULONG               Flags;
} NTFS_IRP_CONTEXT, *PNTFS_IRP_CONTEXT;

extern NTFS_DATA NtfsData;

/* ------------------------------------------------------------------ */
/* Debug trace to the QEMU debugcon port (0xE9); -DNTFS_DBGPORT to use. */
/* ------------------------------------------------------------------ */
#ifdef NTFS_DBGPORT
static __inline void NtfsDbgCh(unsigned char c)
{ unsigned short port = 0x00E9; __asm__ __volatile__("outb %b0,%w1"::"a"(c),"d"(port)); }
static __inline void NtfsDbgStr(const char *s){ while(*s) NtfsDbgCh((unsigned char)*s++); }
static __inline void NtfsDbgHex(unsigned long v){ int i; char h[9]; const char*d="0123456789abcdef";
  for(i=7;i>=0;i--){h[i]=d[v&0xF];v>>=4;} h[8]=0; NtfsDbgStr(h); }
#define DBGP(s)  NtfsDbgStr(s)
#define DBGX(v)  NtfsDbgHex((unsigned long)(v))
#else
#define DBGP(s)  ((void)0)
#define DBGX(v)  ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Prototypes                                                          */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
NTSTATUS NTAPI NtfsFsdDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);

NTSTATUS NtfsCommonFileSystemControl(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsMountVolume(PNTFS_IRP_CONTEXT Ctx);

NTSTATUS NtfsCommonCreate(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsCommonRead(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsCommonWrite(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsCommonSetInformation(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsCreateInDir(PNTFS_VCB Vcb, ULONGLONG parentRec, USHORT parentSeq,
                         const WCHAR *wname, ULONG wn, const void *data, ULONG dlen,
                         BOOLEAN IsDir, ULONGLONG *newRecOut);
BOOLEAN  NtfsDirIsEmpty(PNTFS_VCB Vcb, ULONGLONG rec);
VOID     NtfsLoadUpcase(PNTFS_VCB Vcb);
NTSTATUS NtfsLoadDataExtents(PNTFS_VCB Vcb, ULONGLONG BaseRec, PMFT_RECORD m, PNTFS_RUN *Runs,
                             PULONG Rc, PATTRIBUTE_RECORD First, PBOOLEAN HasList);
NTSTATUS NtfsWriteDataExtents(PNTFS_VCB Vcb, PNTFS_FCB Fcb, PNTFS_RUN Runs, ULONG Rc,
                              ULONGLONG Alloc, ULONGLONG Size, ULONGLONG Init);
NTSTATUS NtfsSetListDataSize(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONGLONG Size);
BOOLEAN  NtfsRefreshAttrList(PNTFS_VCB Vcb, PMFT_RECORD m, ULONGLONG BaseRec);
NTSTATUS NtfsAllocClustersNear(PNTFS_VCB Vcb, ULONG Count, ULONGLONG Hint, ULONGLONG *FirstLcn);
NTSTATUS NtfsAllocClustersPartial(PNTFS_VCB Vcb, ULONG Want, ULONGLONG Hint, ULONGLONG *FirstLcn, PULONG Got);
NTSTATUS NtfsVolumeReadWrite(PNTFS_IRP_CONTEXT Ctx, BOOLEAN Write);
NTSTATUS NtfsVolumeFileInfo(PNTFS_IRP_CONTEXT Ctx, BOOLEAN Set);
BOOLEAN  NtfsVolumeFsctl(PNTFS_IRP_CONTEXT Ctx, NTSTATUS *Out);
VOID     NtfsUnlockVolume(PNTFS_VCB Vcb);
NTSTATUS NtfsDeviceControl(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsUpdateFileInfo(PNTFS_VCB Vcb, PNTFS_FCB Fcb);
NTSTATUS NtfsRenameFile(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONGLONG tparent,
                        const WCHAR *nn, ULONG nl, BOOLEAN Replace);
ULONGLONG NtfsLookupInDir(PNTFS_VCB Vcb, ULONGLONG DirRecord, const WCHAR *Name, ULONG NameLen);
NTSTATUS NtfsIndexAdd(PNTFS_VCB Vcb, ULONGLONG Dir, PUCHAR Ent, ULONG EntLen,
                      const WCHAR *wname, ULONG wn);
NTSTATUS NtfsIndexDelete(PNTFS_VCB Vcb, ULONGLONG Dir, const WCHAR *wname, ULONG wn);
NTSTATUS NtfsIndexUpdate(PNTFS_VCB Vcb, const FILE_NAME_ATTR *fn);
NTSTATUS NtfsDeleteFile(PNTFS_VCB Vcb, PNTFS_FCB Fcb);
NTSTATUS NtfsTruncateFile(PNTFS_VCB Vcb, PNTFS_FCB Fcb);
NTSTATUS NtfsCommonClose(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsCommonCleanup(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsCommonDirectoryControl(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsCommonQueryInformation(PNTFS_IRP_CONTEXT Ctx);
NTSTATUS NtfsCommonQueryVolumeInformation(PNTFS_IRP_CONTEXT Ctx);

/* I/O + MFT (ntfsio.c / ntfsmft.c) */
NTSTATUS NtfsReadDisk(PNTFS_VCB Vcb, ULONGLONG ByteOffset, ULONG Length, PVOID Buffer);
NTSTATUS NtfsReadMftRecord(PNTFS_VCB Vcb, ULONGLONG RecordNumber, PVOID Buffer);
NTSTATUS NtfsReadCompressed(PNTFS_VCB Vcb, PNTFS_RUN Runs, ULONG RunCount, ULONG CuClusters,
                            ULONGLONG Vbo, ULONG Length, PVOID Buffer);
NTSTATUS NtfsReadAttributeData(PNTFS_VCB Vcb, PNTFS_RUN Runs, ULONG RunCount,
                               ULONGLONG Vbo, ULONG Length, PVOID Buffer);
/* write primitives (ntfsio.c / ntfsmft.c) */
NTSTATUS NtfsWriteDisk(PNTFS_VCB Vcb, ULONGLONG ByteOffset, ULONG Length, PVOID Buffer);
NTSTATUS NtfsWriteAttributeData(PNTFS_VCB Vcb, PNTFS_RUN Runs, ULONG RunCount,
                                ULONGLONG Vbo, ULONG Length, PVOID Buffer);
NTSTATUS NtfsWriteMftRecord(PNTFS_VCB Vcb, ULONGLONG RecordNumber, PVOID Buffer);
/* allocation (ntfsalloc.c) */
NTSTATUS NtfsAllocClusters(PNTFS_VCB Vcb, ULONG Count, ULONGLONG *FirstLcn);
NTSTATUS NtfsFreeClusters(PNTFS_VCB Vcb, ULONGLONG FirstLcn, ULONG Count);
NTSTATUS NtfsAllocMftRecord(PNTFS_VCB Vcb, ULONGLONG *RecordNumber);
NTSTATUS NtfsFreeMftRecord(PNTFS_VCB Vcb, ULONGLONG RecordNumber);
ULONGLONG NtfsCountFreeClusters(PNTFS_VCB Vcb);
PNTFS_FCB NtfsCreateFcb(PNTFS_VCB Vcb, ULONGLONG RecordNumber);
void NtfsDeleteFcb(PNTFS_FCB Fcb);

/* cache manager integration (ntfscache.c) */
VOID NtfsInitCaching(PDRIVER_OBJECT DriverObject);
VOID NtfsEnsureCacheMap(PFILE_OBJECT FileObject, PNTFS_FCB Fcb);

#endif /* _NTFS_H_ */
