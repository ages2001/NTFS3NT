/*
 * NTFS3NT - file write path. Writes an opened file's unnamed $DATA, growing the
 * allocation (cluster allocation + run-list extension) when a write or a
 * set-end-of-file goes past the current allocated size. Built on the write
 * primitives (ntfsio.c), the allocator (ntfsalloc.c) and the shared record
 * surgery (NtfsRewriteDataRuns / NtfsSetDataSize in ntfssup.c), all of which are
 * validated against ntfs-3g in the user-mode write tests.
 *
 * Writes are write-through: data goes straight to disk and any cached pages for
 * the written range are purged, so the cached read path (NT 5) stays coherent
 * without the lazy-writer / dirty-page machinery.
 */

#define NTFS_KERNEL
#include "ntfs.h"

static PVOID
NtfsWriteUserBuffer(PIRP Irp)
{
    if (Irp->MdlAddress) return MmGetSystemAddressForMdl(Irp->MdlAddress);
    return Irp->UserBuffer;
}

/*
 * Convert a resident-$DATA file to non-resident so it can grow past the MFT
 * record: allocate clusters for NewSize, rewrite the attribute as non-resident,
 * copy the old resident bytes into the new clusters, and update the FCB. EOF is
 * preserved (the caller's write extends DataSize afterwards).
 */
static NTSTATUS
NtfsConvertResident(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONGLONG NewSize)
{
    ULONG Cs = Vcb->Geo.ClusterSize;
    ULONGLONG clusters = (NewSize + Cs - 1) / Cs;
    ULONGLONG lcn, oldEof = (ULONGLONG)Fcb->Header.FileSize.QuadPart;
    PUCHAR rec = NULL;
    PNTFS_RUN nr = NULL;
    NTSTATUS Status;

    if (clusters == 0) clusters = 1;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    nr = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!rec || !nr) { Status = STATUS_INSUFFICIENT_RESOURCES; goto done; }

    Status = NtfsAllocClusters(Vcb, (ULONG)clusters, &lcn);
    if (Status == STATUS_DISK_FULL && clusters > 1) {
        /* no single free run that long: start with one cluster, then grow
         * piece by piece through NtfsExtendAllocation */
        clusters = 1;
        Status = NtfsAllocClusters(Vcb, 1, &lcn);
    }
    if (!NT_SUCCESS(Status)) goto done;

    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(Status)) { NtfsFreeClusters(Vcb, lcn, (ULONG)clusters); goto done; }

    if (!NtfsConvertDataToNonResident((PMFT_RECORD)rec, Vcb->Geo.MftRecordSize,
                                      lcn, clusters, clusters * Cs, oldEof)) {
        NtfsFreeClusters(Vcb, lcn, (ULONG)clusters);
        Status = STATUS_DISK_FULL; goto done;       /* record too full to convert */
    }
    Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(Status)) { NtfsFreeClusters(Vcb, lcn, (ULONG)clusters); goto done; }

    /* set up the FCB run list, then copy the old resident bytes into place */
    nr->Vcn = 0; nr->Lcn = lcn; nr->Length = clusters; nr->Sparse = FALSE;
    Fcb->DataResident = FALSE;
    Fcb->DataRuns = nr; nr = NULL;
    Fcb->DataRunCount = 1;
    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)(clusters * Cs);
    if (oldEof && Fcb->ResidentData) {
        NtfsWriteAttributeData(Vcb, Fcb->DataRuns, Fcb->DataRunCount, 0,
                               (ULONG)oldEof, Fcb->ResidentData);
    }
    if (Fcb->ResidentData) { ExFreePool(Fcb->ResidentData); Fcb->ResidentData = NULL; }
    Fcb->ResidentLength = 0;
    Status = STATUS_SUCCESS;

done:
    if (rec) ExFreePool(rec);
    if (nr) ExFreePool(nr);
    return Status;
}

/*
 * Grow the unnamed non-resident $DATA allocation of Fcb to cover NewSize bytes:
 * allocate the extra clusters, extend the run list, rewrite the attribute, and
 * reload the FCB's cached runs. DataSize/EOF is left unchanged (the caller sets
 * it after the data is written).
 */
static NTSTATUS
NtfsExtendAllocation(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONGLONG NewSize)
{
    ULONG Cs = Vcb->Geo.ClusterSize;
    ULONGLONG needClusters = (NewSize + Cs - 1) / Cs;
    ULONGLONG curClusters = 0;
    ULONG addClu, left, rc, cap, firstNew, i;
    ULONGLONG lcn, newAlloc, lastEnd = 0;
    PUCHAR rec = NULL;
    PMFT_RECORD m;
    PNTFS_RUN runs = NULL;
    BOOLEAN hasList, mergedFirst = FALSE;
    ULONG mergedLen = 0;
    NTSTATUS Status;

    if (Fcb->DataResident || Fcb->DataRuns == NULL || Fcb->DataRunCount == 0)
        return STATUS_NOT_IMPLEMENTED;
    rc = Fcb->DataRunCount;
    curClusters = Fcb->DataRuns[rc - 1].Vcn + Fcb->DataRuns[rc - 1].Length;
    if (needClusters <= curClusters) return STATUS_SUCCESS;
    addClu = (ULONG)(needClusters - curClusters);

    /* the run list (any length) grows by at most one run per allocated piece */
    cap = rc + 64;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, cap * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (rec == NULL || runs == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    RtlCopyMemory(runs, Fcb->DataRuns, rc * sizeof(NTFS_RUN));
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(Status)) goto done;
    m = (PMFT_RECORD)rec;
    hasList = (NtfsFindAttribute(m, ATTR_ATTRIBUTE_LIST, NULL, 0) != NULL);

    /* allocate the extra clusters, contiguous with the last run when possible,
     * otherwise piece by piece (a fragmented volume still has the space) */
    firstNew = rc;
    if (!runs[rc - 1].Sparse) lastEnd = runs[rc - 1].Lcn + runs[rc - 1].Length;
    for (left = addClu; left; ) {
        ULONG got;
        ULONGLONG hint = runs[rc - 1].Sparse ? 0 : runs[rc - 1].Lcn + runs[rc - 1].Length;
        if (rc >= cap) { Status = STATUS_DISK_FULL; goto undo; }
        Status = NtfsAllocClustersPartial(Vcb, left, hint, &lcn, &got);
        if (!NT_SUCCESS(Status) || got == 0) { if (NT_SUCCESS(Status)) Status = STATUS_DISK_FULL; goto undo; }
        if (!runs[rc - 1].Sparse && runs[rc - 1].Lcn + runs[rc - 1].Length == lcn) {
            if (rc - 1 < firstNew) { mergedFirst = TRUE; mergedLen += got; }
            runs[rc - 1].Length += got;
        } else {
            runs[rc].Vcn = runs[rc - 1].Vcn + runs[rc - 1].Length;
            runs[rc].Lcn = lcn; runs[rc].Length = got; runs[rc].Sparse = FALSE;
            rc++;
        }
        left -= got;
    }
    newAlloc = (curClusters + addClu) * Cs;

    /* store it: in the base record while the run list fits there, else as
     * extents in extension records with an $ATTRIBUTE_LIST */
    if (!hasList &&
        NtfsRewriteDataRuns(m, Vcb->Geo.MftRecordSize, runs, rc, newAlloc,
                            (ULONGLONG)Fcb->Header.FileSize.QuadPart)) {
        Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
        if (!NT_SUCCESS(Status)) goto undo;
        {
            PNTFS_RUN nr = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, rc * sizeof(NTFS_RUN), NTFS_TAG_RUN);
            if (nr) { RtlCopyMemory(nr, runs, rc * sizeof(NTFS_RUN));
                      ExFreePool(Fcb->DataRuns); Fcb->DataRuns = nr; Fcb->DataRunCount = rc; }
        }
    } else {
        Status = NtfsWriteDataExtents(Vcb, Fcb, runs, rc, newAlloc,
                                      (ULONGLONG)Fcb->Header.FileSize.QuadPart,
                                      (ULONGLONG)Fcb->Header.ValidDataLength.QuadPart);
        if (!NT_SUCCESS(Status)) goto undo;
    }
    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)newAlloc;
    Status = STATUS_SUCCESS;
    goto done;
undo:
    /* give back what this call allocated */
    if (mergedFirst) NtfsFreeClusters(Vcb, lastEnd, mergedLen);
    for (i = firstNew; i < rc; i++) NtfsFreeClusters(Vcb, runs[i].Lcn, (ULONG)runs[i].Length);
done:
    if (rec) ExFreePool(rec);
    if (runs) ExFreePool(runs);
    return Status;
}

/* Patch the EOF (DataSize) of a non-resident file into its MFT record. */
static NTSTATUS
NtfsPersistDataSize(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONGLONG NewSize)
{
    PUCHAR rec;
    NTSTATUS Status;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (NT_SUCCESS(Status)) {
        if (NtfsSetDataSize((PMFT_RECORD)rec, NewSize))
            Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
        else    /* $DATA lives in extension records ($ATTRIBUTE_LIST) */
            Status = NtfsSetListDataSize(Vcb, Fcb, NewSize);
    }
    ExFreePool(rec);
    return Status;
}

/* Set a resident file's value length to NewLen (shrink or grow within the MFT
 * record) and refresh the FCB's cached copy and sizes. STATUS_BUFFER_OVERFLOW
 * means it no longer fits the record (the caller converts to non-resident). */
static NTSTATUS
NtfsSetResidentSize(PNTFS_VCB Vcb, PNTFS_FCB Fcb, PUCHAR rec, ULONG NewLen)
{
    PMFT_RECORD m = (PMFT_RECORD)rec;
    PATTRIBUTE_RECORD a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    PUCHAR nb = NULL;

    if (!a || a->NonResident) return STATUS_NOT_IMPLEMENTED;
    if (!NtfsResizeResidentValue(m, Vcb->Geo.MftRecordSize, a, NewLen))
        return STATUS_BUFFER_OVERFLOW;
    if (NewLen) {
        nb = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, NewLen, NTFS_TAG_BUF);
        if (nb == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(nb, NtfsResidentValue(a), NewLen);
    }
    if (Fcb->ResidentData) ExFreePool(Fcb->ResidentData);
    Fcb->ResidentData = nb;
    Fcb->ResidentLength = NewLen;
    Fcb->Header.FileSize.QuadPart = NewLen;
    Fcb->Header.ValidDataLength.QuadPart = NewLen;
    Fcb->Header.AllocationSize.QuadPart =
        ((LONGLONG)NewLen + Vcb->Geo.ClusterSize - 1) & ~((LONGLONG)Vcb->Geo.ClusterSize - 1);
    return STATUS_SUCCESS;
}

/* Write a resident file's $DATA in place, growing the value inside the MFT
 * record when it still fits. STATUS_BUFFER_OVERFLOW = must go non-resident. */
NTSTATUS
NtfsWriteResident(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONG Offset, ULONG Length, PVOID Buffer)
{
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    NTSTATUS Status;
    ULONG End = Offset + Length;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (NT_SUCCESS(Status)) {
        m = (PMFT_RECORD)rec;
        a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
        if (!a || a->NonResident) {
            Status = STATUS_NOT_IMPLEMENTED;
        } else {
            if (End > NtfsResidentValueLength(a))
                Status = NtfsSetResidentSize(Vcb, Fcb, rec, End);   /* grow in record */
            if (NT_SUCCESS(Status)) {
                a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
                RtlCopyMemory((PUCHAR)NtfsResidentValue(a) + Offset, Buffer, Length);
                Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
                if (NT_SUCCESS(Status) && Fcb->ResidentData && End <= Fcb->ResidentLength)
                    RtlCopyMemory(Fcb->ResidentData + Offset, Buffer, Length);
            }
        }
    }
    ExFreePool(rec);
    return Status;
}

/*
 * Truncate a file to zero length (FILE_OVERWRITE / OVERWRITE_IF / SUPERSEDE on
 * an existing file). Resident: shrink the value to 0. Non-resident: EOF and valid
 * data go to 0; the clusters stay allocated and are reused by later writes.
 */
