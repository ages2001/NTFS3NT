/*
 * NTFS3NT - on-disk allocation: cluster allocation via the volume bitmap
 * ($Bitmap, record 6) and MFT-record allocation via $MFT's $BITMAP attribute.
 * First-fit, contiguous for clusters. Used by the write path (grow/create).
 */

#define NTFS_KERNEL
#include "ntfs.h"

#define NTFS_FIRST_USER_MFT  24       /* records 0-23 are reserved by NTFS */

/* Load an attribute's $DATA run list from an MFT record into a pool buffer.
 * Caller frees *Runs. Also returns the data size. */
static NTSTATUS
LoadDataRuns(PNTFS_VCB Vcb, ULONGLONG RecNo, ULONG AttrType,
             const WCHAR *Name, ULONG NameLen,
             PNTFS_RUN *Runs, PULONG RunCount, PULONGLONG DataSize)
{
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    PNTFS_RUN runs;
    ULONG rc;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    *Runs = NULL; *RunCount = 0; if (DataSize) *DataSize = 0;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, RecNo, rec))) { ExFreePool(rec); return Status; }
    m = (PMFT_RECORD)rec;
    a = NtfsFindAttribute(m, AttrType, Name, NameLen);
    if (a == NULL || !a->NonResident) { ExFreePool(rec); return Status; }
    if (DataSize) *DataSize = a->Form.NonResident.DataSize;
    runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
               NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (runs == NULL) { ExFreePool(rec); return STATUS_INSUFFICIENT_RESOURCES; }
    if (NtfsDecodeRuns(a, runs, NTFS_MAX_IDX_RUNS, &rc) && rc) {
        *Runs = runs; *RunCount = rc; Status = STATUS_SUCCESS;
    } else {
        ExFreePool(runs);
    }
    ExFreePool(rec);
    return Status;
}

/* generic bitmap allocate: find Count consecutive clear bits in a bitmap read
 * through (Runs,RunCount), set them, write the affected bytes back. Returns the
 * first bit index in *FirstBit. BitLimit caps the search (total clusters/records). */
static NTSTATUS
BitmapAllocRun(PNTFS_VCB Vcb, PNTFS_RUN Runs, ULONG RunCount, ULONGLONG BitmapBytes,
               ULONG Count, ULONGLONG StartBit, ULONGLONG BitLimit, ULONGLONG *FirstBit,
               ULONG JnlKind)
{
    PUCHAR bm;
    ULONG bmLen;
    ULONGLONG i, run, found = (ULONGLONG)-1;
    NTSTATUS Status;

    if (Count == 0) return STATUS_INVALID_PARAMETER;
    bmLen = (ULONG)BitmapBytes;
    if (bmLen == 0 || bmLen > 0x800000) return STATUS_INSUFFICIENT_RESOURCES; /* 8MB cap */
    bm = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, bmLen, NTFS_TAG_BUF);
    if (bm == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsReadAttributeData(Vcb, Runs, RunCount, 0, bmLen, bm);
    if (!NT_SUCCESS(Status)) { ExFreePool(bm); return Status; }

    /* first-fit scan for Count consecutive clear bits */
    run = 0;
    for (i = StartBit; i < BitLimit; i++) {
        if (!(bm[i >> 3] & (1u << (i & 7)))) {
            if (run == 0) found = i;
            if (++run == Count) break;
        } else {
            run = 0; found = (ULONGLONG)-1;
        }
    }
    if (run < Count || found == (ULONGLONG)-1) { ExFreePool(bm); return STATUS_DISK_FULL; }

    /* write back the affected byte range (journal its old image first) */
    {
        ULONGLONG b0 = found >> 3;
        ULONGLONG b1 = (found + Count - 1) >> 3;
        ULONG wlen = (ULONG)(b1 - b0 + 1);
        PUCHAR old = NULL;
        if (NtfsJnlInTx(Vcb)) {
            old = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, wlen, NTFS_TAG_BUF);
            if (old) RtlCopyMemory(old, bm + b0, wlen);    /* image BEFORE setting bits */
        }
        /* set the bits */
        for (i = found; i < found + Count; i++) bm[i >> 3] |= (1u << (i & 7));
        if (old) { NtfsJnlRange(Vcb, JnlKind, 0, b0, old, bm + b0, wlen); ExFreePool(old); }
        Status = NtfsWriteAttributeData(Vcb, Runs, RunCount, b0, wlen, bm + b0);
    }
    ExFreePool(bm);
    if (NT_SUCCESS(Status)) *FirstBit = found;
    return Status;
}

