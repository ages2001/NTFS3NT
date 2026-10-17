/*
 * NTFS3NT - query file information and volume information (read-only subset).
 */

#define NTFS_KERNEL
#include "ntfs.h"

/*
 * The full path of record Rec from the volume root ("\dir\file"; the root
 * itself is "\"), from the $FILE_NAME parent links - what FileNameInformation
 * returns. NT 3.1's GetVolumeInformation and GetDiskFreeSpace check that the
 * root opens as "\" before they ask anything else. Returns the characters
 * written to the end of Out (Out + MaxChars - n), or 0 if the walk fails.
 */
static ULONG
NtfsFullPath(PNTFS_VCB Vcb, ULONGLONG Rec, PWCHAR Out, ULONG MaxChars)
{
    ULONG rs = Vcb->Geo.MftRecordSize, n = 0, depth;
    PUCHAR buf;
    if (Rec == MFT_RN_ROOT) { if (MaxChars < 1) return 0; Out[MaxChars - 1] = L'\\'; return 1; }
    buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, rs, NTFS_TAG_BUF);
    if (buf == NULL) return 0;
    for (depth = 0; depth < 64 && Rec != MFT_RN_ROOT; depth++) {
        PMFT_RECORD m = (PMFT_RECORD)buf;
        PFILE_NAME_ATTR best = NULL;
        ULONG off;
        if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, Rec, buf))) { n = 0; break; }
        for (off = m->FirstAttributeOffset; off + 8 <= rs; ) {
            PATTRIBUTE_RECORD a = (PATTRIBUTE_RECORD)(buf + off);
            if (a->Type == ATTR_END || a->Length == 0 || off + a->Length > rs) break;
            if (a->Type == ATTR_FILE_NAME && !a->NonResident &&
                NtfsResidentValueLength(a) >= sizeof(FILE_NAME_ATTR)) {
                PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)NtfsResidentValue(a);
                if (best == NULL || best->NameType == 2) best = fn;      /* not the DOS name */
            }
            off += a->Length;
        }
        if (best == NULL || n + best->NameLength + 1 > MaxChars) { n = 0; break; }
        n += best->NameLength;
        RtlCopyMemory(Out + MaxChars - n, (PUCHAR)best + sizeof(FILE_NAME_ATTR), best->NameLength * sizeof(WCHAR));
        n++;
        Out[MaxChars - n] = L'\\';
        Rec = NTFS_MREF_RECORD(best->ParentDirectory);
    }
    if (Rec != MFT_RN_ROOT) n = 0;
    ExFreePool(buf);
    return n;
}