NTSTATUS
NtfsTruncateFile(PNTFS_VCB Vcb, PNTFS_FCB Fcb)
{
    PUCHAR rec;
    NTSTATUS Status;

    if (Fcb->FcbState & FCB_STATE_DIRECTORY) return STATUS_FILE_IS_A_DIRECTORY;
    if (Fcb->DataCompressed) {
        Status = NtfsDecompressFile(Vcb, Fcb);     /* truncate makes it plain */
        if (!NT_SUCCESS(Status)) return Status;
    }
    Fcb->FcbState |= FCB_STATE_MODIFIED;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (NT_SUCCESS(Status)) {
        if (Fcb->DataResident) {
            Status = NtfsSetResidentSize(Vcb, Fcb, rec, 0);
        } else {
            BOOLEAN inBase = NtfsSetDataSize((PMFT_RECORD)rec, 0);
            Status = inBase ? NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec)
                            : NtfsSetListDataSize(Vcb, Fcb, 0);
            if (NT_SUCCESS(Status)) {
                Fcb->Header.FileSize.QuadPart = 0;
                Fcb->Header.ValidDataLength.QuadPart = 0;
            }
        }
        if (NT_SUCCESS(Status) && Fcb->DataResident) Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
    }
    if (NT_SUCCESS(Status) && (Fcb->SectionObjectPointers.SharedCacheMap != NULL ||
                               Fcb->SectionObjectPointers.DataSectionObject != NULL))
        CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
    ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
    KeLeaveCriticalRegion();
    ExFreePool(rec);
    return Status;
}

/* ===================================================================== *
 *  Create / delete (MFT record alloc + directory $I30 index maintenance)   *
 *  Uses the verified shared engine: NtfsBuildFileRecord, NtfsIndexInsert/  *
 *  Remove, NtfsAllocMftRecord/NtfsFreeMftRecord. Large ($INDX) indexes     *
 *  only (a small resident-root directory returns STATUS_NOT_IMPLEMENTED).  *
 * ===================================================================== */

static const WCHAR NtfsI30[4] = { '$','I','3','0' };

/* Index-allocation VCNs (subnode pointers, INDX IndexBlockVcn) count clusters
 * when an index block is at least a cluster, else 512-byte units. */
#define NTFS_IDX_VCN_BYTES(Vcb) (((Vcb)->Geo.IndexBlockSize >= (Vcb)->Geo.ClusterSize) ? \
                                 (Vcb)->Geo.ClusterSize : 512u)
#define NTFS_IDX_VCN_PER_BLOCK(Vcb) ((Vcb)->Geo.IndexBlockSize / NTFS_IDX_VCN_BYTES(Vcb))

static NTSTATUS
NtfsReadIndx(PNTFS_VCB Vcb, PNTFS_RUN IRuns, ULONG IC, ULONGLONG Vcn, PUCHAR Buf)
{
    ULONG ibs = Vcb->Geo.IndexBlockSize;
    NTSTATUS st = NtfsReadAttributeData(Vcb, IRuns, IC, Vcn * NTFS_IDX_VCN_BYTES(Vcb), ibs, Buf);
    if (!NT_SUCCESS(st)) return st;
    if (((PNTFS_RECORD_HEADER)Buf)->Type != NTFS_INDX_SIGNATURE) return STATUS_FILE_CORRUPT_ERROR;
    if (!NtfsApplyFixup(Buf, ibs, Vcb->Geo.BytesPerSector)) return STATUS_FILE_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS
NtfsWriteIndx(PNTFS_VCB Vcb, PNTFS_RUN IRuns, ULONG IC, ULONGLONG Vcn, PUCHAR Buf)
{
    ULONG ibs = Vcb->Geo.IndexBlockSize;
    NTSTATUS st;
    NtfsSetFixup(Buf, ibs, Vcb->Geo.BytesPerSector);
    st = NtfsWriteAttributeData(Vcb, IRuns, IC, Vcn * NTFS_IDX_VCN_BYTES(Vcb), ibs, Buf);
    NtfsApplyFixup(Buf, ibs, Vcb->Geo.BytesPerSector);   /* restore caller's bytes */
    return st;
}

/* ---- directory-index operations (insert / remove / update) -------------- */
static VOID RemoveAttr(PMFT_RECORD m, PATTRIBUTE_RECORD a);

/* Build an index entry (INDEX_ENTRY + FILE_NAME key) from a FILE_NAME value;
 * entbuf must hold 600 bytes. Returns the entry length. */
static ULONG
NtfsMakeIndexEntry(PUCHAR entbuf, ULONGLONG fileRef, const FILE_NAME_ATTR *fn)
{
    ULONG keyLen = sizeof(FILE_NAME_ATTR) + fn->NameLength * 2, entLen;
    PINDEX_ENTRY ie = (PINDEX_ENTRY)entbuf;
    RtlZeroMemory(entbuf, 600);
    RtlCopyMemory(entbuf + sizeof(INDEX_ENTRY), fn, keyLen);
    ie->FileReference = fileRef; ie->KeyLength = (USHORT)keyLen; ie->Flags = 0;
    entLen = (sizeof(INDEX_ENTRY) + keyLen + 7) & ~7u;
    ie->Length = (USHORT)entLen;
    return entLen;
}

typedef struct _NTFS_IDXWORK {
    PUCHAR root, indx, aux;
    PNTFS_RUN iruns;
} NTFS_IDXWORK;

static BOOLEAN
IdxWorkAlloc(PNTFS_VCB Vcb, NTFS_IDXWORK *w)
{
    w->root  = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    w->indx  = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.IndexBlockSize, NTFS_TAG_BUF);
    w->aux   = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 1200, NTFS_TAG_BUF);
    w->iruns = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    return w->root && w->indx && w->aux && w->iruns;
}

static VOID
IdxWorkFree(NTFS_IDXWORK *w)
{
    if (w->root) ExFreePool(w->root);
    if (w->indx) ExFreePool(w->indx);
    if (w->aux) ExFreePool(w->aux);
    if (w->iruns) ExFreePool(w->iruns);
}

static PINDEX_ROOT
IdxRoot(PMFT_RECORD m)
{
    PATTRIBUTE_RECORD ir = NtfsFindAttribute(m, ATTR_INDEX_ROOT, NtfsI30, 4);
    return (ir && !ir->NonResident) ? (PINDEX_ROOT)NtfsResidentValue(ir) : NULL;
}

/* next attribute of type Type after `after` (NULL = first) in record m */
static PATTRIBUTE_RECORD
NextAttrOfType(PMFT_RECORD m, PATTRIBUTE_RECORD after, ULONG Type)
{
    PUCHAR p = (PUCHAR)m;
    ULONG off = after ? (ULONG)((PUCHAR)after - p) + after->Length : m->FirstAttributeOffset;
    while (off + 8 <= m->BytesInUse) {
        PATTRIBUTE_RECORD a = (PATTRIBUTE_RECORD)(p + off);
        if (a->Type == ATTR_END || a->Length == 0) break;
        if (a->Type == Type) return a;
        off += a->Length;
    }
    return NULL;
}

static VOID
RemoveAttr(PMFT_RECORD m, PATTRIBUTE_RECORD a)
{
    PUCHAR p = (PUCHAR)m, at = (PUCHAR)a;
    ULONG len = a->Length, end = (ULONG)(at - p) + len;
    memmove(at, at + len, m->BytesInUse - end);
    m->BytesInUse -= len;
}

/* ================================================================== */
/* Transparent LZNT1 compression: whole-file compress / decompress.     */
/* These rewrite a file's $DATA layout atomically under a journal        */
/* transaction. Bounded by NTFS_COMPRESS_MAX so the whole-file buffer     */
/* stays within non-paged pool (the uncommon write-to-compressed path).   */
/* ================================================================== */

#define NTFS_COMPRESS_MAX   (8u * 1024u * 1024u)   /* 8 MB whole-file RMW cap */
#define NTFS_CU_CLUSTERS    16u                    /* standard compression unit */
#define NTFS_CU_SHIFT       4u                     /* log2(16) -> CompressionUnit field */

/* Allocate `clusters` clusters into the run array (up to cap runs); returns the
 * run count, or 0 on failure (and frees whatever it got). Non-sparse runs only. */
static ULONG
AllocPlainRuns(PNTFS_VCB Vcb, ULONGLONG clusters, PNTFS_RUN runs, ULONG cap, ULONGLONG startVcn)
{
    ULONG rc = 0; ULONGLONG left = clusters, vcn = startVcn, hint = 0;
    while (left) {
        ULONG got; ULONGLONG lcn;
        ULONG want = (left > 0xFFFFFFFFUL) ? 0xFFFFFFF0UL : (ULONG)left;
        if (rc >= cap) goto fail;
        if (!NT_SUCCESS(NtfsAllocClustersPartial(Vcb, want, hint, &lcn, &got)) || got == 0) goto fail;
        if (rc > 0 && !runs[rc-1].Sparse && runs[rc-1].Lcn + runs[rc-1].Length == lcn) {
            runs[rc-1].Length += got;
        } else {
            runs[rc].Vcn = vcn; runs[rc].Lcn = lcn; runs[rc].Length = got; runs[rc].Sparse = FALSE; rc++;
        }
        vcn += got; left -= got; hint = lcn + got;
    }
    return rc;
fail:
    { ULONG i; for (i = 0; i < rc; i++) if (!runs[i].Sparse) NtfsFreeClusters(Vcb, runs[i].Lcn, (ULONG)runs[i].Length); }
    return 0;
}

/* Set/clear the FILE_ATTRIBUTE_COMPRESSED (0x800) display bit in $STD_INFO and
 * every $FILE_NAME of the record `m` already in memory (caller writes it). */
static VOID
SetCompressDisplayBit(PMFT_RECORD m, BOOLEAN On)
{
    PATTRIBUTE_RECORD a = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
    if (a && !a->NonResident) {
        PSTANDARD_INFORMATION si = (PSTANDARD_INFORMATION)NtfsResidentValue(a);
        if (On) si->FileAttributes |= 0x800u; else si->FileAttributes &= ~0x800u;
    }
    for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) {
        PFILE_NAME_ATTR fn;
        if (a->NonResident) continue;
        fn = (PFILE_NAME_ATTR)NtfsResidentValue(a);
        if (On) fn->FileAttributes |= 0x800u; else fn->FileAttributes &= ~0x800u;
    }
}

/* Decompress a compressed file into a plain non-resident $DATA. Safe: produces
 * an ordinary file using the proven plain-run layout. Caller holds no locks. */
