/*
 * NTFS3NT - low-level disk I/O: synchronous reads to the volume's target device,
 * and reading an attribute's data through its decoded run list.
 */

#define NTFS_KERNEL
#include "ntfs.h"

/* Completion routine: signal our event directly and keep the IRP so the
 * caller can read the IOSB and free it. Returning MORE_PROCESSING_REQUIRED
 * stops the I/O manager from auto-completing/freeing it. This avoids the
 * thread-APC completion path of IoBuildSynchronousFsdRequest, which deadlocks
 * when a disk read is issued from within an IRP_MJ_READ dispatch. */
static NTSTATUS NTAPI
NtfsReadDiskComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Raw read: ByteOffset and Length MUST be sector-aligned and Buffer suitable
 * for the disk device. Callers go through NtfsReadDisk, which handles unaligned
 * requests with a bounce buffer. */
static NTSTATUS
NtfsReadDiskRaw(PNTFS_VCB Vcb, ULONGLONG ByteOffset, ULONG Length, PVOID Buffer)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Offset;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Offset.QuadPart = (LONGLONG)ByteOffset;

    Irp = IoBuildAsynchronousFsdRequest(IRP_MJ_READ, Vcb->TargetDeviceObject,
                                        Buffer, Length, &Offset, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoSetCompletionRoutine(Irp, NtfsReadDiskComplete, &Event, TRUE, TRUE, TRUE);

    DBGP("[DSK off="); DBGX((ULONG)ByteOffset); DBGP(" len="); DBGX(Length); DBGP(" ");
    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);
    if (Status == STATUS_PENDING) {
        DBGP("pend ");
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }
    /* completion was short-circuited (MORE_PROCESSING_REQUIRED), so the I/O
     * manager never copied IoStatus into our UserIosb - read it off the IRP. */
    Status = Irp->IoStatus.Status;
    /* we stopped auto-completion, so release the MDL and IRP ourselves */
    if (Irp->MdlAddress) {
        MmUnlockPages(Irp->MdlAddress);
        IoFreeMdl(Irp->MdlAddress);
    }
    IoFreeIrp(Irp);
    DBGP("st="); DBGX(Status); DBGP("]\n");
    return Status;
}

/* Read Length bytes at ByteOffset, handling sub-sector alignment. Disk devices
 * require the offset and length to be sector multiples; when they are not, read
 * the enclosing aligned span into a bounce buffer and copy out the slice. */
NTSTATUS
NtfsReadDisk(PNTFS_VCB Vcb, ULONGLONG ByteOffset, ULONG Length, PVOID Buffer)
{
    ULONG Sec = Vcb->Geo.BytesPerSector;
    ULONGLONG Mask;
    ULONGLONG AStart, AEnd;
    ULONG ALen;
    PUCHAR Bounce;
    NTSTATUS Status;

    /* Geometry may not be parsed yet (boot-sector read during mount); the boot
     * sector read is 512-aligned, so a 512-byte default is always correct here. */
    if (Sec == 0) Sec = 512;
    Mask = (ULONGLONG)Sec - 1;
    AStart = ByteOffset & ~Mask;
    AEnd = (ByteOffset + Length + Mask) & ~Mask;
    ALen = (ULONG)(AEnd - AStart);

    if (AStart == ByteOffset && ALen == Length) {
        return NtfsReadDiskRaw(Vcb, ByteOffset, Length, Buffer);   /* aligned */
    }

    Bounce = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, ALen, NTFS_TAG_BUF);
    if (Bounce == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsReadDiskRaw(Vcb, AStart, ALen, Bounce);
    if (NT_SUCCESS(Status)) {
        RtlCopyMemory(Buffer, Bounce + (ULONG)(ByteOffset - AStart), Length);
    }
    ExFreePool(Bounce);
    return Status;
}

/* ===================================================================== *
 *  Write path (mirror of the read primitives above)                       *
 * ===================================================================== */

/* Raw write: ByteOffset and Length MUST be sector-aligned. Callers go through
 * NtfsWriteDisk, which handles unaligned requests with a read-modify-write
 * bounce buffer. */
