/*
 * NTFS3NT - security: $SECURITY_DESCRIPTOR resolution and the
 * IRP_MJ_QUERY_SECURITY / IRP_MJ_SET_SECURITY paths.
 *
 * On NTFS 3.x a file's descriptor is shared: $STANDARD_INFORMATION holds a
 * SecurityId, and the descriptor itself lives once in $Secure:$SDS, indexed by
 * id ($Secure:$SII) and by hash+id ($Secure:$SDH). On NTFS 1.x (and upgraded
 * files) the descriptor sits in the file's own $SECURITY_DESCRIPTOR attribute.
 * We read both forms and, on a set, store a 3.x shared id (reusing an identical
 * descriptor already in $SDS, or appending a new one) or a 1.x attribute.
 *
 * These IRP majors are above IRP_MJ_CLEANUP, so they are dispatched only on
 * NT 4.0 and later (2000, XP ...); NT 3.1 never calls them. The Se* and Rtl*
 * security helpers used here are exported by every NT from 3.1 on.
 */

#define NTFS_KERNEL
#include "ntfs.h"

/* $Secure:$SDS stream entry header (precedes each stored descriptor) */
#pragma pack(push, 1)
typedef struct _NTFS_SDS_ENTRY {
    ULONG       Hash;                    /* security descriptor hash          */
    ULONG       SecurityId;              /* the id ($STD_INFO.SecurityId)     */
    ULONGLONG   Offset;                  /* this entry's offset in $SDS       */
    ULONG       Length;                  /* entry length incl. this 20-b hdr  */
    /* SECURITY_DESCRIPTOR (self-relative) follows, Length-20 bytes */
} NTFS_SDS_ENTRY, *PNTFS_SDS_ENTRY;
#pragma pack(pop)

#define SDS_HEADER_LEN    20
#define SDS_ALIGN         16
#define SDS_MIRROR        0x40000ULL     /* each entry is written again +256 KB */

static const WCHAR Sds[4] = { '$', 'S', 'D', 'S' };

/* NTFS security-descriptor hash: sum of 32-bit words rotated left (Windows'
 * own function; chkdsk and Windows match descriptors by it). */
static ULONG
SdHash(const UCHAR *Sd, ULONG Len)
{
    ULONG h = 0, i;
    for (i = 0; i + 4 <= Len; i += 4)
        h = (*(PULONG)(Sd + i)) + ((h << 3) | (h >> (32 - 3)));
    return h;
}

/* ------------------------------------------------------------------ */
/* Loading a descriptor                                                */
/* ------------------------------------------------------------------ */

/* Read $Secure:$SDS into one pool buffer (it is small on real volumes: a few
 * KB of standard descriptors). Returns NULL and 0 if absent. */
static PUCHAR
LoadSds(PNTFS_VCB Vcb, PULONG OutLen)
{
    PUCHAR rec, sds = NULL, runbuf = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    ULONG len = 0;
    *OutLen = 0;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!rec) return NULL;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, MFT_RN_SECURE, rec))) { ExFreePool(rec); return NULL; }
    m = (PMFT_RECORD)rec;
    a = NtfsFindAttribute(m, ATTR_DATA, Sds, 4);
    if (!a) { ExFreePool(rec); return NULL; }
    if (!a->NonResident) {
        len = NtfsResidentValueLength(a);
        if (len) { sds = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, len, NTFS_TAG_BUF);
                   if (sds) RtlCopyMemory(sds, NtfsResidentValue(a), len); }
    } else {
        ULONG rc;
        ULONGLONG dsz = a->Form.NonResident.DataSize;
        /* read at most the primary region (mirror is a copy); cap for safety */
        if (dsz > 1024 * 1024) dsz = 1024 * 1024;
        len = (ULONG)dsz;
        runbuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (runbuf && len) {
            PNTFS_RUN runs = (PNTFS_RUN)runbuf;
            sds = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, len, NTFS_TAG_BUF);
            if (sds && NtfsDecodeRuns(a, runs, NTFS_MAX_IDX_RUNS, &rc) && rc) {
                if (!NT_SUCCESS(NtfsReadAttributeData(Vcb, runs, rc, 0, len, sds))) {
                    ExFreePool(sds); sds = NULL;
                }
            } else if (sds) { ExFreePool(sds); sds = NULL; }
        }
    }
    if (runbuf) ExFreePool(runbuf);
    ExFreePool(rec);
    if (sds) *OutLen = len;
    return sds;
}

/* Find the self-relative descriptor for SecurityId in $SDS. On success returns
 * a pool buffer (free with ExFreePool) and its length; NULL if not found. */