NTSTATUS
NtfsDecompressFile(PNTFS_VCB Vcb, PNTFS_FCB Fcb)
{
    ULONG Cs = Vcb->Geo.ClusterSize;
    ULONGLONG dataSize, allocBytes;
    ULONG needClu, cap, rc = 0, i, oldRc = 0;
    PUCHAR buf = NULL, rec = NULL;
    PNTFS_RUN runs = NULL, oldRuns = NULL;
    PMFT_RECORD m;
    NTSTATUS Status;
    BOOLEAN locked = FALSE, extents = FALSE;

    if (!Fcb->DataCompressed || Fcb->DataResident) return STATUS_SUCCESS;
    dataSize = (ULONGLONG)Fcb->Header.FileSize.QuadPart;
    if (dataSize > NTFS_COMPRESS_MAX) return STATUS_NOT_SUPPORTED;   /* too big to RMW here */
    needClu = (ULONG)((dataSize + Cs - 1) / Cs);
    allocBytes = (ULONGLONG)needClu * Cs;

    buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, (ULONG)(allocBytes ? allocBytes : Cs), NTFS_TAG_BUF);
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    cap = Fcb->DataRunCount + needClu + 16;
    runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, cap * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!buf || !rec || !runs) { Status = STATUS_INSUFFICIENT_RESOURCES; goto out; }

    if (needClu) RtlZeroMemory(buf, (ULONG)allocBytes);
    if (dataSize) {
        Status = NtfsReadCompressed(Vcb, Fcb->DataRuns, Fcb->DataRunCount, Fcb->CuClusters,
                                    0, (ULONG)dataSize, buf);
        if (!NT_SUCCESS(Status)) goto out;
    }

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    locked = TRUE;

    /* snapshot the old (compressed) runs so we can free their clusters */
    oldRc = Fcb->DataRunCount;
    oldRuns = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, (oldRc ? oldRc : 1) * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!oldRuns) { Status = STATUS_INSUFFICIENT_RESOURCES; goto unlock; }
    RtlCopyMemory(oldRuns, Fcb->DataRuns, oldRc * sizeof(NTFS_RUN));

    if (needClu) {
        rc = AllocPlainRuns(Vcb, needClu, runs, cap, 0);
        if (rc == 0) { Status = STATUS_DISK_FULL; goto unlock; }
        Status = NtfsWriteAttributeData(Vcb, runs, rc, 0, (ULONG)allocBytes, buf);
        if (!NT_SUCCESS(Status)) goto freenew;
    }

    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(Status)) goto freenew;
    m = (PMFT_RECORD)rec;

    if (needClu == 0) {
        /* empty file: a zero-length plain non-resident $DATA (single sparse-free run not
         * possible) - fall back to a tiny resident $DATA is overkill; keep one real cluster */
        rc = AllocPlainRuns(Vcb, 1, runs, cap, 0);
        if (rc == 0) { Status = STATUS_DISK_FULL; goto freenew; }
        allocBytes = Cs;
    }

    if (NtfsFindAttribute(m, ATTR_ATTRIBUTE_LIST, NULL, 0) == NULL &&
        NtfsSetBaseDataRuns(m, Vcb->Geo.MftRecordSize, runs, rc, allocBytes, dataSize, 0, 0)) {
        SetCompressDisplayBit(m, FALSE);
        Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
        if (!NT_SUCCESS(Status)) goto freenew;
    } else {
        /* run list too big for the base record: use the extent writer (plain) */
        Status = NtfsWriteDataExtents(Vcb, Fcb, runs, rc, allocBytes, dataSize, dataSize);
        if (!NT_SUCCESS(Status)) goto freenew;
        extents = TRUE;
        /* clear the display bit in the (possibly rewritten) base record */
        if (NT_SUCCESS(NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec))) {
            SetCompressDisplayBit((PMFT_RECORD)rec, FALSE);
            NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
        }
    }

    /* success: free the old compressed clusters and adopt the new layout */
    for (i = 0; i < oldRc; i++)
        if (!oldRuns[i].Sparse && oldRuns[i].Length)
            NtfsFreeClusters(Vcb, oldRuns[i].Lcn, (ULONG)oldRuns[i].Length);
    if (!extents) {
        PNTFS_RUN nr = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, rc * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (nr) { RtlCopyMemory(nr, runs, rc * sizeof(NTFS_RUN));
                  if (Fcb->DataRuns) ExFreePool(Fcb->DataRuns); Fcb->DataRuns = nr; Fcb->DataRunCount = rc; }
    }
    Fcb->DataCompressed = FALSE;
    Fcb->CuClusters = 0;
    Fcb->FileAttributes &= ~0x800u;
    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)allocBytes;
    Status = STATUS_SUCCESS;
    goto unlock;

freenew:
    for (i = 0; i < rc; i++) if (!runs[i].Sparse && runs[i].Length) NtfsFreeClusters(Vcb, runs[i].Lcn, (ULONG)runs[i].Length);
unlock:
    if (locked) NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(Status)) NtfsJnlCommit(Vcb); else NtfsJnlAbort(Vcb);
out:
    if (buf) ExFreePool(buf);
    if (rec) ExFreePool(rec);
    if (runs) ExFreePool(runs);
    if (oldRuns) ExFreePool(oldRuns);
    return Status;
}

/* Compress a plain non-resident file into an LZNT1-compressed $DATA. Builds the
 * compressed layout (per unit: compressed clusters + a sparse hole, or 16 real
 * clusters when a unit does not shrink) and rewrites the base-record $DATA with
 * CompressionUnit=4. If the file cannot be represented in the base record, or is
 * resident/empty/too large, it is left plain (success - NTFS also stores some
 * files uncompressed). Caller holds no locks. */
NTSTATUS
NtfsCompressFile(PNTFS_VCB Vcb, PNTFS_FCB Fcb)
{
    ULONG Cs = Vcb->Geo.ClusterSize;
    ULONG CuClu = NTFS_CU_CLUSTERS, unitBytes = NTFS_CU_CLUSTERS * Cs;
    ULONGLONG dataSize, u, numUnits, totalReal = 0;
    ULONG needClu, cap, rc = 0, i, oldRc = 0;
    PUCHAR buf = NULL, comp = NULL, body = NULL, rec = NULL;
    PNTFS_RUN runs = NULL, oldRuns = NULL;
    PMFT_RECORD m;
    NTSTATUS Status;
    BOOLEAN locked = FALSE;

    if (Fcb->DataCompressed || Fcb->DataResident) return STATUS_SUCCESS;
    if (Fcb->FcbState & FCB_STATE_DIRECTORY) return STATUS_SUCCESS;
    dataSize = (ULONGLONG)Fcb->Header.FileSize.QuadPart;
    if (dataSize == 0 || dataSize > NTFS_COMPRESS_MAX) return STATUS_SUCCESS;  /* leave plain */
    needClu = (ULONG)((dataSize + Cs - 1) / Cs);
    numUnits = (dataSize + unitBytes - 1) / unitBytes;

    buf  = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, (ULONG)(numUnits * unitBytes), NTFS_TAG_BUF);
    comp = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, unitBytes + 4096, NTFS_TAG_BUF);
    body = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 4096, NTFS_TAG_BUF);
    rec  = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    cap  = (ULONG)(numUnits * 2 + 8);
    runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, cap * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!buf || !comp || !body || !rec || !runs) { Status = STATUS_INSUFFICIENT_RESOURCES; goto out; }

    RtlZeroMemory(buf, (ULONG)(numUnits * unitBytes));
    Status = NtfsReadAttributeData(Vcb, Fcb->DataRuns, Fcb->DataRunCount, 0, (ULONG)((dataSize + Cs - 1) & ~((ULONGLONG)Cs - 1)), buf);
    if (!NT_SUCCESS(Status)) goto out;

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    locked = TRUE;

    oldRc = Fcb->DataRunCount;
    oldRuns = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, (oldRc ? oldRc : 1) * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!oldRuns) { Status = STATUS_INSUFFICIENT_RESOURCES; goto unlock; }
    RtlCopyMemory(oldRuns, Fcb->DataRuns, oldRc * sizeof(NTFS_RUN));

    /* lay out each compression unit */
    for (u = 0; u < numUnits; u++) {
        PUCHAR unitPtr = buf + (ULONG)(u * unitBytes);
        ULONG op = NtfsLznt1CompressUnit(unitPtr, unitBytes, comp, unitBytes, body);
        ULONG compClu = (op == 0xFFFFFFFFUL) ? CuClu : ((op + Cs - 1) / Cs);
        ULONGLONG vcn = u * CuClu;
        if (compClu < CuClu) {
            /* compressed: compClu real clusters hold the LZNT1 stream, rest sparse */
            ULONG got = AllocPlainRuns(Vcb, compClu, runs + rc, cap - rc, vcn);
            if (got == 0) { Status = STATUS_DISK_FULL; goto freenew; }
            rc += got;
            RtlZeroMemory(comp + op, compClu * Cs - op);
            Status = NtfsWriteAttributeData(Vcb, runs, rc, vcn * Cs, compClu * Cs, comp);
            if (!NT_SUCCESS(Status)) goto freenew;
            /* sparse hole to pad the unit to CuClu VCNs */
            if (rc >= cap) { Status = STATUS_INSUFFICIENT_RESOURCES; goto freenew; }
            runs[rc].Vcn = vcn + compClu; runs[rc].Lcn = 0;
            runs[rc].Length = CuClu - compClu; runs[rc].Sparse = TRUE; rc++;
            totalReal += compClu;
        } else {
            /* no gain: 16 real clusters holding the raw unit bytes */
            ULONG got = AllocPlainRuns(Vcb, CuClu, runs + rc, cap - rc, vcn);
            if (got == 0) { Status = STATUS_DISK_FULL; goto freenew; }
            rc += got;
            Status = NtfsWriteAttributeData(Vcb, runs, rc, vcn * Cs, unitBytes, unitPtr);
            if (!NT_SUCCESS(Status)) goto freenew;
            totalReal += CuClu;
        }
    }

    /* no space saved at all: keep the file plain (free the new clusters) */
    if (totalReal >= (ULONGLONG)numUnits * CuClu) { Status = STATUS_SUCCESS; goto freenew; }

    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(Status)) goto freenew;
    m = (PMFT_RECORD)rec;
    if (NtfsFindAttribute(m, ATTR_ATTRIBUTE_LIST, NULL, 0) != NULL ||
        !NtfsSetBaseDataRuns(m, Vcb->Geo.MftRecordSize, runs, rc,
                             (numUnits * CuClu) * Cs, dataSize, NTFS_CU_SHIFT, totalReal * Cs)) {
        /* mapping won't fit the base record: leave the file plain */
        Status = STATUS_SUCCESS; goto freenew;
    }
    SetCompressDisplayBit(m, TRUE);
    Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(Status)) goto freenew;

    /* success: free old plain clusters, adopt compressed layout */
    for (i = 0; i < oldRc; i++)
        if (!oldRuns[i].Sparse && oldRuns[i].Length)
            NtfsFreeClusters(Vcb, oldRuns[i].Lcn, (ULONG)oldRuns[i].Length);
    {
        PNTFS_RUN nr = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, rc * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (nr) { RtlCopyMemory(nr, runs, rc * sizeof(NTFS_RUN));
                  if (Fcb->DataRuns) ExFreePool(Fcb->DataRuns); Fcb->DataRuns = nr; Fcb->DataRunCount = rc; }
    }
    Fcb->DataCompressed = TRUE;
    Fcb->CuClusters = CuClu;
    Fcb->FileAttributes |= 0x800u;
    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)((numUnits * CuClu) * Cs);
    Status = STATUS_SUCCESS;
    goto unlock;

freenew:
    for (i = 0; i < rc; i++) if (!runs[i].Sparse && runs[i].Length) NtfsFreeClusters(Vcb, runs[i].Lcn, (ULONG)runs[i].Length);
unlock:
    if (locked) NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(Status)) NtfsJnlCommit(Vcb); else NtfsJnlAbort(Vcb);
out:
    if (buf) ExFreePool(buf);
    if (comp) ExFreePool(comp);
    if (body) ExFreePool(body);
    if (rec) ExFreePool(rec);
    if (runs) ExFreePool(runs);
    if (oldRuns) ExFreePool(oldRuns);
    return Status;
}

/* ---- DOS 8.3 short names ------------------------------------------------- */

static WCHAR
ShortChar(WCHAR c)
{
    if (c == ' ' || c == '.') return 0;                     /* dropped */
    if (c >= 'a' && c <= 'z') return (WCHAR)(c - 0x20);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '(': case ')':
    case '-': case '@': case '^': case '_': case '`': case '{': case '}': case '~':
        return c;
    }
    return '_';                                             /* anything else */
}

/* Generate a unique 8.3 alias for a long name in directory `dir`
 * ("LONGNA~1.TXT" style). Returns its length (0 = none found). */
