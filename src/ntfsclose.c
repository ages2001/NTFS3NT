/*
 * NTFS3NT - cleanup and close. Cleanup runs when the last handle to a file object
 * is released; close when the last reference goes away. The root FCB lives for
 * the life of the mount.
 */

#define NTFS_KERNEL
#include "ntfs.h"

NTSTATUS
NtfsCommonCleanup(PNTFS_IRP_CONTEXT Ctx)
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PNTFS_FCB Fcb = (PNTFS_FCB)FileObject->FsContext;

    if (Fcb && Fcb->UncleanCount > 0) {
        Fcb->UncleanCount--;
    }

    /* change-notify requests held for this handle end here */
    NtfsNotifyCleanup(Ctx->Vcb, FileObject);

    /* byte-range locks this handle's process holds through it */
    if (Fcb && Fcb->NodeTypeCode == NTFS_NTC_FCB && !(Fcb->FcbState & FCB_STATE_DIRECTORY))
        FsRtlFastUnlockAll(&Fcb->FileLock, FileObject, IoGetCurrentProcess(), NULL);

    /* last handle closing on a delete-marked file or empty directory:
     * perform the delete now; the FCB then never matches a record again */
    if (Fcb && Fcb->NodeTypeCode == NTFS_NTC_FCB && Fcb->UncleanCount == 0 &&
        (Fcb->FcbState & FCB_STATE_DELETE_PENDING) &&
        NtfsData.WriteEnabled && Ctx->Vcb) {
        /* The cache may hold dirty pages of the file (written-through data that
         * also went into the cache, a mapped view): drop them before the
         * clusters are given back, or the lazy writer would later write them
         * over whatever file gets those clusters next. */
        {
            LARGE_INTEGER Zero;
            Zero.QuadPart = 0;
            if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {
                (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
            }
            if (FileObject->PrivateCacheMap != NULL) {
                CcUninitializeCacheMap(FileObject, &Zero, NULL);
            }
        }
        if (NT_SUCCESS(NtfsDeleteFile(Ctx->Vcb, Fcb))) {
            Fcb->MftRecord = NTFS_DEAD_RECORD;
            /* nothing is allocated any more: a late paging write is dropped */
            Fcb->Header.AllocationSize.QuadPart = 0;
            Fcb->Header.FileSize.QuadPart = 0;
            Fcb->Header.ValidDataLength.QuadPart = 0;
        }
        Fcb->FcbState &= ~(FCB_STATE_DELETE_PENDING | FCB_STATE_MODIFIED);
    }

    /* data/size changed through this handle: refresh $STANDARD_INFORMATION
     * times and the duplicated info in the directory entry (what DIR shows) */
    if (Fcb && Fcb->NodeTypeCode == NTFS_NTC_FCB && (Fcb->FcbState & FCB_STATE_MODIFIED) &&
        Fcb->MftRecord != NTFS_DEAD_RECORD && NtfsData.WriteEnabled && Ctx->Vcb) {
        Fcb->FcbState &= ~FCB_STATE_MODIFIED;
        NtfsUpdateFileInfo(Ctx->Vcb, Fcb);
    }

    /* Tear down this file object's cache map, if one was created for it.
     * Writes go through to the disk, so this releases the shared cache map /
     * section; pages the cache holds dirty are written later to the file's own
     * clusters. */
    if (FileObject->PrivateCacheMap != NULL) {
        CcUninitializeCacheMap(FileObject, NULL, NULL);
    }
    return STATUS_SUCCESS;
}

/* IRP_MJ_LOCK_CONTROL: byte-range locks, kept by the FsRtl package (which
 * completes the IRP itself) */
NTSTATUS
NtfsCommonLockControl(PNTFS_IRP_CONTEXT Ctx)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;

    if (Fcb == NULL || Fcb->NodeTypeCode != NTFS_NTC_FCB || (Fcb->FcbState & FCB_STATE_DIRECTORY))
        return STATUS_INVALID_PARAMETER;
    Ctx->Flags |= NTFS_CTX_NO_COMPLETE;
    return FsRtlProcessFileLock(&Fcb->FileLock, Ctx->Irp, NULL);
}

NTSTATUS
NtfsCommonClose(PNTFS_IRP_CONTEXT Ctx)
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PNTFS_FCB Fcb = (PNTFS_FCB)FileObject->FsContext;
    PNTFS_VCB Vcb = Ctx->Vcb;

    /* release the per-handle CCB (directory-enumeration state or an ADS stream
     * descriptor), freeing any stream buffers it owns */
    {
        PNTFS_CCB Ccb = (PNTFS_CCB)FileObject->FsContext2;
        if (Ccb && Ccb->NodeTypeCode == NTFS_NTC_CCB) {
            if (Ccb->StreamResidentData) ExFreePool(Ccb->StreamResidentData);
            if (Ccb->StreamRuns) ExFreePool(Ccb->StreamRuns);
            if (Ccb->Snap) ExFreePool(Ccb->Snap);
            ExFreePool(Ccb);
            FileObject->FsContext2 = NULL;
        }
    }

    if (Fcb == NULL) {
        return STATUS_SUCCESS;          /* FS device handle */
    }
    if (Vcb) {
        if (Vcb->OpenCount > 0) Vcb->OpenCount--;
    }
    if (Fcb->RefCount > 0) {
        Fcb->RefCount--;
    }
    /*
     * Do NOT free the FCB here even at RefCount 0. The Cache Manager tears a
     * shared cache map down lazily and keeps referencing the FCB's embedded
     * SECTION_OBJECT_POINTERS after the handle closes; freeing the FCB would let
     * the next allocation reuse that memory and Cc would then read one file's
     * cached pages for another (observed: a non-resident file served a prior
     * resident file's bytes). FCBs are kept for the life of the mount and reused
     * on reopen (NtfsCreateFcb matches by record); they are released at dismount.
     */
    FileObject->FsContext = NULL;
    return STATUS_SUCCESS;
}