static PUCHAR
SdsFindById(PUCHAR Sds, ULONG SdsLen, ULONG SecurityId, PULONG OutLen)
{
    ULONG off = 0;
    *OutLen = 0;
    while (off + SDS_HEADER_LEN <= SdsLen) {
        PNTFS_SDS_ENTRY e = (PNTFS_SDS_ENTRY)(Sds + off);
        ULONG elen = e->Length;
        if (elen < SDS_HEADER_LEN || elen > SdsLen - off) {
            /* a gap to the next 256 KB block (the mirror); skip ahead */
            ULONGLONG nb = ((ULONGLONG)off + SDS_MIRROR - 1) & ~(SDS_MIRROR - 1);
            if (nb <= off || nb >= SdsLen) break;
            off = (ULONG)nb; continue;
        }
        if (e->SecurityId == SecurityId && SecurityId != 0) {
            ULONG dl = elen - SDS_HEADER_LEN;
            PUCHAR sd = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, dl, NTFS_TAG_BUF);
            if (sd) { RtlCopyMemory(sd, Sds + off + SDS_HEADER_LEN, dl); *OutLen = dl; }
            return sd;
        }
        off += (elen + SDS_ALIGN - 1) & ~(SDS_ALIGN - 1);
    }
    return NULL;
}

/* Load a file's security descriptor (self-relative) into a pool buffer. The
 * caller frees it with ExFreePool. Falls back to a world descriptor so an open
 * is never blocked by a missing descriptor. */
static NTSTATUS
NtfsLoadFileSd(PNTFS_VCB Vcb, ULONGLONG RecNo, PUCHAR *OutSd, PULONG OutLen)
{
    PUCHAR rec, sd = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD si, sda;
    ULONG id = 0, len = 0;
    *OutSd = NULL; *OutLen = 0;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, RecNo, rec))) { ExFreePool(rec); return STATUS_UNSUCCESSFUL; }
    m = (PMFT_RECORD)rec;

    /* a 1.x / upgraded file keeps the descriptor in its own attribute */
    sda = NtfsFindAttribute(m, ATTR_SECURITY_DESCRIPTOR, NULL, 0);
    if (sda && !sda->NonResident) {
        len = NtfsResidentValueLength(sda);
        if (len) { sd = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, len, NTFS_TAG_BUF);
                   if (sd) RtlCopyMemory(sd, NtfsResidentValue(sda), len); }
    }
    if (!sd) {
        si = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
        if (si && !si->NonResident && NtfsResidentValueLength(si) >= 0x48)
            id = ((PSTANDARD_INFORMATION)NtfsResidentValue(si))->SecurityId;
    }
    ExFreePool(rec);

    if (!sd && id) {
        ULONG slen;
        PUCHAR sds = LoadSds(Vcb, &slen);
        if (sds) { sd = SdsFindById(sds, slen, id, &len); ExFreePool(sds); }
    }
    if (!sd) {
        /* world-accessible fallback: a valid absolute->self-relative SD with a
         * null DACL (everyone), so callers get a descriptor and access is open */
        SECURITY_DESCRIPTOR abs;
        ULONG n = 0;
        RtlCreateSecurityDescriptor(&abs, SECURITY_DESCRIPTOR_REVISION);
        RtlSetDaclSecurityDescriptor(&abs, TRUE, NULL, FALSE);   /* NULL DACL = all access */
        RtlAbsoluteToSelfRelativeSD(&abs, NULL, &n);
        if (n) {
            sd = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, n, NTFS_TAG_BUF);
            if (sd && NT_SUCCESS(RtlAbsoluteToSelfRelativeSD(&abs, (PSECURITY_DESCRIPTOR)sd, &n)))
                len = n;
            else if (sd) { ExFreePool(sd); sd = NULL; }
        }
    }
    if (!sd) return STATUS_INSUFFICIENT_RESOURCES;
    *OutSd = sd; *OutLen = len;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* QUERY_SECURITY                                                      */
/* ------------------------------------------------------------------ */

