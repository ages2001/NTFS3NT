/*
 * NTFS3NT - Cache Manager integration.
 *
 * NT 5 (Windows 2000 / XP) route ordinary file reads through the Cache Manager
 * and will re-issue a cached read forever if the file system does not take part
 * (the symptom: a user file read loops at offset 0, never advancing). NT 3.51 /
 * 4.0 tolerate a plain non-cached driver; 2000 / XP do not. This file adds the
 * standard participation:
 *
 *   - a FAST_IO_DISPATCH whose FastIoRead is FsRtlCopyRead (serves hits straight
 *     from the cache, no IRP),
 *   - CcInitializeCacheMap on first cached read of a file,
 *   - cached IRP_MJ_READ served by CcCopyRead, which issues the actual disk fill
 *     back to us as paging/no-cache reads (handled by the normal run-list path),
 *   - the four cache-manager callbacks (acquire/release for lazy-write and
 *     read-ahead), read-only so they only take the FCB resource.
 *
 * All of CcInitializeCacheMap / CcCopyRead / FsRtlCopyRead exist from NT 3.1 on,
 * so the one universal binary keeps working on 3.1 - 4.0 as well.
 */

#define NTFS_KERNEL
#include "ntfs.h"

static CACHE_MANAGER_CALLBACKS NtfsCacheCallbacks;

/* ---- cache manager callbacks (read-only: just the FCB resource) ---------- */

static BOOLEAN NTAPI
NtfsAcqForLazyWrite(PVOID Context, BOOLEAN Wait)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Context;
    KeEnterCriticalRegion();
    if (!ExAcquireResourceExclusiveLite(&Fcb->Resource, Wait)) {
        KeLeaveCriticalRegion();
        return FALSE;
    }
    return TRUE;
}

static VOID NTAPI
NtfsRelFromLazyWrite(PVOID Context)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Context;
    ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
    KeLeaveCriticalRegion();
}

static BOOLEAN NTAPI
NtfsAcqForReadAhead(PVOID Context, BOOLEAN Wait)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Context;
    KeEnterCriticalRegion();
    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        KeLeaveCriticalRegion();
        return FALSE;
    }
    return TRUE;
}

static VOID NTAPI
NtfsRelFromReadAhead(PVOID Context)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Context;
    ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
    KeLeaveCriticalRegion();
}

/* ---- fast I/O ------------------------------------------------------------ */

static BOOLEAN NTAPI
NtfsFastIoCheckIfPossible(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset,
                          ULONG Length, BOOLEAN Wait, ULONG LockKey,
                          BOOLEAN CheckForReadOperation, PIO_STATUS_BLOCK IoStatus,
                          PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);  UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);      UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);     UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    /* No byte-range locks are supported, so a read is always possible; writes
     * always take the IRP path (NtfsCommonWrite). */
    return CheckForReadOperation ? TRUE : FALSE;
}

/*
 * Fast-I/O write: always decline so the write arrives as an IRP and goes
 * through NtfsCommonWrite (which persists sizes/allocation to the MFT).
 * This entry MUST be non-NULL: once a file has a PrivateCacheMap, the NT I/O
 * manager calls FastIoWrite unconditionally (only a checked-build ASSERT
 * guards it), so a NULL here is a jump to address 0 (STOP 0x1E).
 */
static BOOLEAN NTAPI
NtfsFastIoWrite(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length,
                BOOLEAN Wait, ULONG LockKey, PVOID Buffer, PIO_STATUS_BLOCK IoStatus,
                PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject); UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);     UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(IoStatus);   UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

/* Mm maps the file (for cached or mapped access): serialize against the FCB. */
static VOID NTAPI
NtfsAcqFileForCreateSection(PFILE_OBJECT FileObject)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)FileObject->FsContext;
    if (Fcb) {
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
    }
}

static VOID NTAPI
NtfsRelFileForCreateSection(PFILE_OBJECT FileObject)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)FileObject->FsContext;
    if (Fcb) {
        ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
        KeLeaveCriticalRegion();
    }
}

/* ---- setup + helpers ----------------------------------------------------- */

VOID
NtfsInitCaching(PDRIVER_OBJECT DriverObject)
{
    PFAST_IO_DISPATCH f = &NtfsData.FastIoDispatch;

    RtlZeroMemory(f, sizeof(*f));
    f->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    f->FastIoCheckIfPossible = NtfsFastIoCheckIfPossible;
    f->FastIoRead  = FsRtlCopyRead;     /* cache hits, no IRP */
    f->FastIoWrite = NtfsFastIoWrite;   /* declines -> IRP path (never NULL) */
    f->AcquireFileForNtCreateSection = NtfsAcqFileForCreateSection;
    f->ReleaseFileForNtCreateSection = NtfsRelFileForCreateSection;
    DriverObject->FastIoDispatch = f;

    NtfsCacheCallbacks.AcquireForLazyWrite  = NtfsAcqForLazyWrite;
    NtfsCacheCallbacks.ReleaseFromLazyWrite = NtfsRelFromLazyWrite;
    NtfsCacheCallbacks.AcquireForReadAhead  = NtfsAcqForReadAhead;
    NtfsCacheCallbacks.ReleaseFromReadAhead = NtfsRelFromReadAhead;
}

/* Initialize the shared cache map for this file object once. */
VOID
NtfsEnsureCacheMap(PFILE_OBJECT FileObject, PNTFS_FCB Fcb)
{
    CC_FILE_SIZES Sizes;

    if (FileObject->PrivateCacheMap != NULL) {
        return;                         /* already caching */
    }
    Sizes.AllocationSize  = Fcb->Header.AllocationSize;
    Sizes.FileSize        = Fcb->Header.FileSize;
    Sizes.ValidDataLength = Fcb->Header.ValidDataLength;

    CcInitializeCacheMap(FileObject, &Sizes, FALSE, &NtfsCacheCallbacks, Fcb);
}
