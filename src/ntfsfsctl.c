/*
 * NTFS3NT - volume mount. Reads $Boot, parses the geometry, bootstraps $MFT's
 * run list from record 0, builds the VCB and opens the root directory.
 */

#define NTFS_KERNEL
#include "ntfs.h"


NTSTATUS
NtfsMountVolume(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PDEVICE_OBJECT TargetDevice = IrpSp->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT VolumeDevice = NULL;
    PNTFS_VCB Vcb = NULL;
    PUCHAR Sector = NULL;
    PUCHAR Rec0 = NULL;
    PNTFS_RUN TmpRuns = NULL;          /* pool, not stack: 512 runs = 16 KB */
    ULONG RunCount = 0;
    PATTRIBUTE_RECORD Attr;
    NTSTATUS Status;

    Status = IoCreateDevice(NtfsData.DriverObject, sizeof(NTFS_VCB), NULL,
                            FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE, &VolumeDevice);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    VolumeDevice->StackSize = (CCHAR)(TargetDevice->StackSize + 1);
    /* Direct I/O: read/write IRPs arrive with a system-mapped MDL that is valid
     * in any thread/process context. This is what NT 4.0+ expect of a file
     * system and avoids the fragile raw-user-buffer ("neither I/O") path that
     * leaves data reaching the caller unreliably. */
    VolumeDevice->Flags |= DO_DIRECT_IO;
#ifdef DO_DEVICE_INITIALIZING
    VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

    Vcb = (PNTFS_VCB)VolumeDevice->DeviceExtension;
    RtlZeroMemory(Vcb, sizeof(NTFS_VCB));
    Vcb->NodeTypeCode = NTFS_NTC_VCB;
    Vcb->NodeByteSize = sizeof(NTFS_VCB);
    Vcb->VolumeDeviceObject = VolumeDevice;
    Vcb->TargetDeviceObject = TargetDevice;
    Vcb->Vpb = Vpb;
    ExInitializeResourceLite(&Vcb->Resource);
    InitializeListHead(&Vcb->FcbList);
    KeInitializeEvent(&Vcb->MetaEvent, SynchronizationEvent, TRUE);
    Vcb->MetaOwner = NULL;
    Vcb->MetaDepth = 0;

    __try {
        /* run scratch buffer (pool - far too big for the kernel stack) */
        TmpRuns = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
                      NTFS_MAX_MFT_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (TmpRuns == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; __leave; }

        /* 1. boot sector */
        Sector = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 512, NTFS_TAG_BUF);
        if (Sector == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; __leave; }

        Status = NtfsReadDisk(Vcb, 0, 512, Sector);
        if (!NT_SUCCESS(Status)) { __leave; }

        if (!NtfsParseBoot(Sector, 512, &Vcb->Geo)) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        /* 2. $MFT record 0 (direct read at MftLcn) -> $DATA run list */
        Rec0 = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
        if (Rec0 == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; __leave; }

        Status = NtfsReadDisk(Vcb, Vcb->Geo.MftLcn * (ULONGLONG)Vcb->Geo.ClusterSize,
                              Vcb->Geo.MftRecordSize, Rec0);
        if (!NT_SUCCESS(Status)) { __leave; }

        if (((PNTFS_RECORD_HEADER)Rec0)->Type != NTFS_FILE_SIGNATURE ||
            !NtfsApplyFixup(Rec0, Vcb->Geo.MftRecordSize, Vcb->Geo.BytesPerSector)) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Attr = NtfsFindAttribute((PMFT_RECORD)Rec0, ATTR_DATA, NULL, 0);
        if (Attr == NULL || !Attr->NonResident ||
            !NtfsDecodeRuns(Attr, TmpRuns, NTFS_MAX_MFT_RUNS, &RunCount) || RunCount == 0) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Vcb->MftRuns = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
                          RunCount * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (Vcb->MftRuns == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; __leave; }
        RtlCopyMemory(Vcb->MftRuns, TmpRuns, RunCount * sizeof(NTFS_RUN));
        Vcb->MftRunCount = RunCount;
        Vcb->MftRecordCount = Attr->Form.NonResident.DataSize / Vcb->Geo.MftRecordSize;

        /* 3. root directory (record 5) */
        Vcb->RootFcb = NtfsCreateFcb(Vcb, MFT_RN_ROOT);
        if (Vcb->RootFcb == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; __leave; }
        Vcb->RootFcb->FcbState |= FCB_STATE_DIRECTORY | FCB_STATE_ROOT;

        /* volume label ($Volume:$VOLUME_NAME) and free-space count ($Bitmap) */
        {
            PUCHAR vr = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
            if (vr && NT_SUCCESS(NtfsReadMftRecord(Vcb, MFT_RN_VOLUME, vr))) {
                PATTRIBUTE_RECORD vn = NtfsFindAttribute((PMFT_RECORD)vr, ATTR_VOLUME_NAME, NULL, 0);
                PATTRIBUTE_RECORD vi = NtfsFindAttribute((PMFT_RECORD)vr, ATTR_VOLUME_INFORMATION, NULL, 0);
                Vcb->NtfsMajor = 3; Vcb->NtfsMinor = 1;
                if (vi && !vi->NonResident && NtfsResidentValueLength(vi) >= 10) {
                    Vcb->NtfsMajor = ((PVOLUME_INFORMATION)NtfsResidentValue(vi))->MajorVersion;
                    Vcb->NtfsMinor = ((PVOLUME_INFORMATION)NtfsResidentValue(vi))->MinorVersion;
                }
                if (vn && !vn->NonResident) {
                    ULONG l = NtfsResidentValueLength(vn);
                    if (l > sizeof(Vcb->VolumeLabel)) l = sizeof(Vcb->VolumeLabel);
                    l &= ~1u;
                    RtlCopyMemory(Vcb->VolumeLabel, NtfsResidentValue(vn), l);
                    Vcb->VolumeLabelLength = (USHORT)l;
                    RtlCopyMemory(Vpb->VolumeLabel, Vcb->VolumeLabel, l);
                    Vpb->VolumeLabelLength = (USHORT)l;
                }
            }
            if (vr) ExFreePool(vr);
            Vcb->FreeClusters = NtfsCountFreeClusters(Vcb);
            NtfsLoadUpcase(Vcb);
        }

        /* 4. publish the mount */
        Vpb->DeviceObject = VolumeDevice;
        Vpb->SerialNumber = (ULONG)Vcb->Geo.SerialNumber;
        Vcb->VcbState = VCB_STATE_MOUNTED;    /* writable (write path enabled) */
        InsertTailList(&NtfsData.VcbList, &Vcb->VcbLinks);

        DBGP("[NTFS3NT: mounted, "); DBGX((ULONG)Vcb->MftRecordCount); DBGP(" MFT records]\n");
        Status = STATUS_SUCCESS;
    }
    __finally {
        if (TmpRuns) ExFreePool(TmpRuns);
        if (Sector) ExFreePool(Sector);
        if (Rec0) ExFreePool(Rec0);
        if (!NT_SUCCESS(Status)) {
            if (Vcb->MftRuns) ExFreePool(Vcb->MftRuns);
            ExDeleteResourceLite(&Vcb->Resource);
            IoDeleteDevice(VolumeDevice);
        }
    }
    return Status;
}

