/*
 * NTFS3NT - reading MFT records and managing FCBs. A record is located by
 * mapping record# * RecordSize through $MFT's own run list (Vcb->MftRuns), read
 * from the disk, and its update-sequence fixup applied.
 */

#define NTFS_KERNEL
#include "ntfs.h"

NTSTATUS
NtfsReadMftRecord(PNTFS_VCB Vcb, ULONGLONG RecordNumber, PVOID Buffer)
{
    ULONGLONG Vbo = RecordNumber * (ULONGLONG)Vcb->Geo.MftRecordSize;
    NTSTATUS Status;

    Status = NtfsReadAttributeData(Vcb, Vcb->MftRuns, Vcb->MftRunCount,
                                   Vbo, Vcb->Geo.MftRecordSize, Buffer);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (((PNTFS_RECORD_HEADER)Buffer)->Type != NTFS_FILE_SIGNATURE) {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    if (!NtfsApplyFixup(Buffer, Vcb->Geo.MftRecordSize, Vcb->Geo.BytesPerSector)) {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    return STATUS_SUCCESS;
}

/* Write an MFT record back: apply the update-sequence (fixup) stamp, then write
 * through $MFT's own run list. Buffer is a full record (RecordSize bytes). */
NTSTATUS
NtfsWriteMftRecord(PNTFS_VCB Vcb, ULONGLONG RecordNumber, PVOID Buffer)
{
    ULONGLONG Vbo = RecordNumber * (ULONGLONG)Vcb->Geo.MftRecordSize;
    NTSTATUS Status;
    NtfsSetFixup(Buffer, Vcb->Geo.MftRecordSize, Vcb->Geo.BytesPerSector);
    Status = NtfsWriteAttributeData(Vcb, Vcb->MftRuns, Vcb->MftRunCount,
                                    Vbo, Vcb->Geo.MftRecordSize, Buffer);
    /* $MFTMirr duplicates the first records - four, or a whole cluster's worth
       when a cluster holds more (8 KB clusters and up; Windows and ntfs-3g
       both compare that many): keep the copy identical */
    if (NT_SUCCESS(Status) && Vcb->Geo.MftMirrLcn &&
        RecordNumber < (ULONGLONG)((Vcb->Geo.ClusterSize / Vcb->Geo.MftRecordSize > 4) ?
                                   Vcb->Geo.ClusterSize / Vcb->Geo.MftRecordSize : 4))
        NtfsWriteDisk(Vcb, Vcb->Geo.MftMirrLcn * Vcb->Geo.ClusterSize + Vbo,
                      Vcb->Geo.MftRecordSize, Buffer);
    /* undo the fixup so the caller's buffer holds the real bytes again */
    NtfsApplyFixup(Buffer, Vcb->Geo.MftRecordSize, Vcb->Geo.BytesPerSector);
    return Status;
}

PNTFS_FCB
NtfsCreateFcb(PNTFS_VCB Vcb, ULONGLONG RecordNumber)
{
    PNTFS_FCB Fcb;

    /* reuse an existing FCB for this record if already open */
    {
        PLIST_ENTRY e;
        for (e = Vcb->FcbList.Flink; e != &Vcb->FcbList; e = e->Flink) {
            PNTFS_FCB f = CONTAINING_RECORD(e, NTFS_FCB, FcbLinks);
            if (f->MftRecord == RecordNumber) {
                f->RefCount++;
                return f;
            }
        }
    }

    Fcb = (PNTFS_FCB)ExAllocatePoolWithTag(NonPagedPool, sizeof(NTFS_FCB), NTFS_TAG_FCB);
    if (Fcb == NULL) {
        return NULL;
    }
    RtlZeroMemory(Fcb, sizeof(NTFS_FCB));
    Fcb->NodeTypeCode = NTFS_NTC_FCB;
    Fcb->NodeByteSize = sizeof(NTFS_FCB);
    Fcb->Vcb = Vcb;
    Fcb->MftRecord = RecordNumber;
    Fcb->RefCount = 1;
    ExInitializeResourceLite(&Fcb->Resource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);
    Fcb->Header.Resource = &Fcb->Resource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;
    Fcb->Header.NodeTypeCode = NTFS_NTC_FCB;
    Fcb->Header.NodeByteSize = sizeof(NTFS_FCB);
    InsertTailList(&Vcb->FcbList, &Fcb->FcbLinks);
    return Fcb;
}

void
NtfsDeleteFcb(PNTFS_FCB Fcb)
{
    RemoveEntryList(&Fcb->FcbLinks);
    if (Fcb->DataRuns) {
        ExFreePool(Fcb->DataRuns);
    }
    if (Fcb->ResidentData) {
        ExFreePool(Fcb->ResidentData);
    }
    ExDeleteResourceLite(&Fcb->Resource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);
    ExFreePool(Fcb);
}