/* generic bitmap free: clear Count bits starting at bit, write them back. */
static NTSTATUS
BitmapFreeRun(PNTFS_VCB Vcb, PNTFS_RUN Runs, ULONG RunCount, ULONGLONG BitmapBytes,
              ULONGLONG StartBit, ULONG Count, ULONG JnlKind)
{
    ULONGLONG b0 = StartBit >> 3;
    ULONGLONG b1 = (StartBit + Count - 1) >> 3;
    ULONG wlen = (ULONG)(b1 - b0 + 1);
    PUCHAR buf, old = NULL;
    ULONGLONG i;
    NTSTATUS Status;

    if (Count == 0) return STATUS_SUCCESS;
    if (b1 >= BitmapBytes) return STATUS_INVALID_PARAMETER;
    buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, wlen, NTFS_TAG_BUF);
    if (buf == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    Status = NtfsReadAttributeData(Vcb, Runs, RunCount, b0, wlen, buf);
    if (NT_SUCCESS(Status)) {
        if (NtfsJnlInTx(Vcb)) {
            old = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, wlen, NTFS_TAG_BUF);
            if (old) RtlCopyMemory(old, buf, wlen);        /* image BEFORE clearing bits */
        }
        for (i = StartBit; i < StartBit + Count; i++)
            buf[(i >> 3) - b0] &= ~(1u << (i & 7));
        if (old) { NtfsJnlRange(Vcb, JnlKind, 0, b0, old, buf, wlen); ExFreePool(old); }
        Status = NtfsWriteAttributeData(Vcb, Runs, RunCount, b0, wlen, buf);
    }
    ExFreePool(buf);
    return Status;
}

/* Count the free clusters in $Bitmap (64 KB at a time); used at mount for the
 * volume free-space query and then kept current by alloc/free. */
ULONGLONG
NtfsCountFreeClusters(PNTFS_VCB Vcb)
{
    PNTFS_RUN runs; ULONG rc; ULONGLONG bytes, off, freeC = 0, bit = 0;
    PUCHAR buf;
    const ULONG chunk = 0x10000;
    if (!NT_SUCCESS(LoadDataRuns(Vcb, MFT_RN_BITMAP, ATTR_DATA, NULL, 0, &runs, &rc, &bytes)))
        return 0;
    buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, chunk, NTFS_TAG_BUF);
    if (buf) {
        for (off = 0; off < bytes && bit < Vcb->Geo.TotalClusters; off += chunk) {
            ULONG n = (bytes - off > chunk) ? chunk : (ULONG)(bytes - off), i;
            if (!NT_SUCCESS(NtfsReadAttributeData(Vcb, runs, rc, off, n, buf))) break;
            for (i = 0; i < n && bit < Vcb->Geo.TotalClusters; i++) {
                UCHAR b = buf[i]; ULONG k;
                for (k = 0; k < 8 && bit < Vcb->Geo.TotalClusters; k++, bit++)
                    if (!(b & (1u << k))) freeC++;
            }
        }
        ExFreePool(buf);
    }
    ExFreePool(runs);
    return freeC;
}

NTSTATUS
NtfsAllocClusters(PNTFS_VCB Vcb, ULONG Count, ULONGLONG *FirstLcn)
{
    PNTFS_RUN runs; ULONG rc; ULONGLONG bytes;
    NTSTATUS Status;
    NTFS_LOCK_META(Vcb);
    Status = LoadDataRuns(Vcb, MFT_RN_BITMAP, ATTR_DATA, NULL, 0, &runs, &rc, &bytes);
    if (NT_SUCCESS(Status)) {
        Status = BitmapAllocRun(Vcb, runs, rc, bytes, Count, 0,
                                Vcb->Geo.TotalClusters, FirstLcn, NTFS_JNL_BITMAP);
        ExFreePool(runs);
        if (NT_SUCCESS(Status) && Vcb->FreeClusters >= Count) Vcb->FreeClusters -= Count;
    }
    NTFS_UNLOCK_META(Vcb);
    return Status;
}