static NTSTATUS
NtfsWriteDiskRaw(PNTFS_VCB Vcb, ULONGLONG ByteOffset, ULONG Length, PVOID Buffer)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Offset;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Offset.QuadPart = (LONGLONG)ByteOffset;

    Irp = IoBuildAsynchronousFsdRequest(IRP_MJ_WRITE, Vcb->TargetDeviceObject,
                                        Buffer, Length, &Offset, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoSetCompletionRoutine(Irp, NtfsReadDiskComplete, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }
    Status = Irp->IoStatus.Status;
    if (Irp->MdlAddress) {
        MmUnlockPages(Irp->MdlAddress);
        IoFreeMdl(Irp->MdlAddress);
    }
    IoFreeIrp(Irp);
    return Status;
}

/* Write Length bytes at ByteOffset, handling sub-sector alignment with a
 * read-modify-write of the enclosing aligned span. */
NTSTATUS
NtfsWriteDisk(PNTFS_VCB Vcb, ULONGLONG ByteOffset, ULONG Length, PVOID Buffer)
{
    ULONG Sec = Vcb->Geo.BytesPerSector;
    ULONGLONG Mask, AStart, AEnd;
    ULONG ALen;
    PUCHAR Bounce;
    NTSTATUS Status;

    if (Sec == 0) Sec = 512;
    Mask = (ULONGLONG)Sec - 1;
    AStart = ByteOffset & ~Mask;
    AEnd = (ByteOffset + Length + Mask) & ~Mask;
    ALen = (ULONG)(AEnd - AStart);

    if (AStart == ByteOffset && ALen == Length) {
        return NtfsWriteDiskRaw(Vcb, ByteOffset, Length, Buffer);   /* aligned */
    }

    Bounce = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, ALen, NTFS_TAG_BUF);
    if (Bounce == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    /* read-modify-write: read the aligned span, patch the slice, write it back */
    Status = NtfsReadDiskRaw(Vcb, AStart, ALen, Bounce);
    if (NT_SUCCESS(Status)) {
        RtlCopyMemory(Bounce + (ULONG)(ByteOffset - AStart), Buffer, Length);
        Status = NtfsWriteDiskRaw(Vcb, AStart, ALen, Bounce);
    }
    ExFreePool(Bounce);
    return Status;
}

/*
 * Write Length bytes at virtual byte offset Vbo to an attribute described by
 * Runs[0..RunCount). The target range must be backed by allocated clusters
 * (no sparse holes) - callers ensure the file is grown first. Splits writes at
 * run boundaries, mirroring NtfsReadAttributeData.
 */
NTSTATUS
NtfsWriteAttributeData(PNTFS_VCB Vcb, PNTFS_RUN Runs, ULONG RunCount,
                       ULONGLONG Vbo, ULONG Length, PVOID Buffer)
{
    PUCHAR In = (PUCHAR)Buffer;
    ULONG Cs = Vcb->Geo.ClusterSize;

    while (Length > 0) {
        ULONGLONG Vcn = Vbo / Cs;
        ULONG InClu = (ULONG)(Vbo % Cs);
        ULONGLONG Lcn, Left;
        BOOLEAN Sparse;
        ULONG Chunk;
        NTSTATUS Status;

        if (!NtfsMapVcn(Runs, RunCount, Vcn, &Lcn, &Left, &Sparse)) {
            return STATUS_END_OF_FILE;     /* unallocated: cannot write a hole */
        }
        {
            ULONGLONG runBytes = Left * (ULONGLONG)Cs - InClu;
            if (runBytes > 0x100000) runBytes = 0x100000;
            Chunk = (ULONG)runBytes;
        }
        if (Chunk > Length) Chunk = Length;

        Status = NtfsWriteDisk(Vcb, Lcn * (ULONGLONG)Cs + InClu, Chunk, In);
        if (!NT_SUCCESS(Status)) return Status;
        In += Chunk; Vbo += Chunk; Length -= Chunk;
    }
    return STATUS_SUCCESS;
}

/*
 * Read Length bytes at virtual byte offset Vbo from an attribute described by
 * Runs[0..RunCount). Handles sparse holes (read as zero) and splits reads at
 * cluster-run boundaries. Length/Vbo need not be cluster-aligned.
 */
