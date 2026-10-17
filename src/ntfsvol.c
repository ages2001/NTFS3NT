/*
 * NTFS3NT - volume (DASD) handles and device-control pass-through.
 *
 * Opening "\\.\X:" (an empty file name with no related file object) gives a
 * handle on the volume itself, as every NT file system provides. Disk tools
 * (CHKDSK, FORMAT, ntfsfmt/ntfschk, backup programs) use it to read sectors,
 * query the disk geometry/partition through IOCTLs, and lock/dismount the
 * volume. FileObject->FsContext is the VCB for such a handle.
 */

#define NTFS_KERNEL
#include "ntfs.h"

#ifndef FSCTL_LOCK_VOLUME
#define FSCTL_LOCK_VOLUME         CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_UNLOCK_VOLUME       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 7, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_DISMOUNT_VOLUME     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_IS_VOLUME_MOUNTED
#define FSCTL_IS_VOLUME_MOUNTED   CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 10, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_ALLOW_EXTENDED_DASD_IO
#define FSCTL_ALLOW_EXTENDED_DASD_IO CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

/* bytes addressable through the volume handle (boot count + backup boot sector) */
static ULONGLONG
VolumeBytes(PNTFS_VCB Vcb)
{
    return (Vcb->Geo.TotalSectors + 1) * (ULONGLONG)Vcb->Geo.BytesPerSector;
}

static PVOID
IrpBuffer(PIRP Irp)
{
    if (Irp->MdlAddress) return MmGetSystemAddressForMdl(Irp->MdlAddress);
    return Irp->UserBuffer;
}

/* Raw sector read/write through the volume handle (sector-aligned only). */
NTSTATUS
NtfsVolumeReadWrite(PNTFS_IRP_CONTEXT Ctx, BOOLEAN Write)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNTFS_VCB Vcb = Ctx->Vcb;
    ULONGLONG Offset = (ULONGLONG)(Write ? IrpSp->Parameters.Write.ByteOffset.QuadPart
                                         : IrpSp->Parameters.Read.ByteOffset.QuadPart);
    ULONG Length = Write ? IrpSp->Parameters.Write.Length : IrpSp->Parameters.Read.Length;
    ULONG Sec = Vcb->Geo.BytesPerSector, done = 0;
    ULONGLONG vb = VolumeBytes(Vcb);
    PUCHAR Buffer;
    NTSTATUS Status = STATUS_SUCCESS;

    Ctx->Irp->IoStatus.Information = 0;
    if (Length == 0) return STATUS_SUCCESS;
    if ((Offset % Sec) || (Length % Sec)) return STATUS_INVALID_PARAMETER;
    if (Write && !((Vcb->VcbState & VCB_STATE_LOCKED) && Vcb->LockFileObject == FileObject))
        return STATUS_ACCESS_DENIED;              /* raw writes only while locked */
    if (Offset >= vb) return Write ? STATUS_DISK_FULL : STATUS_END_OF_FILE;
    if (Offset + Length > vb) Length = (ULONG)(vb - Offset);
    Buffer = (PUCHAR)IrpBuffer(Ctx->Irp);
    if (Buffer == NULL) return STATUS_INVALID_USER_BUFFER;

    while (done < Length) {
        ULONG n = Length - done;
        if (n > 0x10000) n = 0x10000;
        Status = Write ? NtfsWriteDisk(Vcb, Offset + done, n, Buffer + done)
                       : NtfsReadDisk(Vcb, Offset + done, n, Buffer + done);
        if (!NT_SUCCESS(Status)) break;
        done += n;
    }
    if (done) {
        Ctx->Irp->IoStatus.Information = done;
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
            FileObject->CurrentByteOffset.QuadPart = (LONGLONG)(Offset + done);
        Status = STATUS_SUCCESS;
    }
    return Status;
}