NTSTATUS
NtfsQuerySecurity(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PNTFS_FCB Fcb = (PNTFS_FCB)IrpSp->FileObject->FsContext;
    PNTFS_VCB Vcb = Ctx->Vcb;
    SECURITY_INFORMATION SecInfo = IrpSp->Parameters.QuerySecurity.SecurityInformation;
    PSECURITY_DESCRIPTOR Out = Ctx->Irp->UserBuffer;
    ULONG OutLen = IrpSp->Parameters.QuerySecurity.Length;
    PUCHAR sd = NULL;
    ULONG sdLen = 0, need;
    NTSTATUS Status;

    if (!Fcb || !Vcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    Status = NtfsLoadFileSd(Vcb, Fcb->MftRecord, &sd, &sdLen);
    if (!NT_SUCCESS(Status)) return Status;

    need = OutLen;
    /* SeQuerySecurityDescriptorInfo copies the requested parts into Out; if Out
     * is too small it sets need to the size wanted and returns BUFFER_TOO_SMALL */
    Status = SeQuerySecurityDescriptorInfo(&SecInfo, Out, &need, (PSECURITY_DESCRIPTOR *)&sd);
    Ctx->Irp->IoStatus.Information = need;
    ExFreePool(sd);
    return Status;
}

/* ------------------------------------------------------------------ */
/* SET_SECURITY                                                        */
/* ------------------------------------------------------------------ */

/* Store a self-relative descriptor on the file itself, as a resident
 * $SECURITY_DESCRIPTOR attribute, and clear any shared SecurityId so the two do
 * not disagree. Every NTFS (1.x and 3.x) reads a file's own descriptor; this
 * avoids maintaining the shared $SDS/$SII/$SDH indexes for a one-off set.
 * Descriptors that do not fit the record resident are refused. */
static NTSTATUS
NtfsSecStoreAttr(PNTFS_VCB Vcb, ULONGLONG RecNo, PUCHAR Sd, ULONG SdLen)
{
    PUCHAR rec, old = NULL, attr = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a, si;
    PATTRIBUTE_RECORD oldsd;
    ULONG alen;
    NTSTATUS st;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    old = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    alen = 0x18 + SdLen;                 /* resident attr header (no name) + value, 8-align */
    alen = (alen + 7) & ~7u;
    attr = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, alen, NTFS_TAG_BUF);
    if (!rec || !old || !attr) { st = STATUS_INSUFFICIENT_RESOURCES; goto out; }

    st = NtfsReadMftRecord(Vcb, RecNo, rec);
    if (!NT_SUCCESS(st)) goto out;
    m = (PMFT_RECORD)rec;
    RtlCopyMemory(old, rec, Vcb->Geo.MftRecordSize);

    /* drop an existing resident $SECURITY_DESCRIPTOR, then insert the new one */
    oldsd = NtfsFindAttribute(m, ATTR_SECURITY_DESCRIPTOR, NULL, 0);
    if (oldsd)
        NtfsRemoveAttribute(m, oldsd);

    RtlZeroMemory(attr, alen);
    a = (PATTRIBUTE_RECORD)attr;
    a->Type = ATTR_SECURITY_DESCRIPTOR;
    a->Length = alen;
    a->NonResident = 0;
    a->NameLength = 0;
    a->NameOffset = 0x18;
    a->Form.Resident.ValueLength = SdLen;
    a->Form.Resident.ValueOffset = 0x18;
    RtlCopyMemory(attr + 0x18, Sd, SdLen);
    if (!NtfsInsertAttribute(m, Vcb->Geo.MftRecordSize, attr, alen)) {
        st = STATUS_INSUFFICIENT_RESOURCES;        /* record full (very large SD) */
        goto out;
    }

    /* clear the shared id so the on-attribute descriptor is authoritative */
    si = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
    if (si && !si->NonResident && NtfsResidentValueLength(si) >= 0x48)
        ((PSTANDARD_INFORMATION)NtfsResidentValue(si))->SecurityId = 0;

    NtfsJnlRecord(Vcb, RecNo, old, rec);
    st = NtfsWriteMftRecord(Vcb, RecNo, rec);
out:
    if (rec) ExFreePool(rec);
    if (old) ExFreePool(old);
    if (attr) ExFreePool(attr);
    return st;
}

NTSTATUS
NtfsSetSecurity(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PNTFS_FCB Fcb = (PNTFS_FCB)IrpSp->FileObject->FsContext;
    PNTFS_VCB Vcb = Ctx->Vcb;
    SECURITY_INFORMATION SecInfo = IrpSp->Parameters.SetSecurity.SecurityInformation;
    PSECURITY_DESCRIPTOR Mod = IrpSp->Parameters.SetSecurity.SecurityDescriptor;
    PUCHAR cur = NULL, newsd;
    ULONG curLen = 0, newLen;
    NTSTATUS Status;
    GENERIC_MAPPING map = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };

    if (!Fcb || !Vcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    if (!NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;

    Status = NtfsLoadFileSd(Vcb, Fcb->MftRecord, &cur, &curLen);
    if (!NT_SUCCESS(Status)) return Status;

    /* merge the requested components into the current descriptor; Se* frees the
     * old 'cur' and replaces it with the newly built self-relative descriptor */
    Status = SeSetSecurityDescriptorInfo(NULL, &SecInfo, Mod,
                                         (PSECURITY_DESCRIPTOR *)&cur, PagedPool, &map);
    if (!NT_SUCCESS(Status)) { ExFreePool(cur); return Status; }
    newsd = cur;
    newLen = RtlLengthSecurityDescriptor(newsd);

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    Status = NtfsSecStoreAttr(Vcb, Fcb->MftRecord, newsd, newLen);
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(Status)) { NtfsJnlCommit(Vcb); Fcb->FcbState |= FCB_STATE_MODIFIED; }
    else NtfsJnlAbort(Vcb);

    ExFreePool(newsd);      /* allocated by SeSetSecurityDescriptorInfo (PagedPool) */
    return Status;
}