static ULONG
NtfsMakeShortName(PNTFS_VCB Vcb, ULONGLONG dir, const WCHAR *ln, ULONG n, PWCHAR out)
{
    WCHAR base[6], ext[3];
    ULONG bl = 0, el = 0, i, k, end;
    LONG dot = -1;
    for (i = 0; i < n; i++) if (ln[i] == '.') dot = (LONG)i;
    if (dot == 0) dot = -1;
    end = (dot >= 0) ? (ULONG)dot : n;
    for (i = 0; i < end && bl < 6; i++) { WCHAR c = ShortChar(ln[i]); if (c) base[bl++] = c; }
    if (dot >= 0)
        for (i = (ULONG)dot + 1; i < n && el < 3; i++) { WCHAR c = ShortChar(ln[i]); if (c) ext[el++] = c; }
    if (bl == 0) base[bl++] = '_';
    /* like NT: NAME~1..NAME~4 first; after that the base becomes two letters
     * plus a 4-digit hash of the long name, so a directory full of similar
     * names does not need thousands of probes per new file */
    for (k = 1; k < 1000000; k++) {
        WCHAR num[8], b2[6]; ULONG nd = 0, t, keep, o = 0, b2l = bl, j;
        const WCHAR *bp = base;
        if (k > 4) {
            ULONG h = 0;
            static const char hx[] = "0123456789ABCDEF";
            for (j = 0; j < n; j++) h = h * 37 + ln[j];
            h += (k - 5) / 9 * 7919;                       /* next hash per 9 tries */
            b2[0] = base[0]; b2[1] = (bl > 1) ? base[1] : '_';
            for (j = 0; j < 4; j++) b2[2 + j] = (WCHAR)hx[(h >> (12 - 4 * j)) & 15];
            bp = b2; b2l = 6;
            t = (k - 5) % 9 + 1;
        } else {
            t = k;
        }
        do { num[nd++] = (WCHAR)('0' + t % 10); t /= 10; } while (t);
        keep = 8 - (nd + 1); if (keep > b2l) keep = b2l;
        for (i = 0; i < keep; i++) out[o++] = bp[i];
        out[o++] = '~';
        while (nd) out[o++] = num[--nd];
        if (el) { out[o++] = '.'; for (i = 0; i < el; i++) out[o++] = ext[i]; }
        if (NtfsLookupInDir(Vcb, dir, out, o) == (ULONGLONG)-1) return o;
    }
    return 0;
}

/* If the record's (single) Win32 name is not a legal 8.3 name, add a DOS
 * $FILE_NAME alias next to it, as NT does with 8.3 generation enabled. */
static VOID
NtfsAddDosName(PNTFS_VCB Vcb, PMFT_RECORD m, ULONGLONG dir)
{
    PATTRIBUTE_RECORD a;
    PFILE_NAME_ATTR fn = NULL;
    WCHAR sn[13];
    UCHAR ab[0x18 + sizeof(FILE_NAME_ATTR) + 24 + 8];
    ULONG sl, vlen, alen, i;

    for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) {
        PFILE_NAME_ATTR f = (PFILE_NAME_ATTR)NtfsResidentValue(a);
        if (f->NameType == NTFS_FNAME_DOS) return;          /* already has one */
        if (fn == NULL) fn = f;
    }
    if (fn == NULL || fn->NameType != NTFS_FNAME_WIN32) return;
    sl = NtfsMakeShortName(Vcb, dir, (PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)),
                           fn->NameLength, sn);
    if (sl == 0) return;
    vlen = sizeof(FILE_NAME_ATTR) + sl * 2;
    alen = (0x18 + vlen + 7) & ~7u;
    RtlZeroMemory(ab, sizeof(ab));
    a = (PATTRIBUTE_RECORD)ab;
    a->Type = ATTR_FILE_NAME; a->Length = alen; a->NameOffset = 0x18;
    a->Form.Resident.ValueLength = vlen; a->Form.Resident.ValueOffset = 0x18;
    a->Form.Resident.IndexedFlag = 1;
    RtlCopyMemory(ab + 0x18, fn, sizeof(FILE_NAME_ATTR));
    ((PFILE_NAME_ATTR)(ab + 0x18))->NameLength = (UCHAR)sl;
    ((PFILE_NAME_ATTR)(ab + 0x18))->NameType = NTFS_FNAME_DOS;
    for (i = 0; i < sl; i++) ((PWCHAR)(ab + 0x18 + sizeof(FILE_NAME_ATTR)))[i] = sn[i];
    if (NtfsInsertAttributeEx(m, Vcb->Geo.MftRecordSize, ab, alen, TRUE))
        m->HardLinkCount = 2;
}

/* Link every $FILE_NAME of record m (fileRef) into directory `dir`; on a
 * failure the names already linked are unlinked again. */
static NTSTATUS
NtfsLinkAllNames(PNTFS_VCB Vcb, PMFT_RECORD m, ULONGLONG fileRef, ULONGLONG dir, PUCHAR entbuf)
{
    PATTRIBUTE_RECORD a, b;
    NTSTATUS Status = STATUS_SUCCESS;
    for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) {
        PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)NtfsResidentValue(a);
        ULONG len = NtfsMakeIndexEntry(entbuf, fileRef, fn);
        Status = NtfsIndexAdd(Vcb, dir, entbuf, len,
                              (PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)), fn->NameLength);
        if (!NT_SUCCESS(Status)) {
            for (b = NextAttrOfType(m, NULL, ATTR_FILE_NAME); b && b != a; b = NextAttrOfType(m, b, ATTR_FILE_NAME)) {
                PFILE_NAME_ATTR f = (PFILE_NAME_ATTR)NtfsResidentValue(b);
                NtfsIndexDelete(Vcb, dir, (PWCHAR)((PUCHAR)f + sizeof(FILE_NAME_ATTR)), f->NameLength);
            }
            break;
        }
    }
    return Status;
}

/* ---- create / delete / rename / info update ------------------------------ */

/*
 * Create `wname` in directory parentRec: a file with resident $DATA = data, or
 * (IsDir) an empty directory. The new record number is returned in *newRecOut.
 */
NTSTATUS
NtfsCreateInDir(PNTFS_VCB Vcb, ULONGLONG parentRec, USHORT parentSeq,
                const WCHAR *wname, ULONG wn, const void *data, ULONG dlen,
                BOOLEAN IsDir, ULONGLONG *newRecOut)
{
    PUCHAR frec = NULL, entbuf = NULL;
    ULONGLONG newRec = 0, parentRef, fileRef, ft;
    LARGE_INTEGER t;
    USHORT seq = 1;
    ULONG entLen, i;
    PATTRIBUTE_RECORD fna;
    NTSTATUS Status;

    if (wn == 0 || wn > 255) return STATUS_OBJECT_NAME_INVALID;
    for (i = 0; i < wn; i++)
        if (wname[i] < 0x20 || wname[i] == '"' || wname[i] == '*' || wname[i] == '/' ||
            wname[i] == ':' || wname[i] == '<' || wname[i] == '>' || wname[i] == '?' ||
            wname[i] == '|' || wname[i] == '\\')
            return STATUS_OBJECT_NAME_INVALID;
    frec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    entbuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 600, NTFS_TAG_BUF);
    if (!frec || !entbuf) { Status = STATUS_INSUFFICIENT_RESOURCES; goto done; }

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    Status = NtfsAllocMftRecord(Vcb, &newRec);
    if (!NT_SUCCESS(Status)) goto unlock;
    /* keep the sequence number a previous delete bumped (stale refs stay stale) */
    if (NT_SUCCESS(NtfsReadMftRecord(Vcb, newRec, frec)) &&
        ((PMFT_RECORD)frec)->SequenceNumber != 0)
        seq = ((PMFT_RECORD)frec)->SequenceNumber;

    KeQuerySystemTime(&t); ft = (ULONGLONG)t.QuadPart;
    parentRef = (parentRec & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)parentSeq << 48);
    if (IsDir)
        NtfsBuildDirRecord(frec, Vcb->Geo.MftRecordSize, Vcb->Geo.BytesPerSector,
                           newRec, seq, parentRef, wname, wn, ft,
                           Vcb->Geo.IndexBlockSize, Vcb->Geo.ClusterSize);
    else
        NtfsBuildFileRecord(frec, Vcb->Geo.MftRecordSize, Vcb->Geo.BytesPerSector,
                            newRec, seq, parentRef, wname, wn, ft, data, dlen);
    /* NTFS 1.x (NT 3.1 - 4.0 volumes): the 1.x record header and the short
     * 0x30-byte $STANDARD_INFORMATION */
    if (Vcb->NtfsMajor < 3) {
        NtfsRecordToV1(frec, Vcb->Geo.MftRecordSize);
        PATTRIBUTE_RECORD nsi = NtfsFindAttribute((PMFT_RECORD)frec, ATTR_STANDARD_INFORMATION, NULL, 0);
        if (nsi) NtfsResizeResidentValue((PMFT_RECORD)frec, Vcb->Geo.MftRecordSize, nsi, 0x30);
    }
    /* security: inherit the parent's $Secure id (NTFS 3.x), or else copy the
     * parent's own $SECURITY_DESCRIPTOR attribute (1.x-style descriptors,
     * also found on upgraded volumes) */
    {
        PUCHAR pr = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
        if (pr && NT_SUCCESS(NtfsReadMftRecord(Vcb, parentRec, pr))) {
            PATTRIBUTE_RECORD psi = NtfsFindAttribute((PMFT_RECORD)pr, ATTR_STANDARD_INFORMATION, NULL, 0);
            PATTRIBUTE_RECORD nsi = NtfsFindAttribute((PMFT_RECORD)frec, ATTR_STANDARD_INFORMATION, NULL, 0);
            PATTRIBUTE_RECORD psd = NtfsFindAttribute((PMFT_RECORD)pr, ATTR_SECURITY_DESCRIPTOR, NULL, 0);
            ULONG pid = (psi && !psi->NonResident && NtfsResidentValueLength(psi) >= 0x48) ?
                        ((PSTANDARD_INFORMATION)NtfsResidentValue(psi))->SecurityId : 0;
            if (pid && nsi && NtfsResidentValueLength(nsi) >= 0x48)
                ((PSTANDARD_INFORMATION)NtfsResidentValue(nsi))->SecurityId = pid;
            else if (psd && !psd->NonResident)
                NtfsInsertAttribute((PMFT_RECORD)frec, Vcb->Geo.MftRecordSize, psd, psd->Length);
        }
        if (pr) ExFreePool(pr);
    }
    NtfsAddDosName(Vcb, (PMFT_RECORD)frec, parentRec);
    Status = NtfsWriteMftRecord(Vcb, newRec, frec);
    if (!NT_SUCCESS(Status)) { NtfsFreeMftRecord(Vcb, newRec); goto unlock; }
    fileRef = (newRec & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)seq << 48);

    Status = NtfsLinkAllNames(Vcb, (PMFT_RECORD)frec, fileRef, parentRec, entbuf);
    if (!NT_SUCCESS(Status)) {
        ((PMFT_RECORD)frec)->Flags &= ~MFT_RECORD_IN_USE;
        ((PMFT_RECORD)frec)->SequenceNumber++;
        NtfsWriteMftRecord(Vcb, newRec, frec);
        NtfsFreeMftRecord(Vcb, newRec);
        goto unlock;
    }
    if (newRecOut) *newRecOut = newRec;
    Status = STATUS_SUCCESS;
unlock:
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(Status)) NtfsJnlCommit(Vcb); else NtfsJnlAbort(Vcb);
done:
    if (frec) ExFreePool(frec);
    if (entbuf) ExFreePool(entbuf);
    return Status;
}

/* TRUE if directory record `rec` holds no entries (root and INDX blocks). */
BOOLEAN
NtfsDirIsEmpty(PNTFS_VCB Vcb, ULONGLONG rec)
{
    NTFS_IDXWORK w = { 0 };
    BOOLEAN empty = FALSE;
    PMFT_RECORD m;
    PINDEX_ROOT r;
    PINDEX_ENTRY e;
    PATTRIBUTE_RECORD ia, bm;
    ULONG ic, nblk, b, ibs = Vcb->Geo.IndexBlockSize;

    if (!IdxWorkAlloc(Vcb, &w)) goto done;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, rec, w.root))) goto done;
    m = (PMFT_RECORD)w.root;
    r = IdxRoot(m);
    if (r == NULL) goto done;
    e = NtfsFirstIndexEntry(&r->Header);
    if (!(e->Flags & INDEX_ENTRY_END)) goto done;
    if (!(r->Header.Flags & 1)) { empty = TRUE; goto done; }
    ia = NtfsFindAttribute(m, ATTR_INDEX_ALLOCATION, NtfsI30, 4);
    if (!ia || !NtfsDecodeRuns(ia, w.iruns, NTFS_MAX_IDX_RUNS, &ic) || !ic) goto done;
    bm = NtfsFindAttribute(m, ATTR_BITMAP, NtfsI30, 4);
    nblk = (ULONG)(ia->Form.NonResident.DataSize / ibs);
    for (b = 0; b < nblk; b++) {
        if (bm && !bm->NonResident && (b >> 3) < NtfsResidentValueLength(bm) &&
            !(((PUCHAR)NtfsResidentValue(bm))[b >> 3] & (1u << (b & 7))))
            continue;                                  /* block not in use */
        if (!NT_SUCCESS(NtfsReadIndx(Vcb, w.iruns, ic, (ULONGLONG)b * NTFS_IDX_VCN_PER_BLOCK(Vcb), w.indx)))
            goto done;
        e = NtfsFirstIndexEntry(&((PINDEX_BLOCK)w.indx)->Header);
        if (!(e->Flags & INDEX_ENTRY_END)) goto done;
    }
    empty = TRUE;