/* Allocate up to Want clusters as ONE run: the first free run at/after Hint
 * (then from the start of the volume), shortened to Want. *Got = its length.
 * Lets a file grow on a fragmented volume, a piece at a time. */
NTSTATUS
NtfsAllocClustersPartial(PNTFS_VCB Vcb, ULONG Want, ULONGLONG Hint, ULONGLONG *FirstLcn, PULONG Got)
{
    PNTFS_RUN runs; ULONG rc; ULONGLONG bytes, i, start = (Hint < Vcb->Geo.TotalClusters) ? Hint : 0;
    PUCHAR bm = NULL;
    NTSTATUS Status;
    int pass;
    *Got = 0;
    if (Want == 0) return STATUS_INVALID_PARAMETER;
    NTFS_LOCK_META(Vcb);
    Status = LoadDataRuns(Vcb, MFT_RN_BITMAP, ATTR_DATA, NULL, 0, &runs, &rc, &bytes);
    if (!NT_SUCCESS(Status)) goto out;
    bm = (bytes && bytes <= 0x800000) ? (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, (ULONG)bytes, NTFS_TAG_BUF) : NULL;
    if (bm == NULL || !NT_SUCCESS(Status = NtfsReadAttributeData(Vcb, runs, rc, 0, (ULONG)bytes, bm))) {
        if (bm == NULL) Status = STATUS_INSUFFICIENT_RESOURCES;
        ExFreePool(runs); goto out;
    }
    Status = STATUS_DISK_FULL;
    for (pass = 0; pass < 2 && Status == STATUS_DISK_FULL; pass++) {
        ULONGLONG from = pass ? 0 : start, to = pass ? start : Vcb->Geo.TotalClusters;
        for (i = from; i < to; i++) {
            if (!(bm[i >> 3] & (1u << (i & 7)))) {
                ULONGLONG j = i, n = 0;
                while (j < Vcb->Geo.TotalClusters && n < Want && !(bm[j >> 3] & (1u << (j & 7)))) { j++; n++; }
                for (j = i; j < i + n; j++) bm[j >> 3] |= (UCHAR)(1u << (j & 7));
                Status = NtfsWriteAttributeData(Vcb, runs, rc, i >> 3,
                             (ULONG)(((i + n - 1) >> 3) - (i >> 3) + 1), bm + (i >> 3));
                if (NT_SUCCESS(Status)) {
                    *FirstLcn = i; *Got = (ULONG)n;
                    if (Vcb->FreeClusters >= n) Vcb->FreeClusters -= n;
                }
                break;
            }
        }
    }
    ExFreePool(runs);
out:
    if (bm) ExFreePool(bm);
    NTFS_UNLOCK_META(Vcb);
    return Status;
}

/* As NtfsAllocClusters, trying first at Hint (e.g. right after the last run
 * of the attribute being extended, to keep it contiguous). */
NTSTATUS
NtfsAllocClustersNear(PNTFS_VCB Vcb, ULONG Count, ULONGLONG Hint, ULONGLONG *FirstLcn)
{
    PNTFS_RUN runs; ULONG rc; ULONGLONG bytes;
    NTSTATUS Status;
    if (Hint == 0 || Hint >= Vcb->Geo.TotalClusters) return NtfsAllocClusters(Vcb, Count, FirstLcn);
    NTFS_LOCK_META(Vcb);
    Status = LoadDataRuns(Vcb, MFT_RN_BITMAP, ATTR_DATA, NULL, 0, &runs, &rc, &bytes);
    if (NT_SUCCESS(Status)) {
        Status = BitmapAllocRun(Vcb, runs, rc, bytes, Count, Hint, Vcb->Geo.TotalClusters, FirstLcn, NTFS_JNL_BITMAP);
        if (Status == STATUS_DISK_FULL)
            Status = BitmapAllocRun(Vcb, runs, rc, bytes, Count, 0, Vcb->Geo.TotalClusters, FirstLcn, NTFS_JNL_BITMAP);
        ExFreePool(runs);
        if (NT_SUCCESS(Status) && Vcb->FreeClusters >= Count) Vcb->FreeClusters -= Count;
    }
    NTFS_UNLOCK_META(Vcb);
    return Status;
}

