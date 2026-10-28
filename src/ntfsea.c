/*
 * NTFS3NT - extended attributes ($EA / $EA_INFORMATION) and the
 * IRP_MJ_QUERY_EA / IRP_MJ_SET_EA paths.
 *
 * NTFS stores EAs as a packed list in the file's $EA attribute, each entry in
 * the FILE_FULL_EA_INFORMATION layout (NextEntryOffset, Flags, EaNameLength,
 * EaValueLength, Name, 0, Value); $EA_INFORMATION holds the packed size. We
 * keep the list resident (EAs are small); a set that would not fit the record
 * is refused with STATUS_EA_TOO_LARGE.
 */

#define NTFS_KERNEL
#include "ntfs.h"

#pragma pack(push, 1)
typedef struct _NTFS_EA_INFO {          /* $EA_INFORMATION (0xD0) value */
    USHORT      PackedEaSize;
    USHORT      NeedEaCount;
    ULONG       UnpackedEaSize;
} NTFS_EA_INFO, *PNTFS_EA_INFO;
#pragma pack(pop)

#ifndef FILE_NEED_EA
#define FILE_NEED_EA 0x80
#endif

/* Load the file's $EA value into a pool buffer (NULL/0 if none). */
static PUCHAR
LoadEa(PNTFS_VCB Vcb, ULONGLONG RecNo, PULONG OutLen)
{
    PUCHAR rec, ea = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    ULONG len = 0;
    *OutLen = 0;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!rec) return NULL;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, RecNo, rec))) { ExFreePool(rec); return NULL; }
    m = (PMFT_RECORD)rec;
    a = NtfsFindAttribute(m, ATTR_EA, NULL, 0);
    if (a && !a->NonResident) {
        len = NtfsResidentValueLength(a);
        if (len) { ea = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, len, NTFS_TAG_BUF);
                   if (ea) RtlCopyMemory(ea, NtfsResidentValue(a), len); }
    } else if (a && a->NonResident) {
        PNTFS_RUN runs; ULONG rc;
        len = (ULONG)a->Form.NonResident.DataSize;
        runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (runs && len) {
            ea = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, len, NTFS_TAG_BUF);
            if (ea && NtfsDecodeRuns(a, runs, NTFS_MAX_IDX_RUNS, &rc) && rc) {
                if (!NT_SUCCESS(NtfsReadAttributeData(Vcb, runs, rc, 0, len, ea))) { ExFreePool(ea); ea = NULL; }
            } else if (ea) { ExFreePool(ea); ea = NULL; }
        }
        if (runs) ExFreePool(runs);
    }
    ExFreePool(rec);
    if (ea) *OutLen = len;
    return ea;
}

/* ------------------------------------------------------------------ */
/* QUERY_EA                                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
NtfsQueryEa(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PNTFS_FCB Fcb = (PNTFS_FCB)IrpSp->FileObject->FsContext;
    PNTFS_VCB Vcb = Ctx->Vcb;
    PUCHAR Out = (PUCHAR)(Ctx->Irp->AssociatedIrp.SystemBuffer ?
                          Ctx->Irp->AssociatedIrp.SystemBuffer : Ctx->Irp->UserBuffer);
    ULONG OutLen = IrpSp->Parameters.QueryEa.Length;
    BOOLEAN single = (BOOLEAN)((IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) != 0);
    PUCHAR ea; ULONG eaLen, in = 0, out = 0;
    PFILE_FULL_EA_INFORMATION last = NULL;

    if (!Fcb || !Vcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    ea = LoadEa(Vcb, Fcb->MftRecord, &eaLen);
    if (!ea || eaLen == 0) { if (ea) ExFreePool(ea); Ctx->Irp->IoStatus.Information = 0; return STATUS_NO_EAS_ON_FILE; }

    while (in < eaLen) {
        PFILE_FULL_EA_INFORMATION s = (PFILE_FULL_EA_INFORMATION)(ea + in);
        ULONG entry = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + s->EaNameLength + 1 + s->EaValueLength;
        ULONG aligned = (entry + 3) & ~3u;
        if (out + entry > OutLen) {
            ExFreePool(ea);
            Ctx->Irp->IoStatus.Information = out;
            return out ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL;
        }
        RtlCopyMemory(Out + out, s, entry);
        last = (PFILE_FULL_EA_INFORMATION)(Out + out);
        last->NextEntryOffset = (out + aligned <= OutLen && !single) ? aligned : 0;
        out += aligned;
        if (single) break;
        if (s->NextEntryOffset == 0) break;
        in += s->NextEntryOffset;
    }
    if (last) last->NextEntryOffset = 0;
    ExFreePool(ea);
    Ctx->Irp->IoStatus.Information = out;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SET_EA                                                              */
/* ------------------------------------------------------------------ */

/* Replace the file's $EA with the packed list `Ea` (EaLen bytes), recompute
 * $EA_INFORMATION, and set/clear FILE_ATTRIBUTE_EA. Resident only. */