NTSTATUS
NtfsReadAttributeData(PNTFS_VCB Vcb, PNTFS_RUN Runs, ULONG RunCount,
                      ULONGLONG Vbo, ULONG Length, PVOID Buffer)
{
    PUCHAR Out = (PUCHAR)Buffer;
    ULONG Cs = Vcb->Geo.ClusterSize;

    while (Length > 0) {
        ULONGLONG Vcn = Vbo / Cs;
        ULONG InClu = (ULONG)(Vbo % Cs);
        ULONGLONG Lcn, Left;
        BOOLEAN Sparse;
        ULONG Chunk;
        NTSTATUS Status;

        if (!NtfsMapVcn(Runs, RunCount, Vcn, &Lcn, &Left, &Sparse)) {
            if (Sparse) {
                Chunk = Cs - InClu;
                if (Chunk > Length) Chunk = Length;
                RtlZeroMemory(Out, Chunk);
                Out += Chunk; Vbo += Chunk; Length -= Chunk;
                continue;
            }
            return STATUS_END_OF_FILE;
        }

        /* bytes contiguous on disk from here: to the end of this run */
        Chunk = Cs - InClu;
        {
            ULONGLONG runBytes = Left * (ULONGLONG)Cs - InClu;
            if ((ULONGLONG)Length < runBytes && Length < 0x100000) {
                /* read what we can in one go up to the run end, capped */
            }
            if (runBytes > 0x100000) runBytes = 0x100000; /* 1MB cap per read */
            Chunk = (ULONG)runBytes;
        }
        if (Chunk > Length) Chunk = Length;

        Status = NtfsReadDisk(Vcb, Lcn * (ULONGLONG)Cs + InClu, Chunk, Out);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Out += Chunk; Vbo += Chunk; Length -= Chunk;
    }
    return STATUS_SUCCESS;
}

/*
 * Read Length bytes at Vbo from a COMPRESSED (LZNT1) attribute. NTFS stores the
 * data in compression units of CuClusters clusters: a unit is either fully
 * allocated (stored uncompressed), partly allocated + sparse tail (the mapped
 * clusters hold the LZNT1 stream, decompressed to the unit size), or fully
 * sparse (zeros). We decompress each unit the request touches and copy out the
 * overlapping bytes.
 */
NTSTATUS
NtfsReadCompressed(PNTFS_VCB Vcb, PNTFS_RUN Runs, ULONG RunCount, ULONG CuClusters,
                   ULONGLONG Vbo, ULONG Length, PVOID Buffer)
{
    ULONG Cs = Vcb->Geo.ClusterSize;
    ULONG CuSize = CuClusters * Cs;
    PUCHAR Comp, Unit, Out = (PUCHAR)Buffer;
    NTSTATUS Status = STATUS_SUCCESS;

    if (CuSize == 0) return STATUS_INVALID_PARAMETER;

    Comp = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, CuSize, NTFS_TAG_BUF);
    Unit = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, CuSize, NTFS_TAG_BUF);
    if (Comp == NULL || Unit == NULL) {
        if (Comp) ExFreePool(Comp);
        if (Unit) ExFreePool(Unit);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (Length > 0) {
        ULONGLONG UnitBase = (Vbo / CuSize) * (ULONGLONG)CuSize;
        ULONGLONG UnitVcn = UnitBase / Cs;
        ULONG InUnit = (ULONG)(Vbo - UnitBase);
        ULONG Chunk = CuSize - InUnit;
        ULONG Mapped = 0, i;
        if (Chunk > Length) Chunk = Length;

        for (i = 0; i < CuClusters; i++) {
            ULONGLONG lcn, left; BOOLEAN sp;
            if (NtfsMapVcn(Runs, RunCount, UnitVcn + i, &lcn, &left, &sp)) {
                Mapped++;
            }
        }

        if (Mapped == CuClusters) {
            /* stored uncompressed */
            Status = NtfsReadAttributeData(Vcb, Runs, RunCount, UnitBase, CuSize, Unit);
            if (!NT_SUCCESS(Status)) break;
        } else if (Mapped == 0) {
            RtlZeroMemory(Unit, CuSize);
        } else {
            /* compressed: mapped clusters hold the LZNT1 stream */
            RtlZeroMemory(Comp, CuSize);
            Status = NtfsReadAttributeData(Vcb, Runs, RunCount, UnitBase, Mapped * Cs, Comp);
            if (!NT_SUCCESS(Status)) break;
            NtfsLznt1Unit(Comp, Mapped * Cs, Unit, CuSize);
        }

        RtlCopyMemory(Out, Unit + InUnit, Chunk);
        Out += Chunk; Vbo += Chunk; Length -= Chunk;
    }

    ExFreePool(Comp);
    ExFreePool(Unit);
    return Status;
}