NTSTATUS
NtfsFreeClusters(PNTFS_VCB Vcb, ULONGLONG FirstLcn, ULONG Count)
{
    PNTFS_RUN runs; ULONG rc; ULONGLONG bytes;
    NTSTATUS Status;
    NTFS_LOCK_META(Vcb);
    Status = LoadDataRuns(Vcb, MFT_RN_BITMAP, ATTR_DATA, NULL, 0, &runs, &rc, &bytes);
    if (NT_SUCCESS(Status)) {
        Status = BitmapFreeRun(Vcb, runs, rc, bytes, FirstLcn, Count, NTFS_JNL_BITMAP);
        ExFreePool(runs);
        if (NT_SUCCESS(Status)) Vcb->FreeClusters += Count;
    }
    NTFS_UNLOCK_META(Vcb);
    return Status;
}

/*
 * Grow the MFT by a few clusters (at least 16 records): extend $MFT's $DATA,
 * format the new records as free FILE records, grow $MFT:$BITMAP if needed,
 * and update the in-memory MFT run list. Caller holds the meta lock.
 */
static NTSTATUS
NtfsExtendMft(PNTFS_VCB Vcb)
{
    ULONG rs = Vcb->Geo.MftRecordSize, cs = Vcb->Geo.ClusterSize;
    ULONG grow = (16 * rs + cs - 1) / cs, rc, i, newRecs;
    PUCHAR rec = NULL, fr = NULL;
    PNTFS_RUN runs = NULL, newMft = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    ULONGLONG lcn, oldAlloc, oldDs, newAlloc, firstNew, recCount;
    NTSTATUS Status;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, rs, NTFS_TAG_BUF);
    fr = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, cs > rs ? cs : rs, NTFS_TAG_BUF);
    runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_MFT_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!rec || !fr || !runs) { Status = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    Status = NtfsReadMftRecord(Vcb, MFT_RN_MFT, rec);
    if (!NT_SUCCESS(Status)) goto out;
    m = (PMFT_RECORD)rec;
    if (NtfsFindAttribute(m, ATTR_ATTRIBUTE_LIST, NULL, 0)) { Status = STATUS_DISK_FULL; goto out; }
    a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    if (!a || !a->NonResident || !NtfsDecodeRuns(a, runs, NTFS_MAX_MFT_RUNS - 1, &rc) || !rc) {
        Status = STATUS_DISK_FULL; goto out;
    }
    oldAlloc = a->Form.NonResident.AllocatedSize;
    oldDs = a->Form.NonResident.DataSize;
    lcn = 0;
    if (oldAlloc >= oldDs + rs) {
        /* allocated but unused space past the MFT's EOF (Windows and ntfs-3g
         * preallocate): just take it into use */
        grow = 0;
        newAlloc = oldAlloc;
        a->Form.NonResident.DataSize = newAlloc;
        a->Form.NonResident.InitializedSize = newAlloc;
    } else {
        /* grow by 1/8 of the MFT (at least 16 records, at most 1 MB), right
         * after its last run when possible: fewer, longer runs */
        {
            ULONG eighth = (ULONG)(oldAlloc / 8 / cs);
            ULONG cap = 0x100000 / cs;
            ULONGLONG hint = runs[rc - 1].Sparse ? 0 : runs[rc - 1].Lcn + runs[rc - 1].Length;
            if (eighth > grow) grow = eighth;
            if (grow > cap && cap >= (16 * rs + cs - 1) / cs) grow = cap;
            grow = (grow + (rs / cs ? rs / cs : 1) - 1) / (rs / cs ? rs / cs : 1) * (rs / cs ? rs / cs : 1);
            Status = NtfsAllocClustersNear(Vcb, grow, hint, &lcn);
            if (!NT_SUCCESS(Status)) {
                grow = (16 * rs + cs - 1) / cs;
                Status = NtfsAllocClustersNear(Vcb, grow, hint, &lcn);
            }
        }
        if (!NT_SUCCESS(Status)) goto out;
        if (!runs[rc - 1].Sparse && runs[rc - 1].Lcn + runs[rc - 1].Length == lcn) {
            runs[rc - 1].Length += grow;
        } else {
            runs[rc].Vcn = runs[rc - 1].Vcn + runs[rc - 1].Length;
            runs[rc].Lcn = lcn; runs[rc].Length = grow; runs[rc].Sparse = FALSE; rc++;
        }
        newAlloc = oldAlloc + (ULONGLONG)grow * cs;
        if (!NtfsRewriteAttrRuns(m, rs, a, runs, rc, newAlloc, newAlloc)) {
            if (grow) NtfsFreeClusters(Vcb, lcn, grow); Status = STATUS_DISK_FULL; goto out;
        }
    }

    /* install the new run list so the new records can be written */
    newMft = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, rc * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (newMft == NULL) { if (grow) NtfsFreeClusters(Vcb, lcn, grow); Status = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    RtlCopyMemory(newMft, runs, rc * sizeof(NTFS_RUN));
    firstNew = oldDs / rs;              /* every record past the old EOF is formatted */
    recCount = newAlloc / rs;
    newRecs = (ULONG)(recCount - firstNew);

    /* $MFT:$BITMAP must cover recCount bits */
    a = NtfsFindAttribute(m, ATTR_BITMAP, NULL, 0);
    if (a == NULL) { ExFreePool(newMft); if (grow) NtfsFreeClusters(Vcb, lcn, grow); Status = STATUS_DISK_FULL; goto out; }
    {
        ULONGLONG need = ((recCount + 63) / 64) * 8;     /* bytes, 8-aligned */
        if (!a->NonResident) {
            if (NtfsResidentValueLength(a) < need &&
                !NtfsResizeResidentValue(m, rs, a, (ULONG)need)) {
                ExFreePool(newMft); if (grow) NtfsFreeClusters(Vcb, lcn, grow); Status = STATUS_DISK_FULL; goto out;
            }
        } else if (need > a->Form.NonResident.DataSize) {
            if (need > a->Form.NonResident.AllocatedSize) {
                /* add one zeroed cluster to the MFT bitmap */
                PNTFS_RUN br = runs;        /* $DATA runs already copied to newMft */
                ULONG bc; ULONGLONG blcn;
                if (!NtfsDecodeRuns(a, br, NTFS_MAX_IDX_RUNS - 1, &bc) || !bc ||
                    !NT_SUCCESS(NtfsAllocClusters(Vcb, 1, &blcn))) {
                    ExFreePool(newMft); if (grow) NtfsFreeClusters(Vcb, lcn, grow); Status = STATUS_DISK_FULL; goto out;
                }
                RtlZeroMemory(fr, cs);
                NtfsWriteDisk(Vcb, blcn * cs, cs, fr);
                if (br[bc - 1].Lcn + br[bc - 1].Length == blcn) br[bc - 1].Length++;
                else { br[bc].Vcn = br[bc - 1].Vcn + br[bc - 1].Length; br[bc].Lcn = blcn;
                       br[bc].Length = 1; br[bc].Sparse = FALSE; bc++; }
                if (!NtfsRewriteAttrRuns(m, rs, a, br, bc,
                        a->Form.NonResident.AllocatedSize + cs, need)) {
                    NtfsFreeClusters(Vcb, blcn, 1);
                    ExFreePool(newMft); if (grow) NtfsFreeClusters(Vcb, lcn, grow); Status = STATUS_DISK_FULL; goto out;
                }
                a = NtfsFindAttribute(m, ATTR_BITMAP, NULL, 0);
            }
            a->Form.NonResident.DataSize = need;
            a->Form.NonResident.InitializedSize = need;
        }
    }

    ExFreePool(Vcb->MftRuns);
    Vcb->MftRuns = newMft;
    Vcb->MftRunCount = rc;

    /* format the new records as free FILE records */
    for (i = 0; i < newRecs; i++) {
        PMFT_RECORD f = (PMFT_RECORD)fr;
        ULONG usaCount = rs / Vcb->Geo.BytesPerSector + 1;
        ULONG usaOff = Vcb->NtfsMajor < 3 ? 0x2A : 0x30;     /* 1.x / 3.x header */
        RtlZeroMemory(fr, rs);
        f->Rhdr.Type = NTFS_FILE_SIGNATURE;
        f->Rhdr.UsaOffset = (USHORT)usaOff;
        f->Rhdr.UsaCount = (USHORT)usaCount;
        f->SequenceNumber = 1;
        f->FirstAttributeOffset = (USHORT)((usaOff + usaCount * 2 + 7) & ~7u);
        f->Flags = 0;
        f->BytesAllocated = rs;
        if (Vcb->NtfsMajor >= 3) f->MftRecordNumber = (ULONG)(firstNew + i);
        *(PULONG)(fr + f->FirstAttributeOffset) = ATTR_END;
        f->BytesInUse = f->FirstAttributeOffset + 8;
        NtfsWriteMftRecord(Vcb, firstNew + i, fr);
    }
    Status = NtfsWriteMftRecord(Vcb, MFT_RN_MFT, rec);
    if (NT_SUCCESS(Status)) Vcb->MftRecordCount = recCount;
out:
    if (rec) ExFreePool(rec);
    if (fr) ExFreePool(fr);
    if (runs) ExFreePool(runs);
    return Status;
}

NTSTATUS
NtfsAllocMftRecord(PNTFS_VCB Vcb, ULONGLONG *RecordNumber)
{
    PNTFS_RUN runs; ULONG rc; ULONGLONG bytes;
    NTSTATUS Status;
    int pass;

    NTFS_LOCK_META(Vcb);
    for (pass = 0; pass < 2; pass++) {
        Status = LoadDataRuns(Vcb, MFT_RN_MFT, ATTR_BITMAP, NULL, 0, &runs, &rc, &bytes);
        if (!NT_SUCCESS(Status)) break;
        /* search from the first user record; cap at bitmap capacity or MFT size */
        {
            ULONGLONG limit = bytes * 8;
            if (limit > Vcb->MftRecordCount) limit = Vcb->MftRecordCount;
            Status = BitmapAllocRun(Vcb, runs, rc, bytes, 1,
                                    NTFS_FIRST_USER_MFT, limit, RecordNumber, NTFS_JNL_MFTBMP);
        }
        ExFreePool(runs);
        if (Status != STATUS_DISK_FULL || pass == 1) break;
        /* MFT full: grow it and retry once */
        Status = NtfsExtendMft(Vcb);
        if (!NT_SUCCESS(Status)) break;
    }
    NTFS_UNLOCK_META(Vcb);
    return Status;
}

NTSTATUS
NtfsFreeMftRecord(PNTFS_VCB Vcb, ULONGLONG RecordNumber)
{
    PNTFS_RUN runs; ULONG rc; ULONGLONG bytes;
    NTSTATUS Status;
    NTFS_LOCK_META(Vcb);
    Status = LoadDataRuns(Vcb, MFT_RN_MFT, ATTR_BITMAP, NULL, 0, &runs, &rc, &bytes);
    if (NT_SUCCESS(Status)) {
        Status = BitmapFreeRun(Vcb, runs, rc, bytes, RecordNumber, 1, NTFS_JNL_MFTBMP);
        ExFreePool(runs);
    }
    NTFS_UNLOCK_META(Vcb);
    return Status;
}

/* Load the volume's $UpCase table (record 10) once, so name collation and
 * case-insensitive lookups match Windows NTFS exactly for every language. */
VOID
NtfsLoadUpcase(PNTFS_VCB Vcb)
{
    PNTFS_RUN runs; ULONG rc; ULONGLONG bytes;
    PWCHAR tab;
    if (NtfsUpcaseTable != NULL) return;
    if (!NT_SUCCESS(LoadDataRuns(Vcb, 10, ATTR_DATA, NULL, 0, &runs, &rc, &bytes))) return;
    if (bytes >= 0x20000) {
        tab = (PWCHAR)ExAllocatePoolWithTag(PagedPool, 0x20000, NTFS_TAG_BUF);
        if (tab) {
            if (NT_SUCCESS(NtfsReadAttributeData(Vcb, runs, rc, 0, 0x20000, tab)) &&
                tab['a'] == 'A' && tab['z'] == 'Z' && tab['A'] == 'A')
                NtfsUpcaseTable = tab;
            else
                ExFreePool(tab);
        }
    }
    ExFreePool(runs);
}
