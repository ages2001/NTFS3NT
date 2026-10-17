/*
 * NTFS3NT - IRP dispatch. Routes major functions to the common handlers and
 * completes the IRP unless a handler took ownership (NTFS_CTX_NO_COMPLETE).
 */

#define NTFS_KERNEL
#include "ntfs.h"

static NTSTATUS
NtfsGuardedDispatch(PNTFS_IRP_CONTEXT Ctx)
{
    /* disk IOCTLs go to the disk on any handle */
    if (Ctx->IrpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL)
        return NtfsDeviceControl(Ctx);

    /* volume (DASD) handle */
    if (NTFS_IS_VOLUME_FO(Ctx)) {
        switch (Ctx->IrpSp->MajorFunction) {
        case IRP_MJ_READ:              return NtfsVolumeReadWrite(Ctx, FALSE);
        case IRP_MJ_WRITE:             return NtfsVolumeReadWrite(Ctx, TRUE);
        case IRP_MJ_QUERY_INFORMATION: return NtfsVolumeFileInfo(Ctx, FALSE);
        case IRP_MJ_SET_INFORMATION:   return NtfsVolumeFileInfo(Ctx, TRUE);
        case IRP_MJ_DIRECTORY_CONTROL: return STATUS_INVALID_PARAMETER;
        case IRP_MJ_CLEANUP:
            if ((Ctx->Vcb->VcbState & VCB_STATE_LOCKED) &&
                Ctx->Vcb->LockFileObject == Ctx->IrpSp->FileObject)
                NtfsUnlockVolume(Ctx->Vcb);
            return STATUS_SUCCESS;
        case IRP_MJ_CLOSE:
            if (Ctx->Vcb->OpenCount > 0) Ctx->Vcb->OpenCount--;
            Ctx->IrpSp->FileObject->FsContext = NULL;
            return STATUS_SUCCESS;
        }
    }

    switch (Ctx->IrpSp->MajorFunction) {

    case IRP_MJ_CREATE:
        return NtfsCommonCreate(Ctx);
    case IRP_MJ_CLOSE:
        return NtfsCommonClose(Ctx);
    case IRP_MJ_CLEANUP:
        return NtfsCommonCleanup(Ctx);
    case IRP_MJ_READ:
        return NtfsCommonRead(Ctx);
    case IRP_MJ_WRITE:
        return NtfsCommonWrite(Ctx);
    case IRP_MJ_DIRECTORY_CONTROL:
        return NtfsCommonDirectoryControl(Ctx);
    case IRP_MJ_QUERY_INFORMATION:
        return NtfsCommonQueryInformation(Ctx);
    case IRP_MJ_SET_INFORMATION:
        return NtfsCommonSetInformation(Ctx);
    case IRP_MJ_QUERY_VOLUME_INFORMATION:
        return NtfsCommonQueryVolumeInformation(Ctx);
    case IRP_MJ_FILE_SYSTEM_CONTROL:
        return NtfsCommonFileSystemControl(Ctx);
    case IRP_MJ_FLUSH_BUFFERS:          /* file data and metadata are written through */
    case IRP_MJ_SHUTDOWN:
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS NTAPI
NtfsFsdDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    NTFS_IRP_CONTEXT Ctx;
    NTSTATUS Status;

    FsRtlEnterFileSystem();

    RtlZeroMemory(&Ctx, sizeof(Ctx));
    Ctx.Irp = Irp;
    Ctx.IrpSp = IoGetCurrentIrpStackLocation(Irp);
    Ctx.DeviceObject = DeviceObject;
    if (DeviceObject != NtfsData.FileSystemDeviceObject) {
        Ctx.Vcb = (PNTFS_VCB)DeviceObject->DeviceExtension;
    }

    DBGP("[FSD mj="); DBGX(Ctx.IrpSp->MajorFunction);
    if (Ctx.IrpSp->MajorFunction == IRP_MJ_QUERY_VOLUME_INFORMATION) { DBGP(" vcls="); DBGX(Ctx.IrpSp->Parameters.QueryVolume.FsInformationClass); DBGP(" len="); DBGX(Ctx.IrpSp->Parameters.QueryVolume.Length); }
    if (Ctx.IrpSp->MajorFunction == IRP_MJ_CREATE && Ctx.IrpSp->FileObject) { USHORT k; DBGP(" opt="); DBGX(Ctx.IrpSp->Parameters.Create.Options); DBGP(" \""); for (k = 0; k < Ctx.IrpSp->FileObject->FileName.Length / 2 && k < 80; k++) { char ch[2]; ch[0] = (char)Ctx.IrpSp->FileObject->FileName.Buffer[k]; ch[1] = 0; DBGP(ch); } DBGP("\""); }
    if (Ctx.IrpSp->MajorFunction == IRP_MJ_DIRECTORY_CONTROL && Ctx.IrpSp->Parameters.QueryDirectory.FileName) { USHORT k; PUNICODE_STRING u = (PUNICODE_STRING)Ctx.IrpSp->Parameters.QueryDirectory.FileName; DBGP(" pat=\""); for (k = 0; k < u->Length / 2 && k < 80; k++) { char ch[2]; ch[0] = (char)u->Buffer[k]; ch[1] = 0; DBGP(ch); } DBGP("\""); }
    if (Ctx.IrpSp->MajorFunction == IRP_MJ_QUERY_INFORMATION) { DBGP(" icls="); DBGX(Ctx.IrpSp->Parameters.QueryFile.FileInformationClass); DBGP(" len="); DBGX(Ctx.IrpSp->Parameters.QueryFile.Length); }
    if (Ctx.IrpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL || Ctx.IrpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL) { DBGP(" ioctl="); DBGX(Ctx.IrpSp->Parameters.DeviceIoControl.IoControlCode); DBGP(" mn="); DBGX(Ctx.IrpSp->MinorFunction); }
    DBGP("]\n");

    Status = NtfsGuardedDispatch(&Ctx);

    if (!NT_SUCCESS(Status)) {
        DBGP("[ERR mj="); DBGX(Ctx.IrpSp->MajorFunction);
        if (Ctx.IrpSp->MajorFunction == IRP_MJ_QUERY_INFORMATION)
            { DBGP(" cls="); DBGX(Ctx.IrpSp->Parameters.QueryFile.FileInformationClass); }
        DBGP(" st="); DBGX(Status); DBGP("]\n");
    }

    if (!(Ctx.Flags & NTFS_CTX_NO_COMPLETE)) {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_DISK_INCREMENT);
    }

    FsRtlExitFileSystem();
    return Status;
}