done:
    IdxWorkFree(&w);
    return empty;
}

/* free every non-resident attribute's clusters held in record m */
static VOID
FreeRecordClusters(PNTFS_VCB Vcb, PMFT_RECORD m)
{
    PUCHAR p = (PUCHAR)m;
    ULONG off = m->FirstAttributeOffset, i, rc;
    PNTFS_RUN runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
                        NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (runs == NULL) return;
    while (off + 8 <= m->BytesInUse) {
        PATTRIBUTE_RECORD a = (PATTRIBUTE_RECORD)(p + off);
        if (a->Type == ATTR_END || a->Length == 0) break;
        if (a->NonResident && NtfsDecodeRuns(a, runs, NTFS_MAX_IDX_RUNS, &rc)) {
            for (i = 0; i < rc; i++)
                if (!runs[i].Sparse && runs[i].Length)
                    NtfsFreeClusters(Vcb, runs[i].Lcn, (ULONG)runs[i].Length);
        }
        off += a->Length;
    }
    ExFreePool(runs);
}

/*
 * Delete the file or (empty) directory in Fcb: remove every $FILE_NAME from its
 * parent index, free all its clusters (unnamed/named $DATA, index allocation,
 * extension records listed in an $ATTRIBUTE_LIST), clear IN_USE (bump the
 * sequence number) and free the MFT record.
 */
NTSTATUS
NtfsDeleteFile(PNTFS_VCB Vcb, PNTFS_FCB Fcb)
{
    PUCHAR frec = NULL, xrec = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD fna, al;
    NTSTATUS Status;

    if (Fcb->FcbState & FCB_STATE_ROOT) return STATUS_CANNOT_DELETE;
    frec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    xrec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!frec || !xrec) { Status = STATUS_INSUFFICIENT_RESOURCES; goto done; }

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, frec);
    if (!NT_SUCCESS(Status)) goto unlock;
    m = (PMFT_RECORD)frec;
    if ((m->Flags & MFT_RECORD_IS_DIRECTORY) && !NtfsDirIsEmpty(Vcb, Fcb->MftRecord)) {
        Status = STATUS_DIRECTORY_NOT_EMPTY; goto unlock;
    }

    /* 1. unlink every name from its parent directory */
    for (fna = NextAttrOfType(m, NULL, ATTR_FILE_NAME); fna; fna = NextAttrOfType(m, fna, ATTR_FILE_NAME)) {
        PFILE_NAME_ATTR fn;
        if (fna->NonResident) continue;
        fn = (PFILE_NAME_ATTR)NtfsResidentValue(fna);
        Status = NtfsIndexDelete(Vcb, NTFS_MREF_RECORD(fn->ParentDirectory),
                                 (PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)), fn->NameLength);
        if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) goto unlock;
    }

    /* 2. free the clusters (base record, then any extension records) */
    FreeRecordClusters(Vcb, m);
    al = NtfsFindAttribute(m, ATTR_ATTRIBUTE_LIST, NULL, 0);
    if (al && !al->NonResident) {
        PUCHAR lp = (PUCHAR)NtfsResidentValue(al), le = lp + NtfsResidentValueLength(al);
        ULONGLONG lastX = (ULONGLONG)-1;
        while (lp + sizeof(ATTRIBUTE_LIST_ENTRY) <= le) {
            PATTRIBUTE_LIST_ENTRY x = (PATTRIBUTE_LIST_ENTRY)lp;
            ULONGLONG xr = NTFS_MREF_RECORD(x->AttributeReference);
            if (x->Length == 0) break;
            if (xr != Fcb->MftRecord && xr != lastX &&
                NT_SUCCESS(NtfsReadMftRecord(Vcb, xr, xrec))) {
                PMFT_RECORD xm = (PMFT_RECORD)xrec;
                if (xm->Flags & MFT_RECORD_IN_USE) {
                    FreeRecordClusters(Vcb, xm);
                    xm->Flags &= ~MFT_RECORD_IN_USE; xm->SequenceNumber++;
                    NtfsWriteMftRecord(Vcb, xr, xrec);
                    NtfsFreeMftRecord(Vcb, xr);
                }
                lastX = xr;
            }
            lp += x->Length;
        }
    }

    /* 3. release the base record */
    m->Flags &= ~MFT_RECORD_IN_USE;
    m->SequenceNumber++;
    NtfsWriteMftRecord(Vcb, Fcb->MftRecord, frec);
    Status = NtfsFreeMftRecord(Vcb, Fcb->MftRecord);
unlock:
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(Status)) NtfsJnlCommit(Vcb); else NtfsJnlAbort(Vcb);
done:
    if (frec) ExFreePool(frec);
    if (xrec) ExFreePool(xrec);
    return Status;
}

/*
 * Write the FCB's times/attributes to $STANDARD_INFORMATION and refresh the
 * duplicated information (times, sizes, attributes) in every $FILE_NAME and in
 * the matching parent index entries - what a directory listing shows.
 */
NTSTATUS
NtfsUpdateFileInfo(PNTFS_VCB Vcb, PNTFS_FCB Fcb)
{
    PUCHAR frec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    ULONG siAttr = 0;
    BOOLEAN isDir = (Fcb->FcbState & FCB_STATE_DIRECTORY) != 0;
    NTSTATUS Status;

    frec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (frec == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, frec);
    if (!NT_SUCCESS(Status)) goto out;
    m = (PMFT_RECORD)frec;
    a = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
    if (a && !a->NonResident) {
        PSTANDARD_INFORMATION si = (PSTANDARD_INFORMATION)NtfsResidentValue(a);
        si->CreationTime = Fcb->CreationTime;
        si->ChangeTime = Fcb->LastWriteTime;
        si->MftChangeTime = Fcb->ChangeTime;
        si->LastAccessTime = Fcb->LastAccessTime;
        /* NORMAL (0x80) is never stored: no attribute bits means 0 */
        si->FileAttributes = (si->FileAttributes & ~0xA7u) | (Fcb->FileAttributes & 0x27u);
        siAttr = si->FileAttributes;
    }
    for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) {
        PFILE_NAME_ATTR fn;
        if (a->NonResident) continue;
        fn = (PFILE_NAME_ATTR)NtfsResidentValue(a);
        fn->CreationTime = Fcb->CreationTime;
        fn->ChangeTime = Fcb->LastWriteTime;
        fn->MftChangeTime = Fcb->ChangeTime;
        fn->LastAccessTime = Fcb->LastAccessTime;
        if (isDir) {
            fn->AllocatedSize = fn->DataSize = 0;
            fn->FileAttributes = siAttr | NTFS_FILE_NAME_INDEX_PRESENT;
        } else {
            fn->DataSize = (ULONGLONG)Fcb->Header.FileSize.QuadPart;
            fn->AllocatedSize = Fcb->DataResident ? ((fn->DataSize + 7) & ~7ULL)
                                                  : (ULONGLONG)Fcb->Header.AllocationSize.QuadPart;
            fn->FileAttributes = siAttr | (Fcb->FileAttributes & 0x800u);
        }
    }
    Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, frec);
    if (!NT_SUCCESS(Status)) goto out;
    for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME))
        if (!a->NonResident) NtfsIndexUpdate(Vcb, (PFILE_NAME_ATTR)NtfsResidentValue(a));
out:
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(Status)) NtfsJnlCommit(Vcb); else NtfsJnlAbort(Vcb);
    ExFreePool(frec);
    return Status;
}

/*
 * Rename/move Fcb to `nn` (nl chars) in directory tparent ((ULONGLONG)-1 =
 * the current parent). An existing different target is replaced only when
 * Replace is set and it is a closed, non-directory file.
 */
/*
 * Create a hard link: add a new $FILE_NAME (Win32, + a DOS alias if the name is
 * not 8.3) for `name` in directory `tparent` on the file's existing MFT record,
 * bump its link count, and insert the directory index entry. The file keeps one
 * record; only names multiply. Directories cannot be hard-linked.
 */
NTSTATUS
NtfsCreateHardLink(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONGLONG tparent,
                   const WCHAR *name, ULONG nl, BOOLEAN replace)
{
    PUCHAR rec = NULL, old = NULL, entbuf = NULL, ab = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD fna;
    PFILE_NAME_ATTR src, nfn;
    ULONGLONG parentRef, fileRef;
    USHORT pseq = 5;
    ULONG vlen, alen, i, entLen, before;
    NTSTATUS Status;

    if (nl == 0 || nl > 255) return STATUS_OBJECT_NAME_INVALID;
    for (i = 0; i < nl; i++)
        if (name[i] < 0x20 || name[i] == '"' || name[i] == '*' || name[i] == '/' ||
            name[i] == ':' || name[i] == '<' || name[i] == '>' || name[i] == '?' ||
            name[i] == '|' || name[i] == '\\')
            return STATUS_OBJECT_NAME_INVALID;
    if (Fcb->FcbState & FCB_STATE_DIRECTORY) return STATUS_FILE_IS_A_DIRECTORY;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    old = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    entbuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 600, NTFS_TAG_BUF);
    ab = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 0x18 + sizeof(FILE_NAME_ATTR) + 512, NTFS_TAG_BUF);
    if (!rec || !old || !entbuf || !ab) { Status = STATUS_INSUFFICIENT_RESOURCES; goto out; }

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);

    if (NtfsLookupInDir(Vcb, tparent, name, nl) != (ULONGLONG)-1) {
        Status = replace ? STATUS_NOT_IMPLEMENTED : STATUS_OBJECT_NAME_COLLISION;
        goto unlock;
    }
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(Status)) goto unlock;
    m = (PMFT_RECORD)rec;
    if (m->Flags & MFT_RECORD_IS_DIRECTORY) { Status = STATUS_FILE_IS_A_DIRECTORY; goto unlock; }
    RtlCopyMemory(old, rec, Vcb->Geo.MftRecordSize);

    fna = NtfsFindAttribute(m, ATTR_FILE_NAME, NULL, 0);
    if (!fna) { Status = STATUS_UNSUCCESSFUL; goto unlock; }
    src = (PFILE_NAME_ATTR)NtfsResidentValue(fna);

    /* parent sequence for the file reference in the key */
    { PUCHAR pr = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
      if (pr && NT_SUCCESS(NtfsReadMftRecord(Vcb, tparent, pr))) pseq = ((PMFT_RECORD)pr)->SequenceNumber;
      if (pr) ExFreePool(pr); }
    parentRef = (tparent & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)pseq << 48);
    fileRef = (Fcb->MftRecord & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)m->SequenceNumber << 48);

    /* build the new Win32 $FILE_NAME from the file's existing one (same times
     * and sizes), with the new parent and name */
    vlen = sizeof(FILE_NAME_ATTR) + nl * 2;
    alen = (0x18 + vlen + 7) & ~7u;
    RtlZeroMemory(ab, alen);
    { PATTRIBUTE_RECORD a = (PATTRIBUTE_RECORD)ab;
      a->Type = ATTR_FILE_NAME; a->Length = alen; a->NameOffset = 0x18;
      a->Form.Resident.ValueLength = vlen; a->Form.Resident.ValueOffset = 0x18;
      a->Form.Resident.IndexedFlag = 1; }
    nfn = (PFILE_NAME_ATTR)(ab + 0x18);
    RtlCopyMemory(nfn, src, sizeof(FILE_NAME_ATTR));
    nfn->ParentDirectory = parentRef;
    nfn->NameLength = (UCHAR)nl;
    nfn->NameType = NTFS_FNAME_WIN32;
    for (i = 0; i < nl; i++) ((PWCHAR)(ab + 0x18 + sizeof(FILE_NAME_ATTR)))[i] = name[i];

    before = 0;
    { PATTRIBUTE_RECORD a; for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) before++; }
    if (!NtfsInsertAttributeEx(m, Vcb->Geo.MftRecordSize, ab, alen, FALSE)) {
        Status = STATUS_INSUFFICIENT_RESOURCES; goto unlock;
    }
    m->HardLinkCount = (USHORT)(before + 1);
    /* a DOS alias if the name is not a legal 8.3 (adds a second name) */
    NtfsAddDosName(Vcb, m, tparent);
    { USHORT cnt = 0; PATTRIBUTE_RECORD a;
      for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) cnt++;
      m->HardLinkCount = cnt; }

    NtfsJnlRecord(Vcb, Fcb->MftRecord, old, rec);
    Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(Status)) goto unlock;

    /* insert index entries for every name that now points to tparent and is
     * not already linked there (the new Win32 name and its DOS alias) */
    { PATTRIBUTE_RECORD a;
      for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) {
        PFILE_NAME_ATTR f = (PFILE_NAME_ATTR)NtfsResidentValue(a);
        if ((f->ParentDirectory & 0xFFFFFFFFFFFFULL) != tparent) continue;
        if (NtfsLookupInDir(Vcb, tparent, (PWCHAR)((PUCHAR)f + sizeof(FILE_NAME_ATTR)), f->NameLength) != (ULONGLONG)-1)
            continue;
        entLen = NtfsMakeIndexEntry(entbuf, fileRef, f);
        NtfsIndexAdd(Vcb, tparent, entbuf, entLen,
                     (PWCHAR)((PUCHAR)f + sizeof(FILE_NAME_ATTR)), f->NameLength);
      }
    }
    Status = STATUS_SUCCESS;

