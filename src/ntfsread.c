/*
 * NTFS3NT - file read. Reads an opened file's unnamed $DATA.
 *
 * Resident $DATA is copied from the FCB's cached bytes. Non-resident $DATA is
 * served through the Cache Manager for ordinary (cached) reads - required by
 * NT 5 (2000/XP) - while the Cache Manager's own fills, Mm page faults and any
 * explicitly non-cached read go straight to the disk through the run list.
 *
 * Paging / non-cached reads are page-aligned and Mm expects the *whole*
 * requested length back: we read the bytes that exist (up to file size) and
 * zero-fill the remainder of the buffer, returning the full length. Cached
 * reads are clamped to the file size as usual.
 */

#define NTFS_KERNEL
#include "ntfs.h"

static PVOID
NtfsUserBuffer(PIRP Irp)
{
    if (Irp->MdlAddress) {
        return MmGetSystemAddressForMdl(Irp->MdlAddress);
    }
    return Irp->UserBuffer;
}

NTSTATUS
NtfsCommonRead(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNTFS_FCB Fcb = (PNTFS_FCB)FileObject->FsContext;
    PNTFS_CCB Ccb = (PNTFS_CCB)FileObject->FsContext2;
    PNTFS_VCB Vcb = Ctx->Vcb;
    ULONGLONG Offset = (ULONGLONG)IrpSp->Parameters.Read.ByteOffset.QuadPart;
    ULONG Length = IrpSp->Parameters.Read.Length;
    ULONGLONG FileSize;
    ULONG Avail;
    BOOLEAN NonCached;
    PVOID Buffer;
    NTSTATUS Status;

    Ctx->Irp->IoStatus.Information = 0;

    if (Fcb == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (Length == 0) {
        return STATUS_SUCCESS;
    }
    /* a byte range another handle has locked */
    if (!(Ctx->Irp->Flags & IRP_PAGING_IO) && Fcb->NodeTypeCode == NTFS_NTC_FCB &&
        !FsRtlCheckLockForReadAccess(&Fcb->FileLock, Ctx->Irp)) {
        return STATUS_FILE_LOCK_CONFLICT;
    }

    /* ---- named $DATA stream (ADS): served directly from the CCB ---------- */
    if (Ccb && Ccb->NodeTypeCode == NTFS_NTC_CCB && Ccb->NamedStream) {
        ULONGLONG ssz = Ccb->StreamSize;
        ULONG savail = (Offset < ssz) ? (ULONG)(ssz - Offset) : 0;
        if (savail > Length) savail = Length;
        Buffer = NtfsUserBuffer(Ctx->Irp);
        if (Buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        NonCached = (BOOLEAN)((Ctx->Irp->Flags & (IRP_NOCACHE | IRP_PAGING_IO)) != 0);
        if (Ccb->StreamResident) {
            if (savail && Ccb->StreamResidentData)
                RtlCopyMemory(Buffer, Ccb->StreamResidentData + Offset, savail);
        } else if (savail) {
            if (Ccb->StreamCompressed)
                Status = NtfsReadCompressed(Vcb, Ccb->StreamRuns, Ccb->StreamRunCount,
                                            Ccb->StreamCuClusters, Offset, savail, Buffer);
            else
                Status = NtfsReadAttributeData(Vcb, Ccb->StreamRuns, Ccb->StreamRunCount,
                                               Offset, savail, Buffer);
            if (!NT_SUCCESS(Status)) return Status;
        }
        if (Ctx->Irp->Flags & IRP_PAGING_IO) {
            if (savail < Length) RtlZeroMemory((PUCHAR)Buffer + savail, Length - savail);
            Ctx->Irp->IoStatus.Information = Length;
            return STATUS_SUCCESS;
        }
        if (savail == 0) return STATUS_END_OF_FILE;
        if (NonCached && savail < Length) RtlZeroMemory((PUCHAR)Buffer + savail, Length - savail);
        Ctx->Irp->IoStatus.Information = savail;
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
            FileObject->CurrentByteOffset.QuadPart = Offset + savail;
        return STATUS_SUCCESS;
    }

    if (Fcb->FcbState & FCB_STATE_DIRECTORY) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    FileSize = (ULONGLONG)Fcb->Header.FileSize.QuadPart;
    NonCached = (BOOLEAN)((Ctx->Irp->Flags & (IRP_NOCACHE | IRP_PAGING_IO)) != 0);

    DBGP("[RD frec="); DBGX((ULONG)Fcb->MftRecord);
    DBGP(" off="); DBGX((ULONG)Offset); DBGP(" len="); DBGX(Length);
    DBGP(" fsz="); DBGX((ULONG)FileSize); DBGP(" nc="); DBGX(NonCached);
    DBGP(" res="); DBGX(Fcb->DataResident); DBGP("]\n");

    Buffer = NtfsUserBuffer(Ctx->Irp);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* bytes of real data available from Offset */
    Avail = (Offset < FileSize) ? (ULONG)(FileSize - Offset) : 0;
    if (Avail > Length) {
        Avail = Length;
    }

    /* ---- resident $DATA: copied from the FCB, no disk I/O ---------------- */
    if (Fcb->DataResident) {
        if (Avail && Fcb->ResidentData) {
            RtlCopyMemory(Buffer, Fcb->ResidentData + Offset, Avail);
        }
        if (Ctx->Irp->Flags & IRP_PAGING_IO) {
            if (Avail < Length) {
                RtlZeroMemory((PUCHAR)Buffer + Avail, Length - Avail);
            }
            Ctx->Irp->IoStatus.Information = Length;      /* Mm: whole page back */
            return STATUS_SUCCESS;
        }
        if (Avail == 0) {
            return STATUS_END_OF_FILE;
        }
        if (NonCached && Avail < Length) RtlZeroMemory((PUCHAR)Buffer + Avail, Length - Avail);
        Ctx->Irp->IoStatus.Information = Avail;
        /* synchronous file objects: the file system advances the file pointer,
         * otherwise `type`/sequential readers re-read offset 0 forever */
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
            FileObject->CurrentByteOffset.QuadPart = Offset + Avail;
        return STATUS_SUCCESS;
    }

    /* ---- cached read (ordinary ReadFile): go through the Cache Manager --- */
    if (!NonCached) {
        LARGE_INTEGER Off;
        if (Avail == 0) {
            return STATUS_END_OF_FILE;
        }
        Off.QuadPart = (LONGLONG)Offset;

        KeEnterCriticalRegion();
        ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
        NtfsEnsureCacheMap(FileObject, Fcb);
        if (CcCopyRead(FileObject, &Off, Avail, TRUE, Buffer, &Ctx->Irp->IoStatus)) {
            Status = Ctx->Irp->IoStatus.Status;
        } else {
            Status = STATUS_UNSUCCESSFUL;
        }
        ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
        KeLeaveCriticalRegion();
        DBGP("[RD cc frec="); DBGX((ULONG)Fcb->MftRecord); DBGP(" st="); DBGX(Status);
        DBGP(" inf="); DBGX((ULONG)Ctx->Irp->IoStatus.Information);
        DBGP(" b="); { int _i; for(_i=0;_i<4;_i++) DBGX(((PUCHAR)Buffer)[_i]); } DBGP("]\n");

        if (NT_SUCCESS(Status) && (FileObject->Flags & FO_SYNCHRONOUS_IO)) {
            FileObject->CurrentByteOffset.QuadPart =
                Offset + Ctx->Irp->IoStatus.Information;
        }
        return Status;
    }

    /* ---- paging / non-cached non-resident: read from the run list ------- */
    if (!(Ctx->Irp->Flags & IRP_PAGING_IO) && Avail == 0) {
        return STATUS_END_OF_FILE;                 /* user non-cached read at EOF */
    }
    /* Mm wants the full requested length: read the valid part, zero the rest.
     * Paging reads (the paging file included) take PagingIoResource shared so
     * they stay coherent with paging writes, and never touch the main resource. */
    if (Avail > 0) {
        BOOLEAN paging = (BOOLEAN)((Ctx->Irp->Flags & IRP_PAGING_IO) != 0);
        if (paging) { KeEnterCriticalRegion(); ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE); }
        if (Fcb->DataCompressed) {
            Status = NtfsReadCompressed(Vcb, Fcb->DataRuns, Fcb->DataRunCount,
                                        Fcb->CuClusters, Offset, Avail, Buffer);
        } else {
            Status = NtfsReadAttributeData(Vcb, Fcb->DataRuns, Fcb->DataRunCount,
                                           Offset, Avail, Buffer);
        }
        if (paging) { ExReleaseResourceForThreadLite(&Fcb->PagingIoResource, ExGetCurrentResourceThread()); KeLeaveCriticalRegion(); }
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    if (Avail < Length) {
        RtlZeroMemory((PUCHAR)Buffer + Avail, Length - Avail);
    }
    if (Ctx->Irp->Flags & IRP_PAGING_IO) {
        Ctx->Irp->IoStatus.Information = Length;   /* Mm: whole page back */
    } else {
        Ctx->Irp->IoStatus.Information = Avail;    /* user non-cached: clamp to EOF */
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
            FileObject->CurrentByteOffset.QuadPart = Offset + Avail;
    }
    return STATUS_SUCCESS;
}
