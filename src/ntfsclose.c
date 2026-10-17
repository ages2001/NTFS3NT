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

    /* last handle closing on a delete-marked file or empty directory:
     * perform the delete now; the FCB then never matches a record again */
    if (Fcb && Fcb->NodeTypeCode == NTFS_NTC_FCB && Fcb->UncleanCount == 0 &&
        (Fcb->FcbState & FCB_STATE_DELETE_PENDING) &&
        NtfsData.WriteEnabled && Ctx->Vcb) {
        if (FileObject->PrivateCacheMap != NULL) {
            CcUninitializeCacheMap(FileObject, NULL, NULL);
        }
        if (NT_SUCCESS(NtfsDeleteFile(Ctx->Vcb, Fcb)))
            Fcb->MftRecord = NTFS_DEAD_RECORD;
        Fcb->FcbState &= ~(FCB_STATE_DELETE_PENDING | FCB_STATE_MODIFIED);
    }

    /* data/size changed through this handle: refresh $STANDARD_INFORMATION
     * times and the duplicated info in the directory entry (what DIR shows) */
    if (Fcb && Fcb->NodeTypeCode == NTFS_NTC_FCB && (Fcb->FcbState & FCB_STATE_MODIFIED) &&
        Fcb->MftRecord != NTFS_DEAD_RECORD && NtfsData.WriteEnabled && Ctx->Vcb) {
        Fcb->FcbState &= ~FCB_STATE_MODIFIED;
        NtfsUpdateFileInfo(Ctx->Vcb, Fcb);
    }

    /* Tear down this file object's cache map, if one was created for it. The
     * driver is read-only, so there is nothing to flush; this just releases the
     * shared cache map / section when the handle closes. */
    if (FileObject->PrivateCacheMap != NULL) {
        CcUninitializeCacheMap(FileObject, NULL, NULL);
    }
    return STATUS_SUCCESS;
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