unlock:
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(Status)) NtfsJnlCommit(Vcb); else NtfsJnlAbort(Vcb);
out:
    if (rec) ExFreePool(rec);
    if (old) ExFreePool(old);
    if (entbuf) ExFreePool(entbuf);
    if (ab) ExFreePool(ab);
    return Status;
}

NTSTATUS
NtfsRenameFile(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONGLONG tparent,
               const WCHAR *nn, ULONG nl, BOOLEAN Replace)
{
    PUCHAR frec = NULL, prec = NULL, entbuf = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a, keep = NULL;
    PFILE_NAME_ATTR fn;
    ULONGLONG existing, now;
    LARGE_INTEGER t;
    USHORT pseq;
    ULONG i, entLen;
    NTSTATUS Status;

    if (nl == 0 || nl > 255) return STATUS_OBJECT_NAME_INVALID;
    for (i = 0; i < nl; i++)
        if (nn[i] < 0x20 || nn[i] == '"' || nn[i] == '*' || nn[i] == '/' || nn[i] == ':' ||
            nn[i] == '<' || nn[i] == '>' || nn[i] == '?' || nn[i] == '|' || nn[i] == '\\')
            return STATUS_OBJECT_NAME_INVALID;
    if (Fcb->FcbState & FCB_STATE_ROOT) return STATUS_INVALID_PARAMETER;
    frec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    prec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    entbuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 600, NTFS_TAG_BUF);
    if (!frec || !prec || !entbuf) { Status = STATUS_INSUFFICIENT_RESOURCES; goto done; }

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, frec);
    if (!NT_SUCCESS(Status)) goto unlock;
    m = (PMFT_RECORD)frec;
    /* the name to keep: the first non-DOS-only $FILE_NAME */
    for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) {
        if (a->NonResident) continue;
        if (((PFILE_NAME_ATTR)NtfsResidentValue(a))->NameType != NTFS_FNAME_DOS) { keep = a; break; }
        if (keep == NULL) keep = a;
    }
    if (keep == NULL) { Status = STATUS_FILE_CORRUPT_ERROR; goto unlock; }
    fn = (PFILE_NAME_ATTR)NtfsResidentValue(keep);
    if (tparent == (ULONGLONG)-1) tparent = NTFS_MREF_RECORD(fn->ParentDirectory);

    /* a directory may not move into its own subtree */
    if (Fcb->FcbState & FCB_STATE_DIRECTORY) {
        ULONGLONG walk = tparent; ULONG depth = 0;
        while (walk != MFT_RN_ROOT && depth++ < 256) {
            PATTRIBUTE_RECORD pa;
            if (walk == Fcb->MftRecord) { Status = STATUS_INVALID_PARAMETER; goto unlock; }
            if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, walk, prec))) break;
            pa = NtfsFindAttribute((PMFT_RECORD)prec, ATTR_FILE_NAME, NULL, 0);
            if (!pa || pa->NonResident) break;
            walk = NTFS_MREF_RECORD(((PFILE_NAME_ATTR)NtfsResidentValue(pa))->ParentDirectory);
        }
    }

    /* target name already present? */
    existing = NtfsLookupInDir(Vcb, tparent, nn, nl);
    if (existing != (ULONGLONG)-1 && existing != Fcb->MftRecord) {
        PLIST_ENTRY le;
        NTFS_FCB tmp;
        if (!Replace) { Status = STATUS_OBJECT_NAME_COLLISION; goto unlock; }
        for (le = Vcb->FcbList.Flink; le != &Vcb->FcbList; le = le->Flink) {
            PNTFS_FCB f = CONTAINING_RECORD(le, NTFS_FCB, FcbLinks);
            if (f->MftRecord == existing && f->UncleanCount) { Status = STATUS_ACCESS_DENIED; goto unlock; }
        }
        if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, existing, prec))) { Status = STATUS_FILE_CORRUPT_ERROR; goto unlock; }
        if (((PMFT_RECORD)prec)->Flags & MFT_RECORD_IS_DIRECTORY) { Status = STATUS_ACCESS_DENIED; goto unlock; }
        RtlZeroMemory(&tmp, sizeof(tmp));
        tmp.MftRecord = existing;
        Status = NtfsDeleteFile(Vcb, &tmp);
        if (!NT_SUCCESS(Status)) goto unlock;
        for (le = Vcb->FcbList.Flink; le != &Vcb->FcbList; le = le->Flink) {
            PNTFS_FCB f = CONTAINING_RECORD(le, NTFS_FCB, FcbLinks);
            if (f->MftRecord == existing) f->MftRecord = NTFS_DEAD_RECORD;
        }
    }

    /* target directory sequence number, for the new parent reference */
    Status = NtfsReadMftRecord(Vcb, tparent, prec);
    if (!NT_SUCCESS(Status)) goto unlock;
    if (!(((PMFT_RECORD)prec)->Flags & MFT_RECORD_IS_DIRECTORY)) { Status = STATUS_NOT_A_DIRECTORY; goto unlock; }
    pseq = ((PMFT_RECORD)prec)->SequenceNumber;

    /* 1. unlink every current name */
    for (a = NextAttrOfType(m, NULL, ATTR_FILE_NAME); a; a = NextAttrOfType(m, a, ATTR_FILE_NAME)) {
        PFILE_NAME_ATTR f;
        if (a->NonResident) continue;
        f = (PFILE_NAME_ATTR)NtfsResidentValue(a);
        Status = NtfsIndexDelete(Vcb, NTFS_MREF_RECORD(f->ParentDirectory),
                                 (PWCHAR)((PUCHAR)f + sizeof(FILE_NAME_ATTR)), f->NameLength);
        if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) goto unlock;
    }
    /* 2. keep one $FILE_NAME, drop the others (e.g. a DOS 8.3 alias) */
    {
        ULONG keepOff = (ULONG)((PUCHAR)keep - (PUCHAR)m);
        a = NextAttrOfType(m, NULL, ATTR_FILE_NAME);
        while (a) {
            ULONG aOff = (ULONG)((PUCHAR)a - (PUCHAR)m);
            if (aOff != keepOff && !a->NonResident) {
                ULONG len = a->Length;
                RemoveAttr(m, a);
                if (aOff < keepOff) keepOff -= len;
                a = NextAttrOfType(m, NULL, ATTR_FILE_NAME);    /* rescan */
                continue;
            }
            a = NextAttrOfType(m, a, ATTR_FILE_NAME);
        }
        keep = (PATTRIBUTE_RECORD)((PUCHAR)m + keepOff);
    }
    /* 3. rewrite the kept name: new parent, new name */
    if (!NtfsResizeResidentValue(m, Vcb->Geo.MftRecordSize, keep, sizeof(FILE_NAME_ATTR) + nl * 2)) {
        Status = STATUS_DISK_FULL; goto unlock;
    }
    fn = (PFILE_NAME_ATTR)NtfsResidentValue(keep);
    KeQuerySystemTime(&t); now = (ULONGLONG)t.QuadPart;
    fn->ParentDirectory = (tparent & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)pseq << 48);
    fn->MftChangeTime = now;
    fn->NameLength = (UCHAR)nl;
    fn->NameType = NtfsIsDos83Name(nn, nl) ? NTFS_FNAME_WIN32DOS : NTFS_FNAME_WIN32;
    for (i = 0; i < nl; i++) ((PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)))[i] = nn[i];
    m->HardLinkCount = 1;
    NtfsAddDosName(Vcb, m, tparent);          /* 8.3 alias for a long new name */
    a = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
    if (a && !a->NonResident) ((PSTANDARD_INFORMATION)NtfsResidentValue(a))->MftChangeTime = now;
    Fcb->ChangeTime = now;
    if (!NtfsRefreshAttrList(Vcb, m, Fcb->MftRecord)) { Status = STATUS_DISK_FULL; goto unlock; }
    Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, frec);
    if (!NT_SUCCESS(Status)) goto unlock;

    /* 4. link under the new name (and its 8.3 alias) */
    Status = NtfsLinkAllNames(Vcb, m,
                 (Fcb->MftRecord & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)m->SequenceNumber << 48),
                 tparent, entbuf);
    if (NT_SUCCESS(Status)) {
        ULONG nb = nl; if (nb > 255) nb = 255;
        RtlCopyMemory(Fcb->FileName, nn, nb * sizeof(WCHAR));
        Fcb->FileNameLength = (USHORT)(nb * sizeof(WCHAR));
    }
unlock:
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(Status)) NtfsJnlCommit(Vcb); else NtfsJnlAbort(Vcb);
done:
    if (frec) ExFreePool(frec);
    if (prec) ExFreePool(prec);
    if (entbuf) ExFreePool(entbuf);
    return Status;
}

/*
 * Write a named $DATA stream (ADS): in place, extending within the stream's
 * allocation and growing the named attribute (cluster alloc + run rewrite) when
 * needed. Resident named streams are written in place; a resident named stream
 * that must grow past its record value returns STATUS_NOT_IMPLEMENTED.
 */