static NTSTATUS
StoreEa(PNTFS_VCB Vcb, ULONGLONG RecNo, PUCHAR Ea, ULONG EaLen, USHORT NeedCount)
{
    PUCHAR rec = NULL, old = NULL, ab = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    ULONG alen;
    NTSTATUS st;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    old = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    ab  = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 0x18 + EaLen + 16, NTFS_TAG_BUF);
    if (!rec || !old || !ab) { st = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    st = NtfsReadMftRecord(Vcb, RecNo, rec);
    if (!NT_SUCCESS(st)) goto out;
    m = (PMFT_RECORD)rec;
    RtlCopyMemory(old, rec, Vcb->Geo.MftRecordSize);

    /* drop any existing $EA and $EA_INFORMATION */
    while ((a = NtfsFindAttribute(m, ATTR_EA, NULL, 0)) != NULL) NtfsRemoveAttribute(m, a);
    while ((a = NtfsFindAttribute(m, ATTR_EA_INFORMATION, NULL, 0)) != NULL) NtfsRemoveAttribute(m, a);

    if (EaLen) {
        /* $EA_INFORMATION */
        alen = (0x18 + sizeof(NTFS_EA_INFO) + 7) & ~7u;
        RtlZeroMemory(ab, alen);
        a = (PATTRIBUTE_RECORD)ab;
        a->Type = ATTR_EA_INFORMATION; a->Length = alen; a->NameOffset = 0x18;
        a->Form.Resident.ValueLength = sizeof(NTFS_EA_INFO); a->Form.Resident.ValueOffset = 0x18;
        { PNTFS_EA_INFO ei = (PNTFS_EA_INFO)(ab + 0x18);
          ei->PackedEaSize = (USHORT)EaLen; ei->NeedEaCount = NeedCount; ei->UnpackedEaSize = EaLen; }
        if (!NtfsInsertAttribute(m, Vcb->Geo.MftRecordSize, ab, alen)) { st = STATUS_EA_TOO_LARGE; goto out; }

        /* $EA */
        alen = (0x18 + EaLen + 7) & ~7u;
        RtlZeroMemory(ab, alen);
        a = (PATTRIBUTE_RECORD)ab;
        a->Type = ATTR_EA; a->Length = alen; a->NameOffset = 0x18;
        a->Form.Resident.ValueLength = EaLen; a->Form.Resident.ValueOffset = 0x18;
        RtlCopyMemory(ab + 0x18, Ea, EaLen);
        if (!NtfsInsertAttribute(m, Vcb->Geo.MftRecordSize, ab, alen)) {
            /* undo the info attr if the data would not fit */
            PATTRIBUTE_RECORD ii = NtfsFindAttribute(m, ATTR_EA_INFORMATION, NULL, 0);
            if (ii) NtfsRemoveAttribute(m, ii);
            st = STATUS_EA_TOO_LARGE; goto out;
        }
    }
    /* FILE_ATTRIBUTE_EA (0x40) in $STD_INFO */
    { PATTRIBUTE_RECORD si = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
      if (si && !si->NonResident && NtfsResidentValueLength(si) >= 0x30) {
          PSTANDARD_INFORMATION s = (PSTANDARD_INFORMATION)NtfsResidentValue(si);
          if (EaLen) s->FileAttributes |= 0x40; else s->FileAttributes &= ~0x40u;
      } }

    NtfsJnlRecord(Vcb, RecNo, old, rec);
    st = NtfsWriteMftRecord(Vcb, RecNo, rec);
out:
    if (rec) ExFreePool(rec);
    if (old) ExFreePool(old);
    if (ab) ExFreePool(ab);
    return st;
}

NTSTATUS
NtfsSetEa(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PNTFS_FCB Fcb = (PNTFS_FCB)IrpSp->FileObject->FsContext;
    PNTFS_VCB Vcb = Ctx->Vcb;
    PUCHAR In = (PUCHAR)Ctx->Irp->AssociatedIrp.SystemBuffer;
    ULONG InLen = IrpSp->Parameters.SetEa.Length;
    PUCHAR packed = NULL;
    ULONG off = 0, out = 0; USHORT need = 0;
    NTSTATUS st;

    if (!Fcb || !Vcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    if (!NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;
    if (!In || InLen == 0) return STATUS_INVALID_PARAMETER;

    /* pack the caller's FILE_FULL_EA_INFORMATION list contiguously (4-aligned) */
    packed = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, InLen + 8, NTFS_TAG_BUF);
    if (!packed) return STATUS_INSUFFICIENT_RESOURCES;
    for (;;) {
        PFILE_FULL_EA_INFORMATION e = (PFILE_FULL_EA_INFORMATION)(In + off);
        ULONG elen = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + e->EaNameLength + 1 + e->EaValueLength;
        ULONG al = (elen + 3) & ~3u;
        if (off + FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) > InLen) break;
        RtlCopyMemory(packed + out, e, elen);
        ((PFILE_FULL_EA_INFORMATION)(packed + out))->NextEntryOffset = al;
        if (e->Flags & FILE_NEED_EA) need++;
        out += al;
        if (e->NextEntryOffset == 0) break;
        off += e->NextEntryOffset;
    }
    if (out) ((PFILE_FULL_EA_INFORMATION)(packed + out - ((out>=4)?4:0)))->NextEntryOffset = 0;

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    st = StoreEa(Vcb, Fcb->MftRecord, packed, out, need);
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(st)) { NtfsJnlCommit(Vcb); Fcb->FcbState |= FCB_STATE_MODIFIED; NtfsReportUsn(Vcb, Fcb->MftRecord, 0x00000400 /*USN_REASON_EA_CHANGE*/); }
    else NtfsJnlAbort(Vcb);
    ExFreePool(packed);
    return st;
}

/* Return the packed EA size for FileEaInformation / dir dup info. */
ULONG
NtfsEaSize(PNTFS_VCB Vcb, ULONGLONG RecNo)
{
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    ULONG sz = 0;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!rec) return 0;
    if (NT_SUCCESS(NtfsReadMftRecord(Vcb, RecNo, rec))) {
        m = (PMFT_RECORD)rec;
        a = NtfsFindAttribute(m, ATTR_EA_INFORMATION, NULL, 0);
        if (a && !a->NonResident && NtfsResidentValueLength(a) >= sizeof(NTFS_EA_INFO))
            sz = ((PNTFS_EA_INFO)NtfsResidentValue(a))->PackedEaSize;
    }
    ExFreePool(rec);
    return sz;
}