/* Query/set information on a volume handle: position and size only. */
NTSTATUS
NtfsVolumeFileInfo(PNTFS_IRP_CONTEXT Ctx, BOOLEAN Set)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PVOID Buffer = Ctx->Irp->AssociatedIrp.SystemBuffer;
    if (Set) {
        if (IrpSp->Parameters.SetFile.FileInformationClass != FilePositionInformation)
            return STATUS_INVALID_PARAMETER;
        FileObject->CurrentByteOffset = ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset;
        return STATUS_SUCCESS;
    }
    switch (IrpSp->Parameters.QueryFile.FileInformationClass) {
    case FilePositionInformation:
        if (IrpSp->Parameters.QueryFile.Length < sizeof(FILE_POSITION_INFORMATION)) return STATUS_BUFFER_TOO_SMALL;
        ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset = FileObject->CurrentByteOffset;
        Ctx->Irp->IoStatus.Information = sizeof(FILE_POSITION_INFORMATION);
        return STATUS_SUCCESS;
    case FileStandardInformation: {
        PFILE_STANDARD_INFORMATION si = (PFILE_STANDARD_INFORMATION)Buffer;
        if (IrpSp->Parameters.QueryFile.Length < sizeof(*si)) return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(si, sizeof(*si));
        si->EndOfFile.QuadPart = (LONGLONG)VolumeBytes(Ctx->Vcb);
        si->AllocationSize = si->EndOfFile;
        si->NumberOfLinks = 1;
        Ctx->Irp->IoStatus.Information = sizeof(*si);
        return STATUS_SUCCESS;
    }
    }
    return STATUS_INVALID_PARAMETER;
}

/* Volume FSCTLs: lock / unlock / dismount / is-mounted. FALSE = not ours. */
BOOLEAN
NtfsVolumeFsctl(PNTFS_IRP_CONTEXT Ctx, NTSTATUS *Out)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    ULONG code = Ctx->IrpSp->Parameters.FileSystemControl.FsControlCode;
    KIRQL irql;

    if (Vcb == NULL) return FALSE;
    switch (code) {
    case FSCTL_LOCK_VOLUME:
        if (Vcb->VcbState & VCB_STATE_LOCKED) { *Out = STATUS_ACCESS_DENIED; return TRUE; }
        /* only the caller's handle may be open (no files, no current dirs) */
        if (Vcb->OpenCount > 1) { *Out = STATUS_ACCESS_DENIED; return TRUE; }
        Vcb->VcbState |= VCB_STATE_LOCKED;
        Vcb->LockFileObject = FileObject;
        IoAcquireVpbSpinLock(&irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(irql);
        *Out = STATUS_SUCCESS; return TRUE;
    case FSCTL_UNLOCK_VOLUME:
        if (!(Vcb->VcbState & VCB_STATE_LOCKED) || Vcb->LockFileObject != FileObject) {
            *Out = STATUS_NOT_LOCKED; return TRUE;
        }
        NtfsUnlockVolume(Vcb);
        *Out = STATUS_SUCCESS; return TRUE;
    case FSCTL_DISMOUNT_VOLUME:
        /* allowed when locked by the caller (or nothing else is open) */
        if (!((Vcb->VcbState & VCB_STATE_LOCKED) && Vcb->LockFileObject == FileObject) &&
            Vcb->OpenCount > 1) { *Out = STATUS_ACCESS_DENIED; return TRUE; }
        Vcb->VcbState |= VCB_STATE_DISMOUNTED;
        /* the next open of the device mounts afresh (e.g. after a format) */
        IoAcquireVpbSpinLock(&irql);
        Vcb->Vpb->Flags &= ~VPB_MOUNTED;
        IoReleaseVpbSpinLock(irql);
        *Out = STATUS_SUCCESS; return TRUE;
    case FSCTL_IS_VOLUME_MOUNTED:
        *Out = (Vcb->VcbState & VCB_STATE_DISMOUNTED) ? STATUS_VOLUME_DISMOUNTED : STATUS_SUCCESS;
        return TRUE;
    case FSCTL_ALLOW_EXTENDED_DASD_IO:
        *Out = STATUS_SUCCESS; return TRUE;
    }
    return FALSE;
}

VOID
NtfsUnlockVolume(PNTFS_VCB Vcb)
{
    KIRQL irql;
    Vcb->VcbState &= ~VCB_STATE_LOCKED;
    Vcb->LockFileObject = NULL;
    IoAcquireVpbSpinLock(&irql);
    Vcb->Vpb->Flags &= ~VPB_LOCKED;
    IoReleaseVpbSpinLock(irql);
}

/* IRP_MJ_DEVICE_CONTROL: hand disk IOCTLs (geometry, partition info, ...) to
 * the disk below, on any handle of the volume. */
NTSTATUS
NtfsDeviceControl(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION next;
    if (Ctx->Vcb == NULL) return STATUS_INVALID_DEVICE_REQUEST;
    next = IoGetNextIrpStackLocation(Ctx->Irp);
    *next = *Ctx->IrpSp;
    next->Control = 0;
    IoSetCompletionRoutine(Ctx->Irp, NULL, NULL, FALSE, FALSE, FALSE);
    Ctx->Flags |= NTFS_CTX_NO_COMPLETE;
    return IoCallDriver(Ctx->Vcb->TargetDeviceObject, Ctx->Irp);
}