static NTSTATUS
NtfsWriteNamedStream(PNTFS_IRP_CONTEXT Ctx, PNTFS_FCB Fcb, PNTFS_CCB Ccb)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PNTFS_VCB Vcb = Ctx->Vcb;
    ULONGLONG Offset = (ULONGLONG)IrpSp->Parameters.Write.ByteOffset.QuadPart;
    ULONG Length = IrpSp->Parameters.Write.Length;
    ULONGLONG End;
    ULONG Cs = Vcb->Geo.ClusterSize;
    PVOID Buffer = NtfsWriteUserBuffer(Ctx->Irp);
    PUCHAR rec = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    NTSTATUS Status;
    ULONG i;

    if (Buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (IrpSp->Parameters.Write.ByteOffset.HighPart == -1 &&
        IrpSp->Parameters.Write.ByteOffset.LowPart == FILE_WRITE_TO_END_OF_FILE)
        Offset = Ccb->StreamSize;
    End = Offset + Length;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; goto rel; }

    if (Ccb->StreamResident) {
        Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
        if (!NT_SUCCESS(Status)) goto rel;
        m = (PMFT_RECORD)rec;
        a = NtfsFindAttribute(m, ATTR_DATA, Ccb->StreamName, Ccb->StreamNameLen);
        if (!a || a->NonResident) { Status = STATUS_OBJECT_NAME_NOT_FOUND; goto rel; }
        if (End > NtfsResidentValueLength(a)) { Status = STATUS_NOT_IMPLEMENTED; goto rel; }
        RtlCopyMemory((PUCHAR)NtfsResidentValue(a) + Offset, Buffer, Length);
        Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
        if (NT_SUCCESS(Status) && Ccb->StreamResidentData && End <= Ccb->StreamResidentLength)
            RtlCopyMemory(Ccb->StreamResidentData + Offset, Buffer, Length);
        if (NT_SUCCESS(Status)) Ctx->Irp->IoStatus.Information = Length;
        goto rel;
    }

    /* non-resident named stream: extend allocation if needed */
    {
        ULONGLONG have = 0;
        for (i = 0; i < Ccb->StreamRunCount; i++) have += Ccb->StreamRuns[i].Length;
        if (End > have * Cs) {
            ULONG addClu = (ULONG)((End + Cs - 1) / Cs - have);
            ULONGLONG lcn;
            PNTFS_RUN nr;
            Status = NtfsAllocClusters(Vcb, addClu, &lcn);
            if (!NT_SUCCESS(Status)) goto rel;
            nr = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
                     (Ccb->StreamRunCount + 1) * sizeof(NTFS_RUN), NTFS_TAG_RUN);
            if (!nr) { NtfsFreeClusters(Vcb, lcn, addClu); Status = STATUS_INSUFFICIENT_RESOURCES; goto rel; }
            RtlCopyMemory(nr, Ccb->StreamRuns, Ccb->StreamRunCount * sizeof(NTFS_RUN));
            if (!nr[Ccb->StreamRunCount - 1].Sparse &&
                nr[Ccb->StreamRunCount - 1].Lcn + nr[Ccb->StreamRunCount - 1].Length == lcn) {
                nr[Ccb->StreamRunCount - 1].Length += addClu;
            } else {
                nr[Ccb->StreamRunCount].Vcn = nr[Ccb->StreamRunCount - 1].Vcn + nr[Ccb->StreamRunCount - 1].Length;
                nr[Ccb->StreamRunCount].Lcn = lcn;
                nr[Ccb->StreamRunCount].Length = addClu;
                nr[Ccb->StreamRunCount].Sparse = FALSE;
                Ccb->StreamRunCount++;
            }
            Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
            if (!NT_SUCCESS(Status)) { ExFreePool(nr); goto rel; }
            m = (PMFT_RECORD)rec;
            a = NtfsFindAttribute(m, ATTR_DATA, Ccb->StreamName, Ccb->StreamNameLen);
            if (!a || !NtfsRewriteAttrRuns(m, Vcb->Geo.MftRecordSize, a, nr, Ccb->StreamRunCount,
                                           (have + addClu) * Cs, Ccb->StreamSize)) {
                ExFreePool(nr); Status = STATUS_DISK_FULL; goto rel;
            }
            Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
            if (!NT_SUCCESS(Status)) { ExFreePool(nr); goto rel; }
            if (Ccb->StreamRuns) ExFreePool(Ccb->StreamRuns);
            Ccb->StreamRuns = nr;
        }
    }

    Status = NtfsWriteAttributeData(Vcb, Ccb->StreamRuns, Ccb->StreamRunCount, Offset, Length, Buffer);
    if (!NT_SUCCESS(Status)) goto rel;

    if (End > Ccb->StreamSize) {
        Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
        if (NT_SUCCESS(Status)) {
            m = (PMFT_RECORD)rec;
            a = NtfsFindAttribute(m, ATTR_DATA, Ccb->StreamName, Ccb->StreamNameLen);
            if (a && a->NonResident) {
                a->Form.NonResident.DataSize = End;
                a->Form.NonResident.InitializedSize = End;
                Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
            }
        }
        if (!NT_SUCCESS(Status)) goto rel;
        Ccb->StreamSize = End;
    }
    Ctx->Irp->IoStatus.Information = Length;
    Status = STATUS_SUCCESS;

rel:
    if (rec) ExFreePool(rec);
    ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
    KeLeaveCriticalRegion();
    return Status;
}