NTSTATUS
NtfsCommonQueryInformation(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PNTFS_FCB Fcb = (PNTFS_FCB)IrpSp->FileObject->FsContext;
    PNTFS_CCB Ccb = (PNTFS_CCB)IrpSp->FileObject->FsContext2;
    FILE_INFORMATION_CLASS Class = IrpSp->Parameters.QueryFile.FileInformationClass;
    PVOID Buffer = Ctx->Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = IrpSp->Parameters.QueryFile.Length;
    BOOLEAN IsDir;
    BOOLEAN NamedStream;
    LARGE_INTEGER EndOfFile, AllocSize;

    if (Fcb == NULL) return STATUS_INVALID_DEVICE_REQUEST;
    IsDir = (BOOLEAN)((Fcb->FcbState & FCB_STATE_DIRECTORY) != 0);
    NamedStream = (BOOLEAN)(Ccb && Ccb->NodeTypeCode == NTFS_NTC_CCB && Ccb->NamedStream);
    /* a named-stream handle reports the stream's size, not the main file's */
    if (NamedStream) {
        EndOfFile.QuadPart = (LONGLONG)Ccb->StreamSize;
        AllocSize.QuadPart = (EndOfFile.QuadPart + Fcb->Vcb->Geo.ClusterSize - 1) &
                             ~((LONGLONG)Fcb->Vcb->Geo.ClusterSize - 1);
        IsDir = FALSE;
    } else {
        EndOfFile = Fcb->Header.FileSize;
        AllocSize = Fcb->Header.AllocationSize;
    }
    RtlZeroMemory(Buffer, Length);

    switch (Class) {

    case FileBasicInformation: {
        PFILE_BASIC_INFORMATION bi = (PFILE_BASIC_INFORMATION)Buffer;
        if (Length < sizeof(*bi)) return STATUS_BUFFER_TOO_SMALL;
        bi->CreationTime.QuadPart = (LONGLONG)Fcb->CreationTime;
        bi->LastAccessTime.QuadPart = (LONGLONG)Fcb->LastAccessTime;
        bi->LastWriteTime.QuadPart = (LONGLONG)Fcb->LastWriteTime;
        bi->ChangeTime.QuadPart = (LONGLONG)Fcb->ChangeTime;
        bi->FileAttributes = Fcb->FileAttributes ? Fcb->FileAttributes :
            (IsDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        Ctx->Irp->IoStatus.Information = sizeof(*bi);
        return STATUS_SUCCESS;
    }
    case FileStandardInformation: {
        PFILE_STANDARD_INFORMATION si = (PFILE_STANDARD_INFORMATION)Buffer;
        if (Length < sizeof(*si)) return STATUS_BUFFER_TOO_SMALL;
        si->EndOfFile = EndOfFile;
        si->AllocationSize = AllocSize;
        si->NumberOfLinks = 1;
        si->Directory = IsDir;
        Ctx->Irp->IoStatus.Information = sizeof(*si);
        return STATUS_SUCCESS;
    }
    case FilePositionInformation: {
        PFILE_POSITION_INFORMATION pi = (PFILE_POSITION_INFORMATION)Buffer;
        if (Length < sizeof(*pi)) return STATUS_BUFFER_TOO_SMALL;
        pi->CurrentByteOffset = IrpSp->FileObject->CurrentByteOffset;
        Ctx->Irp->IoStatus.Information = sizeof(*pi);
        return STATUS_SUCCESS;
    }
    case FileInternalInformation: {
        PFILE_INTERNAL_INFORMATION ii = (PFILE_INTERNAL_INFORMATION)Buffer;
        if (Length < sizeof(*ii)) return STATUS_BUFFER_TOO_SMALL;
        ii->IndexNumber.QuadPart = (LONGLONG)Fcb->MftRecord;
        Ctx->Irp->IoStatus.Information = sizeof(*ii);
        return STATUS_SUCCESS;
    }
    case FileEaInformation: {
        PFILE_EA_INFORMATION ei = (PFILE_EA_INFORMATION)Buffer;
        if (Length < sizeof(*ei)) return STATUS_BUFFER_TOO_SMALL;
        ei->EaSize = 0;
        Ctx->Irp->IoStatus.Information = sizeof(*ei);
        return STATUS_SUCCESS;
    }
    case FileNameInformation: {
        /* the full path from the volume root; the leaf name if the walk fails */
        PFILE_NAME_INFORMATION ni = (PFILE_NAME_INFORMATION)Buffer;
        PWCHAR path = (PWCHAR)ExAllocatePoolWithTag(PagedPool, 1024 * sizeof(WCHAR), NTFS_TAG_BUF);
        ULONG chars = path ? NtfsFullPath(Fcb->Vcb, Fcb->MftRecord, path, 1024) : 0;
        PWCHAR src = chars ? path + 1024 - chars : Fcb->FileName;
        ULONG len = chars ? chars * sizeof(WCHAR) : Fcb->FileNameLength;
        ULONG copy;
        if (Length < FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + sizeof(WCHAR)) {
            if (path) ExFreePool(path);
            return STATUS_BUFFER_TOO_SMALL;
        }
        ni->FileNameLength = len;
        copy = len;
        if (copy > Length - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName))
            copy = Length - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
        if (copy) RtlCopyMemory(ni->FileName, src, copy);
        if (path) ExFreePool(path);
        Ctx->Irp->IoStatus.Information =
            FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + copy;
        return (copy == len) ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
    }
    case FileNetworkOpenInformation: {
        PFILE_NETWORK_OPEN_INFORMATION no = (PFILE_NETWORK_OPEN_INFORMATION)Buffer;
        if (Length < sizeof(*no)) return STATUS_BUFFER_TOO_SMALL;
        no->CreationTime.QuadPart = (LONGLONG)Fcb->CreationTime;
        no->LastAccessTime.QuadPart = (LONGLONG)Fcb->LastAccessTime;
        no->LastWriteTime.QuadPart = (LONGLONG)Fcb->LastWriteTime;
        no->ChangeTime.QuadPart = (LONGLONG)Fcb->ChangeTime;
        no->AllocationSize = AllocSize;
        no->EndOfFile = EndOfFile;
        no->FileAttributes = Fcb->FileAttributes ? Fcb->FileAttributes :
            (IsDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        Ctx->Irp->IoStatus.Information = sizeof(*no);
        return STATUS_SUCCESS;
    }
    case FileAttributeTagInformation: {
        PFILE_ATTRIBUTE_TAG_INFORMATION ti = (PFILE_ATTRIBUTE_TAG_INFORMATION)Buffer;
        if (Length < sizeof(*ti)) return STATUS_BUFFER_TOO_SMALL;
        ti->FileAttributes = Fcb->FileAttributes ? Fcb->FileAttributes :
            (IsDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        ti->ReparseTag = Fcb->ReparseTag;
        Ctx->Irp->IoStatus.Information = sizeof(*ti);
        return STATUS_SUCCESS;
    }
    case FileAllInformation: {
        PFILE_ALL_INFORMATION ai = (PFILE_ALL_INFORMATION)Buffer;
        ULONG hdr = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation);
        if (Length < hdr) return STATUS_BUFFER_TOO_SMALL;
        ai->StandardInformation.EndOfFile = EndOfFile;
        ai->StandardInformation.AllocationSize = AllocSize;
        ai->StandardInformation.NumberOfLinks = 1;
        ai->StandardInformation.Directory = IsDir;
        ai->BasicInformation.CreationTime.QuadPart = (LONGLONG)Fcb->CreationTime;
        ai->BasicInformation.LastAccessTime.QuadPart = (LONGLONG)Fcb->LastAccessTime;
        ai->BasicInformation.LastWriteTime.QuadPart = (LONGLONG)Fcb->LastWriteTime;
        ai->BasicInformation.ChangeTime.QuadPart = (LONGLONG)Fcb->ChangeTime;
        ai->BasicInformation.FileAttributes = Fcb->FileAttributes ? Fcb->FileAttributes :
            (IsDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        ai->InternalInformation.IndexNumber.QuadPart = (LONGLONG)Fcb->MftRecord;
        ai->PositionInformation.CurrentByteOffset = IrpSp->FileObject->CurrentByteOffset;
        ai->NameInformation.FileNameLength = Fcb->FileNameLength;
        {
            ULONG avail = Length - hdr - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
            ULONG copy = Fcb->FileNameLength;
            if ((LONG)avail < 0) avail = 0;
            if (copy > avail) copy = avail;
            if (copy) RtlCopyMemory(ai->NameInformation.FileName, Fcb->FileName, copy);
            Ctx->Irp->IoStatus.Information = hdr +
                FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + copy;
            return (copy == Fcb->FileNameLength) ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
        }
    }
    case FileStreamInformation: {
        /* enumerate the file's $DATA streams: ::$DATA plus any :name:$DATA */
        PUCHAR rec;
        PMFT_RECORD m;
        PATTRIBUTE_RECORD a;
        PFILE_STREAM_INFORMATION prev = NULL;
        ULONG used = 0;
        static const WCHAR kData[] = { ':','$','D','A','T','A' };

        rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Fcb->Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
        if (rec == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        if (!NT_SUCCESS(NtfsReadMftRecord(Fcb->Vcb, Fcb->MftRecord, rec))) {
            ExFreePool(rec); return STATUS_UNSUCCESSFUL;
        }
        m = (PMFT_RECORD)rec;
        for (a = NtfsFirstAttribute(m); a != NULL; a = NtfsNextAttribute(m, a)) {
            ULONGLONG ssz, salloc;
            ULONG nameChars, nmBytes, entryLen, pad;
            PFILE_STREAM_INFORMATION e;
            PWCHAR an;
            if (a->Type != ATTR_DATA) {
                if (a->Type == ATTR_END) break;
                continue;
            }
            nameChars = a->NameLength;
            an = NtfsAttributeName(a);
            /* name is ":" + attr-name + ":$DATA" */
            nmBytes = (ULONG)((1 + nameChars + 6) * sizeof(WCHAR));
            entryLen = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) + nmBytes;
            pad = (entryLen + 7) & ~7u;
            if (used + pad > Length) {
                ExFreePool(rec);
                Ctx->Irp->IoStatus.Information = used;
                return STATUS_BUFFER_OVERFLOW;
            }
            e = (PFILE_STREAM_INFORMATION)((PUCHAR)Buffer + used);
            if (!a->NonResident) {
                ssz = NtfsResidentValueLength(a);
                salloc = (ssz + Fcb->Vcb->Geo.ClusterSize - 1) & ~((ULONGLONG)Fcb->Vcb->Geo.ClusterSize - 1);
            } else {
                ssz = a->Form.NonResident.DataSize;
                salloc = a->Form.NonResident.AllocatedSize;
            }
            e->NextEntryOffset = 0;
            e->StreamNameLength = nmBytes;
            e->StreamSize.QuadPart = (LONGLONG)ssz;
            e->StreamAllocationSize.QuadPart = (LONGLONG)salloc;
            {
                PWCHAR d = e->StreamName; ULONG k;
                *d++ = L':';
                for (k = 0; k < nameChars; k++) *d++ = an[k];
                for (k = 0; k < 6; k++) *d++ = kData[k];
            }
            if (prev) prev->NextEntryOffset = (ULONG)((PUCHAR)e - (PUCHAR)prev);
            prev = e;
            used += pad;
        }
        ExFreePool(rec);
        Ctx->Irp->IoStatus.Information = used;
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS
NtfsCommonQueryVolumeInformation(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PNTFS_VCB Vcb = Ctx->Vcb;
    FS_INFORMATION_CLASS Class = IrpSp->Parameters.QueryVolume.FsInformationClass;
    PVOID Buffer = Ctx->Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;

    if (Vcb == NULL) return STATUS_INVALID_DEVICE_REQUEST;
    RtlZeroMemory(Buffer, Length);

    switch (Class) {

    case FileFsVolumeInformation: {
        PFILE_FS_VOLUME_INFORMATION vi = (PFILE_FS_VOLUME_INFORMATION)Buffer;
        ULONG hdr = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
        ULONG copy = Vcb->VolumeLabelLength;
        if (Length < hdr) return STATUS_BUFFER_TOO_SMALL;
        vi->VolumeSerialNumber = (ULONG)Vcb->Geo.SerialNumber;
        vi->VolumeLabelLength = Vcb->VolumeLabelLength;
        if (copy > Length - hdr) copy = Length - hdr;
        if (copy) RtlCopyMemory(vi->VolumeLabel, Vcb->VolumeLabel, copy);
        Ctx->Irp->IoStatus.Information = hdr + copy;
        return (copy == Vcb->VolumeLabelLength) ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
    }
    case FileFsSizeInformation: {
        PFILE_FS_SIZE_INFORMATION si = (PFILE_FS_SIZE_INFORMATION)Buffer;
        if (Length < sizeof(*si)) return STATUS_BUFFER_TOO_SMALL;
        si->TotalAllocationUnits.QuadPart = (LONGLONG)Vcb->Geo.TotalClusters;
        si->AvailableAllocationUnits.QuadPart = (LONGLONG)Vcb->FreeClusters;
        si->SectorsPerAllocationUnit = Vcb->Geo.SectorsPerCluster;
        si->BytesPerSector = Vcb->Geo.BytesPerSector;
        Ctx->Irp->IoStatus.Information = sizeof(*si);
        return STATUS_SUCCESS;
    }
    case 7: /* FileFsFullSizeInformation (GetDiskFreeSpaceEx; Windows 2000+) */ {
        struct { LARGE_INTEGER Total, CallerAvail, ActualAvail; ULONG SectorsPerUnit, BytesPerSector; }
            *fi = (void *)Buffer;
        if (Length < sizeof(*fi)) return STATUS_BUFFER_TOO_SMALL;
        fi->Total.QuadPart = (LONGLONG)Vcb->Geo.TotalClusters;
        fi->CallerAvail.QuadPart = (LONGLONG)Vcb->FreeClusters;
        fi->ActualAvail.QuadPart = (LONGLONG)Vcb->FreeClusters;
        fi->SectorsPerUnit = Vcb->Geo.SectorsPerCluster;
        fi->BytesPerSector = Vcb->Geo.BytesPerSector;
        Ctx->Irp->IoStatus.Information = sizeof(*fi);
        return STATUS_SUCCESS;
    }
    case FileFsAttributeInformation: {
        PFILE_FS_ATTRIBUTE_INFORMATION ai = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
        ULONG need = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + 4 * sizeof(WCHAR);
        if (Length < need) return STATUS_BUFFER_TOO_SMALL;
        /* case-preserved, Unicode on disk, named streams (ADS); no ACLs and no
         * compression *writes* are claimed (compressed files are read-only) */
        ai->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | 0x00040000;
        ai->MaximumComponentNameLength = 255;
        ai->FileSystemNameLength = 4 * sizeof(WCHAR);
        ai->FileSystemName[0] = 'N'; ai->FileSystemName[1] = 'T';
        ai->FileSystemName[2] = 'F'; ai->FileSystemName[3] = 'S';
        Ctx->Irp->IoStatus.Information = need;
        return STATUS_SUCCESS;
    }
    case FileFsDeviceInformation: {
        PFILE_FS_DEVICE_INFORMATION di = (PFILE_FS_DEVICE_INFORMATION)Buffer;
        if (Length < sizeof(*di)) return STATUS_BUFFER_TOO_SMALL;
        di->DeviceType = FILE_DEVICE_DISK;
        di->Characteristics = 0;
        Ctx->Irp->IoStatus.Information = sizeof(*di);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}