#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 42, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/*
 * FSCTL_GET_REPARSE_POINT - copy the file's $REPARSE_POINT attribute data
 * (tag + GUID/length + reparse payload) into the output buffer. This is how
 * symbolic links, junctions and other reparse points are read back.
 */
static NTSTATUS
NtfsGetReparsePoint(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PNTFS_FCB Fcb = (PNTFS_FCB)IrpSp->FileObject->FsContext;
    PNTFS_VCB Vcb = Ctx->Vcb;
    PVOID Out = Ctx->Irp->AssociatedIrp.SystemBuffer;
    ULONG OutLen = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD rp;
    NTSTATUS Status;

    if (Fcb == NULL || Vcb == NULL) return STATUS_INVALID_PARAMETER;
    if (!(Fcb->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return STATUS_NOT_A_REPARSE_POINT;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec))) {
        ExFreePool(rec); return STATUS_UNSUCCESSFUL;
    }
    m = (PMFT_RECORD)rec;
    rp = NtfsFindAttribute(m, ATTR_REPARSE_POINT, NULL, 0);
    if (rp == NULL) { ExFreePool(rec); return STATUS_NOT_A_REPARSE_POINT; }

    if (!rp->NonResident) {
        ULONG vlen = NtfsResidentValueLength(rp);
        if (OutLen < vlen) {
            ExFreePool(rec);
            Ctx->Irp->IoStatus.Information = 0;
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlCopyMemory(Out, NtfsResidentValue(rp), vlen);
        Ctx->Irp->IoStatus.Information = vlen;
        Status = STATUS_SUCCESS;
    } else {
        /* reparse data is capped at 16 KB by NTFS, so in practice always
         * resident; read non-resident payload through the run list if needed. */
        PNTFS_RUN runs; ULONG rc;
        ULONG vlen = (ULONG)rp->Form.NonResident.DataSize;
        if (OutLen < vlen) { ExFreePool(rec); return STATUS_BUFFER_TOO_SMALL; }
        runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
                   NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (runs == NULL) { ExFreePool(rec); return STATUS_INSUFFICIENT_RESOURCES; }
        if (NtfsDecodeRuns(rp, runs, NTFS_MAX_IDX_RUNS, &rc) && rc)
            Status = NtfsReadAttributeData(Vcb, runs, rc, 0, vlen, Out);
        else
            Status = STATUS_UNSUCCESSFUL;
        if (NT_SUCCESS(Status)) Ctx->Irp->IoStatus.Information = vlen;
        ExFreePool(runs);
    }
    ExFreePool(rec);
    return Status;
}

NTSTATUS
NtfsCommonFileSystemControl(PNTFS_IRP_CONTEXT Ctx)
{
    switch (Ctx->IrpSp->MinorFunction) {
    case IRP_MN_MOUNT_VOLUME:
        return NtfsMountVolume(Ctx);
    case IRP_MN_VERIFY_VOLUME:
        return STATUS_SUCCESS;
    case IRP_MN_USER_FS_REQUEST: {
        ULONG code = Ctx->IrpSp->Parameters.FileSystemControl.FsControlCode;
        NTSTATUS st;
        if (NtfsVolumeFsctl(Ctx, &st)) return st;
        if (code == CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)) {
            /* FSCTL_IS_VOLUME_DIRTY: never dirty (no journal to replay) */
            if (Ctx->IrpSp->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG))
                return STATUS_INVALID_PARAMETER;
            *(PULONG)Ctx->Irp->AssociatedIrp.SystemBuffer = 0;
            Ctx->Irp->IoStatus.Information = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        if (code == CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 11, METHOD_BUFFERED, FILE_ANY_ACCESS))
            return STATUS_SUCCESS;                     /* FSCTL_IS_PATHNAME_VALID */
        if (code == FSCTL_GET_REPARSE_POINT && !NTFS_IS_VOLUME_FO(Ctx))
            return NtfsGetReparsePoint(Ctx);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}