NTSTATUS
NtfsCommonWrite(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNTFS_FCB Fcb = (PNTFS_FCB)FileObject->FsContext;
    PNTFS_CCB Ccb = (PNTFS_CCB)FileObject->FsContext2;
    PNTFS_VCB Vcb = Ctx->Vcb;
    ULONGLONG Offset = (ULONGLONG)IrpSp->Parameters.Write.ByteOffset.QuadPart;
    ULONG Length = IrpSp->Parameters.Write.Length;
    ULONGLONG End;
    PVOID Buffer;
    NTSTATUS Status;

    Ctx->Irp->IoStatus.Information = 0;

    if (Fcb == NULL) return STATUS_INVALID_DEVICE_REQUEST;
    if (!NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;
    if (Fcb->FcbState & FCB_STATE_DIRECTORY) return STATUS_INVALID_DEVICE_REQUEST;
    if (Length == 0) return STATUS_SUCCESS;
    /* Writing into a compressed file: transparently decompress it to a plain
     * file first (it becomes uncompressed), then write normally. Paging writes
     * happen under Mm locks where the whole-file RMW is unsafe, so those still
     * refuse - a compressed file is never the paging file and mmap writes to
     * one are vanishingly rare. */
    if (Fcb->DataCompressed) {
        if (Ctx->Irp->Flags & IRP_PAGING_IO) return STATUS_ACCESS_DENIED;
        Status = NtfsDecompressFile(Vcb, Fcb);
        if (!NT_SUCCESS(Status)) return Status;
    }
    /* EFS-encrypted file: this driver has no EFS key handling, so writing raw
     * bytes would replace ciphertext with plaintext and corrupt the file. We
     * preserve encrypted files read-only (reads return the stored ciphertext). */
    if (Fcb->FileAttributes & NTFS_FILE_ENCRYPTED) return STATUS_ACCESS_DENIED;
    /* a byte range another handle has locked */
    if (!(Ctx->Irp->Flags & IRP_PAGING_IO) && !FsRtlCheckLockForWriteAccess(&Fcb->FileLock, Ctx->Irp))
        return STATUS_FILE_LOCK_CONFLICT;

    /*
     * Paging I/O (the paging file, and memory-mapped flushes) must write straight
     * to the already-allocated clusters with NO metadata changes and WITHOUT the
     * main FCB resource - the memory manager may hold locks that the allocation or
     * record I/O would wait on, which deadlocks (the page-file "freeze"). Clamp to
     * the allocated size and write the run list directly under PagingIoResource.
     */
    if (Ctx->Irp->Flags & IRP_PAGING_IO) {
        ULONGLONG alloc = (ULONGLONG)Fcb->Header.AllocationSize.QuadPart;
        ULONG wlen = Length;
        /* a deleted (or replaced) file: its clusters may belong to another file
         * by now, so its late dirty pages go nowhere */
        if (Fcb->MftRecord == NTFS_DEAD_RECORD) { Ctx->Irp->IoStatus.Information = Length; return STATUS_SUCCESS; }
        if (Offset >= alloc) { Ctx->Irp->IoStatus.Information = 0; return STATUS_SUCCESS; }
        if (Offset + wlen > alloc) wlen = (ULONG)(alloc - Offset);
        Buffer = NtfsWriteUserBuffer(Ctx->Irp);
        if (Buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        if (Fcb->DataResident) {           /* resident files are never paged, but be safe */
            if (Offset + wlen <= Fcb->ResidentLength)
                Status = NtfsWriteResident(Vcb, Fcb, (ULONG)Offset, wlen, Buffer);
            else Status = STATUS_SUCCESS;
        } else {
            KeEnterCriticalRegion();
            ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);
            Status = NtfsWriteAttributeData(Vcb, Fcb->DataRuns, Fcb->DataRunCount,
                                            Offset, wlen, Buffer);
            ExReleaseResourceForThreadLite(&Fcb->PagingIoResource, ExGetCurrentResourceThread());
            KeLeaveCriticalRegion();
        }
        if (NT_SUCCESS(Status)) Ctx->Irp->IoStatus.Information = wlen;
        return Status;
    }

    /* named stream (ADS) write: in place, within the stream's allocation */
    if (Ccb && Ccb->NodeTypeCode == NTFS_NTC_CCB && Ccb->NamedStream)
        return NtfsWriteNamedStream(Ctx, Fcb, Ccb);

    /* write-at-end-of-file sentinel */
    if (IrpSp->Parameters.Write.ByteOffset.HighPart == -1 &&
        IrpSp->Parameters.Write.ByteOffset.LowPart == FILE_WRITE_TO_END_OF_FILE) {
        Offset = (ULONGLONG)Fcb->Header.FileSize.QuadPart;
    }
    End = Offset + Length;

    Buffer = NtfsWriteUserBuffer(Ctx->Irp);
    if (Buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    DBGP("[WR rec="); DBGX((ULONG)Fcb->MftRecord); DBGP(" off="); DBGX((ULONG)Offset); DBGP(" len="); DBGX(Length);
    DBGP(" fl="); DBGX(Ctx->Irp->Flags); DBGP(" mdl="); DBGX(Ctx->Irp->MdlAddress); DBGP(" b0="); DBGX(*(PULONG)Buffer);
    DBGP(" fs="); DBGX(Fcb->Header.FileSize.LowPart); DBGP(" al="); DBGX(Fcb->Header.AllocationSize.LowPart); DBGP(" res="); DBGX(Fcb->DataResident); DBGP("]\n");

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    if (Fcb->DataResident) {
        if (End <= (ULONGLONG)Vcb->Geo.MftRecordSize) {
            /* small: write (and grow) the value inside the MFT record */
            Status = NtfsWriteResident(Vcb, Fcb, (ULONG)Offset, Length, Buffer);
            if (Status != STATUS_BUFFER_OVERFLOW) {
                if (NT_SUCCESS(Status)) {
                    Ctx->Irp->IoStatus.Information = Length;
                    FileObject->CurrentByteOffset.QuadPart = (LONGLONG)End;
                    goto sync_cache;
                }
                goto release;
            }
        }
        /* no longer fits the record: convert to non-resident, then fall through
         * to the non-resident write path below */
        Status = NtfsConvertResident(Vcb, Fcb, End);
        if (!NT_SUCCESS(Status)) goto release;
    }

    /* non-resident: grow the allocation if the write runs past it */
    if (End > (ULONGLONG)Fcb->Header.AllocationSize.QuadPart) {
        Status = NtfsExtendAllocation(Vcb, Fcb, End);
        if (!NT_SUCCESS(Status)) goto release;
    }

    Status = NtfsWriteAttributeData(Vcb, Fcb->DataRuns, Fcb->DataRunCount,
                                    Offset, Length, Buffer);
    if (!NT_SUCCESS(Status)) goto release;

    /* extend EOF if we wrote past it */
    if (End > (ULONGLONG)Fcb->Header.FileSize.QuadPart) {
        Status = NtfsPersistDataSize(Vcb, Fcb, End);
        if (!NT_SUCCESS(Status)) goto release;
        Fcb->Header.FileSize.QuadPart = (LONGLONG)End;
        Fcb->Header.ValidDataLength.QuadPart = (LONGLONG)End;
    }

    FileObject->CurrentByteOffset.QuadPart = (LONGLONG)End;
    Ctx->Irp->IoStatus.Information = Length;
    Status = STATUS_SUCCESS;

sync_cache:
    {   /* data changed: new LastWrite/Change time, dup info refreshed at cleanup */
        LARGE_INTEGER now; KeQuerySystemTime(&now);
        Fcb->LastWriteTime = Fcb->ChangeTime = (ULONGLONG)now.QuadPart;
        Fcb->FileAttributes |= FILE_ATTRIBUTE_ARCHIVE;
        Fcb->FileAttributes &= ~FILE_ATTRIBUTE_NORMAL;
        Fcb->FcbState |= FCB_STATE_MODIFIED;
    }
    /* keep the read cache coherent with the write-through data: tell Cc about
     * the new sizes (its section must cover the new EOF) and drop stale pages.
     * A purge fails while the cache still has the pages mapped (e.g. the
     * last-byte read cmd does before ">>"): then the new bytes go into the
     * cache as well, or a following read would see the old contents. */
    if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
        LARGE_INTEGER Off; Off.QuadPart = (LONGLONG)Offset;
        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        if (!CcPurgeCacheSection(&Fcb->SectionObjectPointers, &Off, Length, FALSE) &&
            !Fcb->DataResident) {
            NtfsEnsureCacheMap(FileObject, Fcb);
            CcCopyWrite(FileObject, &Off, Length, TRUE, Buffer);
        }
    } else if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {
        /* the cache map is gone but Mm still holds the file's pages (they
         * come back on the next cached read): drop the stale ones */
        LARGE_INTEGER Off; Off.QuadPart = (LONGLONG)Offset;
        CcPurgeCacheSection(&Fcb->SectionObjectPointers, &Off, Length, FALSE);
    }

release:
    ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
    KeLeaveCriticalRegion();
    return Status;
}

/*
 * Set-information: EOF and allocation sizing (grow/shrink of the file size).
 * Rename/delete are not yet handled (return STATUS_NOT_IMPLEMENTED).
 */
NTSTATUS
NtfsCommonSetInformation(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNTFS_FCB Fcb = (PNTFS_FCB)FileObject->FsContext;
    PNTFS_VCB Vcb = Ctx->Vcb;
    FILE_INFORMATION_CLASS Class = IrpSp->Parameters.SetFile.FileInformationClass;
    PVOID Buffer = Ctx->Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

    if (Fcb == NULL) return STATUS_INVALID_DEVICE_REQUEST;
    if (!NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;
    DBGP("[SETI rec="); DBGX((ULONG)Fcb->MftRecord); DBGP(" cls="); DBGX(Class);
    if (Class == FileEndOfFileInformation || Class == FileAllocationInformation) { DBGP(" v="); DBGX(((PFILE_END_OF_FILE_INFORMATION)Buffer)->EndOfFile.LowPart); }
    DBGP(" fs="); DBGX(Fcb->Header.FileSize.LowPart); DBGP(" al="); DBGX(Fcb->Header.AllocationSize.LowPart); DBGP("]\n");

    switch (Class) {
    case FileEndOfFileInformation:
    case FileAllocationInformation: {
        ULONGLONG NewSize = (ULONGLONG)((PFILE_END_OF_FILE_INFORMATION)Buffer)->EndOfFile.QuadPart;
        if (Fcb->FcbState & FCB_STATE_DIRECTORY) return STATUS_INVALID_DEVICE_REQUEST;
        if (Fcb->DataCompressed) {
            Status = NtfsDecompressFile(Vcb, Fcb);    /* resize makes it plain */
            if (!NT_SUCCESS(Status)) return Status;
        }

        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
        Fcb->FcbState |= FCB_STATE_MODIFIED;
        /* Resident file: resize the value inside the MFT record when it fits
         * (shrink included); otherwise convert to non-resident first - which is
         * how a freshly created pagefile.sys gets its clusters. */
        if (Fcb->DataResident) {
            BOOLEAN wantResize = (BOOLEAN)(Class == FileEndOfFileInformation ||
                                           NewSize < (ULONGLONG)Fcb->Header.FileSize.QuadPart);
            BOOLEAN stayResident = FALSE;
            if (NewSize <= (ULONGLONG)Vcb->Geo.MftRecordSize) {
                if (!wantResize) {
                    stayResident = TRUE; Status = STATUS_SUCCESS;   /* allocation within record */
                } else {
                    PUCHAR rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
                    Status = rec ? NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec) : STATUS_INSUFFICIENT_RESOURCES;
                    if (NT_SUCCESS(Status)) Status = NtfsSetResidentSize(Vcb, Fcb, rec, (ULONG)NewSize);
                    if (NT_SUCCESS(Status)) { Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec); stayResident = TRUE; }
                    if (rec) ExFreePool(rec);
                    if (Status != STATUS_BUFFER_OVERFLOW && !NT_SUCCESS(Status)) stayResident = TRUE; /* hard error */
                }
            }
            if (!stayResident) {
                Status = NtfsConvertResident(Vcb, Fcb, NewSize);
            }
            if (stayResident || !NT_SUCCESS(Status)) {
                ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
                KeLeaveCriticalRegion();
                return Status;
            }
        }
        if (NewSize > (ULONGLONG)Fcb->Header.AllocationSize.QuadPart)
            Status = NtfsExtendAllocation(Vcb, Fcb, NewSize);
        else
            Status = STATUS_SUCCESS;
        if (NT_SUCCESS(Status) && Class == FileEndOfFileInformation && !Fcb->DataResident) {
            Status = NtfsPersistDataSize(Vcb, Fcb, NewSize);
            if (NT_SUCCESS(Status)) {
                Fcb->Header.FileSize.QuadPart = (LONGLONG)NewSize;
                Fcb->Header.ValidDataLength.QuadPart = (LONGLONG)NewSize;
                if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                    CC_FILE_SIZES Sizes;
                    Sizes.AllocationSize = Fcb->Header.AllocationSize;
                    Sizes.FileSize = Fcb->Header.FileSize;
                    Sizes.ValidDataLength = Fcb->Header.ValidDataLength;
                    CcSetFileSizes(FileObject, &Sizes);
                } else if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {
                    /* no cache map, but Mm may still hold pages past the new end */
                    LARGE_INTEGER From;
                    From.QuadPart = (LONGLONG)((NewSize + PAGE_SIZE - 1) & ~(ULONGLONG)(PAGE_SIZE - 1));
                    CcPurgeCacheSection(&Fcb->SectionObjectPointers, &From, 0, FALSE);
                }
            }
        }
        ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
        KeLeaveCriticalRegion();
        /* the paging file is never closed while the system runs: publish its
         * size in the directory entry now rather than at cleanup */
        if (NT_SUCCESS(Status) && (Fcb->FcbState & FCB_STATE_PAGING_FILE)) {
            Fcb->FcbState &= ~FCB_STATE_MODIFIED;
            NtfsUpdateFileInfo(Vcb, Fcb);
        }
        return Status;
    }
    case FileDispositionInformation: {
        /* mark (or unmark) for delete; the removal happens at cleanup when the
         * last handle closes (NT semantics). */
        BOOLEAN del = ((PFILE_DISPOSITION_INFORMATION)Buffer)->DeleteFile;
        if (Fcb->FcbState & FCB_STATE_ROOT) return STATUS_CANNOT_DELETE;
        if (del && (Fcb->FileAttributes & FILE_ATTRIBUTE_READONLY)) return STATUS_CANNOT_DELETE;
        if (del && (Fcb->FcbState & FCB_STATE_DIRECTORY) && !NtfsDirIsEmpty(Vcb, Fcb->MftRecord))
            return STATUS_DIRECTORY_NOT_EMPTY;
        if (del) Fcb->FcbState |= FCB_STATE_DELETE_PENDING;
        else     Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;
        return STATUS_SUCCESS;
    }
    case FilePositionInformation:
        FileObject->CurrentByteOffset =
            ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset;
        return STATUS_SUCCESS;
    case FileBasicInformation: {
        /* times (0 = leave unchanged, -1 = stop updating: treated as unchanged)
         * and the RO/HIDDEN/SYSTEM/ARCHIVE attributes */
        PFILE_BASIC_INFORMATION bi = (PFILE_BASIC_INFORMATION)Buffer;
        LARGE_INTEGER t;
        if (bi->CreationTime.QuadPart > 0)   Fcb->CreationTime   = (ULONGLONG)bi->CreationTime.QuadPart;
        if (bi->LastAccessTime.QuadPart > 0) Fcb->LastAccessTime = (ULONGLONG)bi->LastAccessTime.QuadPart;
        if (bi->LastWriteTime.QuadPart > 0)  Fcb->LastWriteTime  = (ULONGLONG)bi->LastWriteTime.QuadPart;
        if (bi->ChangeTime.QuadPart > 0)     Fcb->ChangeTime     = (ULONGLONG)bi->ChangeTime.QuadPart;
        else { KeQuerySystemTime(&t); Fcb->ChangeTime = (ULONGLONG)t.QuadPart; }
        if (bi->FileAttributes != 0) {
            ULONG na = (Fcb->FileAttributes & ~(0x27u | FILE_ATTRIBUTE_NORMAL)) | (bi->FileAttributes & 0x27u);
            if (!(Fcb->FcbState & FCB_STATE_DIRECTORY) && !(na & ~FILE_ATTRIBUTE_DIRECTORY))
                na |= FILE_ATTRIBUTE_NORMAL;
            Fcb->FileAttributes = na;
        }
        return NtfsUpdateFileInfo(Vcb, Fcb);
    }
    case FileRenameInformation: {
        PFILE_RENAME_INFORMATION ri = (PFILE_RENAME_INFORMATION)Buffer;
        PFILE_OBJECT tfo = IrpSp->Parameters.SetFile.FileObject;
        BOOLEAN replace = IrpSp->Parameters.SetFile.ReplaceIfExists;
        ULONGLONG tparent = (ULONGLONG)-1;
        PWCHAR nn; ULONG nl, i;
        if (Fcb->FcbState & FCB_STATE_ROOT) return STATUS_INVALID_PARAMETER;
        if (tfo != NULL) {
            /* full-path rename: the I/O manager opened the target directory
             * (SL_OPEN_TARGET_DIRECTORY) and left the final name in it */
            PNTFS_FCB tf = (PNTFS_FCB)tfo->FsContext;
            if (tf == NULL || !(tf->FcbState & FCB_STATE_DIRECTORY)) return STATUS_INVALID_PARAMETER;
            if (tfo->DeviceObject != FileObject->DeviceObject && tf->Vcb != Vcb) return STATUS_NOT_SAME_DEVICE;
            tparent = tf->MftRecord;
            nn = tfo->FileName.Buffer; nl = tfo->FileName.Length / sizeof(WCHAR);
        } else {
            /* simple rename within the same directory */
            nn = ri->FileName; nl = ri->FileNameLength / sizeof(WCHAR);
            for (i = 0; i < nl; i++) if (nn[i] == L'\\') return STATUS_INVALID_PARAMETER;
        }
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
        Status = NtfsRenameFile(Vcb, Fcb, tparent, nn, nl, replace);
        ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
        KeLeaveCriticalRegion();
        return Status;
    }
    case FileLinkInformation: {
        PFILE_LINK_INFORMATION li = (PFILE_LINK_INFORMATION)Buffer;
        PFILE_OBJECT tfo = IrpSp->Parameters.SetFile.FileObject;
        BOOLEAN replace = IrpSp->Parameters.SetFile.ReplaceIfExists;
        ULONGLONG tparent;
        PWCHAR nn; ULONG nl, i;
        if (Fcb->FcbState & FCB_STATE_ROOT) return STATUS_INVALID_PARAMETER;
        if (tfo != NULL) {
            PNTFS_FCB tf = (PNTFS_FCB)tfo->FsContext;
            if (tf == NULL || !(tf->FcbState & FCB_STATE_DIRECTORY)) return STATUS_INVALID_PARAMETER;
            if (tf->Vcb != Vcb) return STATUS_NOT_SAME_DEVICE;
            tparent = tf->MftRecord;
            nn = tfo->FileName.Buffer; nl = tfo->FileName.Length / sizeof(WCHAR);
        } else {
            PNTFS_CCB tccb;
            tparent = Fcb->MftRecord;       /* no target fo: link in a dir we must find */
            /* same-directory link: the parent of the source's name */
            nn = li->FileName; nl = li->FileNameLength / sizeof(WCHAR);
            for (i = 0; i < nl; i++) if (nn[i] == L'\\') return STATUS_INVALID_PARAMETER;
            tccb = NULL; (void)tccb;
            /* resolve the source's parent directory from its $FILE_NAME */
            {
                PUCHAR r = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
                PATTRIBUTE_RECORD a;
                if (!r) return STATUS_INSUFFICIENT_RESOURCES;
                if (NT_SUCCESS(NtfsReadMftRecord(Vcb, Fcb->MftRecord, r)) &&
                    (a = NtfsFindAttribute((PMFT_RECORD)r, ATTR_FILE_NAME, NULL, 0)) != NULL)
                    tparent = ((PFILE_NAME_ATTR)NtfsResidentValue(a))->ParentDirectory & 0xFFFFFFFFFFFFULL;
                ExFreePool(r);
            }
        }
        return NtfsCreateHardLink(Vcb, Fcb, tparent, nn, nl, replace);
    }
    default:
        return STATUS_NOT_IMPLEMENTED;
    }
}
