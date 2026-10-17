/*
 * NTFS3NT - files whose run list does not fit one MFT record.
 *
 * NTFS spreads such a file over several records: the unnamed $DATA is split
 * into extents, each covering a VCN range (LowestVcn..HighestVcn) and kept in
 * an extension record (BaseFileRecord = the file's base record), and the base
 * record gets an $ATTRIBUTE_LIST naming every attribute of the file and the
 * record that holds it. This module reads that layout (any number of extents,
 * resident or non-resident list) and writes it: whenever the run list no
 * longer fits the base record - or the file already has a list - the extents
 * are rebuilt from the full run list into fresh extension records and the
 * list is regenerated. Only the first extent carries the sizes.
 */

#define NTFS_KERNEL
#include "ntfs.h"

/* ---- reading -------------------------------------------------------------- */

/* append the runs of non-resident attribute a to the growing array *R */
static BOOLEAN
AppendRuns(PATTRIBUTE_RECORD a, PNTFS_RUN *R, PULONG Rc, PULONG Cap)
{
    ULONG mp = a->Length - a->Form.NonResident.MappingPairsOffset;
    ULONG max = mp / 2 + 2, n;
    PNTFS_RUN t = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, max * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (t == NULL) return FALSE;
    if (!NtfsDecodeRuns(a, t, max, &n)) { ExFreePool(t); return FALSE; }
    if (*Rc + n > *Cap) {
        ULONG nc = (*Rc + n) * 2 + 16;
        PNTFS_RUN g = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, nc * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (g == NULL) { ExFreePool(t); return FALSE; }
        if (*R) { RtlCopyMemory(g, *R, *Rc * sizeof(NTFS_RUN)); ExFreePool(*R); }
        *R = g; *Cap = nc;
    }
    RtlCopyMemory(*R + *Rc, t, n * sizeof(NTFS_RUN));
    *Rc += n;
    ExFreePool(t);
    return TRUE;
}

/* the attribute list of base record m as a pool copy (resident or not) */
static PUCHAR
ReadAttrList(PNTFS_VCB Vcb, PMFT_RECORD m, PULONG Len)
{
    PATTRIBUTE_RECORD al = NtfsFindAttribute(m, ATTR_ATTRIBUTE_LIST, NULL, 0);
    PUCHAR buf;
    *Len = 0;
    if (al == NULL) return NULL;
    if (!al->NonResident) {
        ULONG l = NtfsResidentValueLength(al);
        buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, l ? l : 8, NTFS_TAG_BUF);
        if (buf) { RtlCopyMemory(buf, NtfsResidentValue(al), l); *Len = l; }
        return buf;
    } else {
        PNTFS_RUN r = NULL; ULONG rc = 0, cap = 0;
        ULONG l = (ULONG)al->Form.NonResident.DataSize;
        if (l == 0 || l > 0x100000 || !AppendRuns(al, &r, &rc, &cap)) { if (r) ExFreePool(r); return NULL; }
        buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, l, NTFS_TAG_BUF);
        if (buf && !NT_SUCCESS(NtfsReadAttributeData(Vcb, r, rc, 0, l, buf))) { ExFreePool(buf); buf = NULL; }
        ExFreePool(r);
        if (buf) *Len = l;
        return buf;
    }
}

/* unnamed $DATA attribute with the given LowestVcn in record m */
static PATTRIBUTE_RECORD
FindDataExtent(PMFT_RECORD m, ULONGLONG LowestVcn)
{
    PUCHAR p = (PUCHAR)m;
    ULONG off = m->FirstAttributeOffset;
    while (off + 8 <= m->BytesInUse) {
        PATTRIBUTE_RECORD a = (PATTRIBUTE_RECORD)(p + off);
        if (a->Type == ATTR_END || a->Length == 0) break;
        if (a->Type == ATTR_DATA && a->NameLength == 0 &&
            (!a->NonResident ? LowestVcn == 0 : a->Form.NonResident.LowestVcn == LowestVcn))
            return a;
        off += a->Length;
    }
    return NULL;
}

/*
 * Load the unnamed $DATA of base record m (record BaseRec): resident or not,
 * in the base record or spread over extension records through the attribute
 * list. *Runs (pool, caller frees) gets every run in VCN order; *First a copy
 * of the first extent's header (sizes, flags, compression unit).
 */
NTSTATUS
NtfsLoadDataExtents(PNTFS_VCB Vcb, ULONGLONG BaseRec, PMFT_RECORD m,
                    PNTFS_RUN *Runs, PULONG Rc, PATTRIBUTE_RECORD First, PBOOLEAN HasList)
{
    PUCHAR list, xrec = NULL;
    ULONG len, off, cap = 0;
    PATTRIBUTE_RECORD a;
    BOOLEAN gotFirst = FALSE;

    *Runs = NULL; *Rc = 0; *HasList = FALSE;
    list = ReadAttrList(Vcb, m, &len);
    if (list == NULL) {
        a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
        if (a == NULL) return STATUS_OBJECT_NAME_NOT_FOUND;
        RtlCopyMemory(First, a, a->NonResident ? 0x40 : 0x18);
        if (!a->NonResident) return STATUS_SUCCESS;
        return AppendRuns(a, Runs, Rc, &cap) ? STATUS_SUCCESS : STATUS_FILE_CORRUPT_ERROR;
    }
    *HasList = TRUE;
    xrec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (xrec == NULL) { ExFreePool(list); return STATUS_INSUFFICIENT_RESOURCES; }
    for (off = 0; off + sizeof(ATTRIBUTE_LIST_ENTRY) <= len; ) {
        PATTRIBUTE_LIST_ENTRY x = (PATTRIBUTE_LIST_ENTRY)(list + off);
        if (x->Length == 0) break;
        if (x->Type == ATTR_DATA && x->NameLength == 0) {
            ULONGLONG r = NTFS_MREF_RECORD(x->AttributeReference);
            PMFT_RECORD rm = m;
            if (r != BaseRec) {
                if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, r, xrec))) goto bad;
                rm = (PMFT_RECORD)xrec;
            }
            a = FindDataExtent(rm, x->LowestVcn);
            if (a == NULL) goto bad;
            if (!gotFirst) { RtlCopyMemory(First, a, a->NonResident ? 0x40 : 0x18); gotFirst = TRUE; }
            if (a->NonResident && !AppendRuns(a, Runs, Rc, &cap)) goto bad;
        }
        off += x->Length;
    }
    ExFreePool(xrec); ExFreePool(list);
    return gotFirst ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
bad:
    ExFreePool(xrec); ExFreePool(list);
    if (*Runs) { ExFreePool(*Runs); *Runs = NULL; *Rc = 0; }
    return STATUS_FILE_CORRUPT_ERROR;
}

/* ---- attribute list construction ------------------------------------------ */

typedef struct _LENT {
    ULONG     Type;
    PWCHAR    Name;
    UCHAR     NameLen;
    ULONGLONG LowestVcn;
    ULONGLONG Ref;
    USHORT    Id;
} LENT;

static int
LentCmp(const LENT *a, const LENT *b)
{
    int c;
    if (a->Type != b->Type) return a->Type < b->Type ? -1 : 1;
    if (a->NameLen != b->NameLen || a->NameLen) {
        if (a->NameLen == 0) return -1;
        if (b->NameLen == 0) return 1;
        c = NtfsCollateName(a->Name, a->NameLen, b->Name, b->NameLen);
        if (c) return c;
    }
    if (a->LowestVcn != b->LowestVcn) return a->LowestVcn < b->LowestVcn ? -1 : 1;
    return 0;
}

/*
 * Rebuild the resident $ATTRIBUTE_LIST of base record m: one entry for every
 * attribute in m itself plus the external entries Ext[0..Next) (attributes
 * living in extension records), sorted by type, name and LowestVcn. Any old
 * list is replaced. FALSE if it does not fit the record.
 */
static BOOLEAN
WriteAttrList(PNTFS_VCB Vcb, PMFT_RECORD m, ULONGLONG BaseRec, const LENT *Ext, ULONG Next)
{
    ULONG rs = Vcb->Geo.MftRecordSize, n = 0, cap, i, j, total = 0, off;
    LENT *e;
    PUCHAR val, ab;
    PATTRIBUTE_RECORD a, old;
    ULONGLONG baseRef = (BaseRec & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)m->SequenceNumber << 48);
    BOOLEAN ok = FALSE;

    old = NtfsFindAttribute(m, ATTR_ATTRIBUTE_LIST, NULL, 0);
    if (old) {
        if (old->NonResident) {
            NTFS_RUN r[8]; ULONG rc;
            if (NtfsDecodeRuns(old, r, 8, &rc))
                for (i = 0; i < rc; i++)
                    if (!r[i].Sparse && r[i].Length) NtfsFreeClusters(Vcb, r[i].Lcn, (ULONG)r[i].Length);
        }
        {   /* remove it */
            PUCHAR p = (PUCHAR)m, at = (PUCHAR)old;
            ULONG l = old->Length, end = (ULONG)(at - p) + l;
            memmove(at, at + l, m->BytesInUse - end);
            m->BytesInUse -= l;
        }
    }
    cap = Next + 64;
    e = (LENT *)ExAllocatePoolWithTag(NonPagedPool, cap * sizeof(LENT), NTFS_TAG_BUF);
    if (e == NULL) return FALSE;
    for (off = m->FirstAttributeOffset; off + 8 <= m->BytesInUse && n < cap - Next; ) {
        a = (PATTRIBUTE_RECORD)((PUCHAR)m + off);
        if (a->Type == ATTR_END || a->Length == 0) break;
        e[n].Type = a->Type;
        e[n].Name = NtfsAttributeName(a);
        e[n].NameLen = a->NameLength;
        e[n].LowestVcn = a->NonResident ? a->Form.NonResident.LowestVcn : 0;
        e[n].Ref = baseRef;
        e[n].Id = a->AttributeId;
        n++;
        off += a->Length;
    }
    for (i = 0; i < Next; i++) e[n++] = Ext[i];
    for (i = 1; i < n; i++) {                       /* insertion sort */
        LENT t = e[i];
        for (j = i; j > 0 && LentCmp(&e[j - 1], &t) > 0; j--) e[j] = e[j - 1];
        e[j] = t;
    }
    for (i = 0; i < n; i++) total += (0x1A + e[i].NameLen * 2 + 7) & ~7u;
    if (m->BytesInUse + 0x18 + ((total + 7) & ~7u) > rs) goto out;

    ab = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 0x18 + total + 8, NTFS_TAG_BUF);
    if (ab == NULL) goto out;
    RtlZeroMemory(ab, 0x18 + total + 8);
    a = (PATTRIBUTE_RECORD)ab;
    a->Type = ATTR_ATTRIBUTE_LIST; a->NameOffset = 0x18;
    a->Form.Resident.ValueOffset = 0x18; a->Form.Resident.ValueLength = total;
    a->Length = (0x18 + total + 7) & ~7u;
    val = ab + 0x18;
    for (i = 0, off = 0; i < n; i++) {
        PATTRIBUTE_LIST_ENTRY x = (PATTRIBUTE_LIST_ENTRY)(val + off);
        ULONG el = (0x1A + e[i].NameLen * 2 + 7) & ~7u;
        x->Type = e[i].Type; x->Length = (USHORT)el;
        x->NameLength = e[i].NameLen; x->NameOffset = 0x1A;
        x->LowestVcn = e[i].LowestVcn; x->AttributeReference = e[i].Ref; x->AttributeId = e[i].Id;
        if (e[i].NameLen) RtlCopyMemory(val + off + 0x1A, e[i].Name, e[i].NameLen * 2);
        off += el;
    }
    /* the names point into m, which the insertion shifts: copy done above */
    ok = NtfsInsertAttribute(m, rs, ab, a->Length);
    ExFreePool(ab);
out:
    ExFreePool(e);
    return ok;
}

/* external (extension-record) entries of base record m's current list */
static ULONG
ExternalEntries(PNTFS_VCB Vcb, PMFT_RECORD m, ULONGLONG BaseRec, LENT **Out, PWCHAR NameBuf)
{
    ULONG len, off, n = 0, cnt = 0, nb = 0;
    PUCHAR list = ReadAttrList(Vcb, m, &len);
    LENT *e;
    *Out = NULL;
    if (list == NULL) return 0;
    for (off = 0; off + sizeof(ATTRIBUTE_LIST_ENTRY) <= len && ((PATTRIBUTE_LIST_ENTRY)(list + off))->Length;
         off += ((PATTRIBUTE_LIST_ENTRY)(list + off))->Length) cnt++;
    e = (LENT *)ExAllocatePoolWithTag(NonPagedPool, (cnt + 1) * sizeof(LENT), NTFS_TAG_BUF);
    if (e == NULL) { ExFreePool(list); return 0; }
    for (off = 0; off + sizeof(ATTRIBUTE_LIST_ENTRY) <= len; ) {
        PATTRIBUTE_LIST_ENTRY x = (PATTRIBUTE_LIST_ENTRY)(list + off);
        if (x->Length == 0) break;
        if (NTFS_MREF_RECORD(x->AttributeReference) != BaseRec && nb + x->NameLength <= 1024) {
            e[n].Type = x->Type; e[n].NameLen = x->NameLength;
            e[n].Name = NameBuf + nb;
            RtlCopyMemory(NameBuf + nb, list + off + x->NameOffset, x->NameLength * 2);
            nb += x->NameLength;
            e[n].LowestVcn = x->LowestVcn; e[n].Ref = x->AttributeReference; e[n].Id = x->AttributeId;
            n++;
        }
        off += x->Length;
    }
    ExFreePool(list);
    *Out = e;
    return n;
}

/*
 * Base record m (not yet written) had attributes added/removed (e.g. its names
 * after a rename): bring its attribute list, if it has one, up to date.
 */
BOOLEAN
NtfsRefreshAttrList(PNTFS_VCB Vcb, PMFT_RECORD m, ULONGLONG BaseRec)
{
    LENT *ext;
    PWCHAR names;
    ULONG n;
    BOOLEAN ok;
    if (!NtfsFindAttribute(m, ATTR_ATTRIBUTE_LIST, NULL, 0)) return TRUE;
    names = (PWCHAR)ExAllocatePoolWithTag(NonPagedPool, 1024 * sizeof(WCHAR), NTFS_TAG_BUF);
    if (names == NULL) return FALSE;
    n = ExternalEntries(Vcb, m, BaseRec, &ext, names);
    ok = WriteAttrList(Vcb, m, BaseRec, ext, n);
    if (ext) ExFreePool(ext);
    ExFreePool(names);
    return ok;
}

/* ---- writing the extents -------------------------------------------------- */

static ULONG SLen(LONGLONG v)
{
    ULONG n = 1;
    while (n < 8) {
        LONGLONG lo = -(((LONGLONG)1) << (8 * n - 1)), hi = (((LONGLONG)1) << (8 * n - 1)) - 1;
        if (v >= lo && v <= hi) break;
        n++;
    }
    return n;
}

/*
 * Store Runs[0..Rc) as the file's unnamed $DATA spread over extension records
 * (each extent as large as a record allows), regenerate the attribute list,
 * then release the extension records of the previous layout. The base record
 * keeps everything else. Sizes go into the first extent.
 */
NTSTATUS
NtfsWriteDataExtents(PNTFS_VCB Vcb, PNTFS_FCB Fcb, PNTFS_RUN Runs, ULONG Rc,
                     ULONGLONG Alloc, ULONGLONG Size, ULONGLONG Init)
{
    ULONG rs = Vcb->Geo.MftRecordSize, bps = Vcb->Geo.BytesPerSector;
    ULONG usaOff = Vcb->NtfsMajor < 3 ? 0x2A : 0x30;           /* 1.x / 3.x record header */
    ULONG usaCount = rs / bps + 1, firstOff = (usaOff + usaCount * 2 + 7) & ~7u;
    ULONG mpCap = rs - firstOff - 0x40 - 8;
    PUCHAR base = NULL, xr = NULL;
    PMFT_RECORD m;
    LENT *ext = NULL, *oldExt = NULL;
    PWCHAR names = NULL;
    ULONG nOld = 0, nExt = 0, maxExt, i, start;
    ULONGLONG *newRecs = NULL;
    PATTRIBUTE_RECORD a;
    NTSTATUS Status;

    if (Rc == 0) return STATUS_INVALID_PARAMETER;
    base = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, rs, NTFS_TAG_BUF);
    xr = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, rs, NTFS_TAG_BUF);
    names = (PWCHAR)ExAllocatePoolWithTag(NonPagedPool, 1024 * sizeof(WCHAR), NTFS_TAG_BUF);
    maxExt = Rc + 1;
    ext = (LENT *)ExAllocatePoolWithTag(NonPagedPool, maxExt * sizeof(LENT), NTFS_TAG_BUF);
    newRecs = (ULONGLONG *)ExAllocatePoolWithTag(NonPagedPool, maxExt * sizeof(ULONGLONG), NTFS_TAG_BUF);
    if (!base || !xr || !names || !ext || !newRecs) { Status = STATUS_INSUFFICIENT_RESOURCES; goto out; }

    NTFS_LOCK_META(Vcb);
    Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, base);
    if (!NT_SUCCESS(Status)) goto unlock;
    m = (PMFT_RECORD)base;

    /* the previous layout's extension records (freed at the end) */
    nOld = ExternalEntries(Vcb, m, Fcb->MftRecord, &oldExt, names);

    /* the base record keeps everything but the unnamed $DATA */
    while ((a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0)) != NULL) {
        PUCHAR p = (PUCHAR)m, at = (PUCHAR)a;
        ULONG l = a->Length, end = (ULONG)(at - p) + l;
        memmove(at, at + l, m->BytesInUse - end);
        m->BytesInUse -= l;
    }

    /* cut the run list into record-sized extents and write each one */
    for (start = 0; start < Rc; ) {
        ULONG end = start, bytes = 1, mpLen;
        LONGLONG prev = 0;
        PMFT_RECORD x = (PMFT_RECORD)xr;
        ULONGLONG rec;
        USHORT seq = 1;
        while (end < Rc) {
            ULONG b = 1 + SLen((LONGLONG)Runs[end].Length) +
                      (Runs[end].Sparse ? 0 : SLen((LONGLONG)Runs[end].Lcn - prev));
            if (bytes + b > mpCap) break;
            bytes += b;
            if (!Runs[end].Sparse) prev = (LONGLONG)Runs[end].Lcn;
            end++;
        }
        if (end == start) { Status = STATUS_FILE_CORRUPT_ERROR; goto undo; }
        Status = NtfsAllocMftRecord(Vcb, &rec);
        if (!NT_SUCCESS(Status)) goto undo;
        newRecs[nExt] = rec;
        if (NT_SUCCESS(NtfsReadMftRecord(Vcb, rec, xr)) && x->SequenceNumber) seq = x->SequenceNumber;
        RtlZeroMemory(xr, rs);
        x->Rhdr.Type = NTFS_FILE_SIGNATURE;
        x->Rhdr.UsaOffset = (USHORT)usaOff; x->Rhdr.UsaCount = (USHORT)usaCount;
        x->SequenceNumber = seq;
        x->HardLinkCount = 0;
        x->FirstAttributeOffset = (USHORT)firstOff;
        x->Flags = MFT_RECORD_IN_USE;
        x->BytesAllocated = rs;
        x->BaseFileRecord = (Fcb->MftRecord & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)m->SequenceNumber << 48);
        x->NextAttributeId = 1;
        if (Vcb->NtfsMajor >= 3) x->MftRecordNumber = (ULONG)rec;
        a = (PATTRIBUTE_RECORD)(xr + firstOff);
        a->Type = ATTR_DATA; a->NonResident = 1; a->NameOffset = 0x40; a->AttributeId = 0;
        a->Form.NonResident.LowestVcn = Runs[start].Vcn;
        a->Form.NonResident.HighestVcn = Runs[end - 1].Vcn + Runs[end - 1].Length - 1;
        a->Form.NonResident.MappingPairsOffset = 0x40;
        if (start == 0) {
            a->Form.NonResident.AllocatedSize = Alloc;
            a->Form.NonResident.DataSize = Size;
            a->Form.NonResident.InitializedSize = Init;
        }
        if (!NtfsEncodeRuns(&Runs[start], end - start, xr + firstOff + 0x40, mpCap, &mpLen)) {
            NtfsFreeMftRecord(Vcb, rec); Status = STATUS_FILE_CORRUPT_ERROR; goto undo;
        }
        a->Length = (0x40 + mpLen + 7) & ~7u;
        *(PULONG)(xr + firstOff + a->Length) = ATTR_END;
        x->BytesInUse = firstOff + a->Length + 8;
        Status = NtfsWriteMftRecord(Vcb, rec, xr);
        if (!NT_SUCCESS(Status)) { NtfsFreeMftRecord(Vcb, rec); goto undo; }
        ext[nExt].Type = ATTR_DATA; ext[nExt].Name = NULL; ext[nExt].NameLen = 0;
        ext[nExt].LowestVcn = Runs[start].Vcn;
        ext[nExt].Ref = (rec & 0xFFFFFFFFFFFFULL) | ((ULONGLONG)seq << 48);
        ext[nExt].Id = 0;
        nExt++;
        start = end;
    }
    /* keep the other external attributes the old list named (none expected) */
    for (i = 0; i < nOld; i++)
        if (!(oldExt[i].Type == ATTR_DATA && oldExt[i].NameLen == 0) && nExt < maxExt)
            ext[nExt++] = oldExt[i];

    if (!WriteAttrList(Vcb, m, Fcb->MftRecord, ext, nExt)) { Status = STATUS_DISK_FULL; goto undo; }
    Status = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, base);
    if (!NT_SUCCESS(Status)) goto undo;

    /* release the previous layout's $DATA extension records */
    for (i = 0; i < nOld; i++) {
        ULONGLONG r = NTFS_MREF_RECORD(oldExt[i].Ref);
        ULONG j; BOOLEAN dup = FALSE;
        if (!(oldExt[i].Type == ATTR_DATA && oldExt[i].NameLen == 0)) continue;
        for (j = 0; j < i; j++) if (NTFS_MREF_RECORD(oldExt[j].Ref) == r) dup = TRUE;
        if (dup || !NT_SUCCESS(NtfsReadMftRecord(Vcb, r, xr))) continue;
        ((PMFT_RECORD)xr)->Flags &= ~MFT_RECORD_IN_USE;
        ((PMFT_RECORD)xr)->SequenceNumber++;
        NtfsWriteMftRecord(Vcb, r, xr);
        NtfsFreeMftRecord(Vcb, r);
    }

    /* the FCB's run list */
    {
        PNTFS_RUN nr = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, Rc * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (nr) {
            RtlCopyMemory(nr, Runs, Rc * sizeof(NTFS_RUN));
            if (Fcb->DataRuns) ExFreePool(Fcb->DataRuns);
            Fcb->DataRuns = nr; Fcb->DataRunCount = Rc;
        }
    }
    Status = STATUS_SUCCESS;
    goto unlock;
undo:
    for (i = 0; i < nExt && i < maxExt; i++)
        if (ext[i].Type == ATTR_DATA && ext[i].NameLen == 0 && ext[i].Name == NULL &&
            NTFS_MREF_RECORD(ext[i].Ref) == newRecs[i]) {
            if (NT_SUCCESS(NtfsReadMftRecord(Vcb, newRecs[i], xr))) {
                ((PMFT_RECORD)xr)->Flags &= ~MFT_RECORD_IN_USE;
                NtfsWriteMftRecord(Vcb, newRecs[i], xr);
            }
            NtfsFreeMftRecord(Vcb, newRecs[i]);
        }
unlock:
    NTFS_UNLOCK_META(Vcb);
out:
    if (base) ExFreePool(base);
    if (xr) ExFreePool(xr);
    if (names) ExFreePool(names);
    if (ext) ExFreePool(ext);
    if (oldExt) ExFreePool(oldExt);
    if (newRecs) ExFreePool(newRecs);
    return Status;
}

/* Update DataSize/InitializedSize in the first extent of a file with an
 * attribute list (the extent may live in an extension record). */
NTSTATUS
NtfsSetListDataSize(PNTFS_VCB Vcb, PNTFS_FCB Fcb, ULONGLONG Size)
{
    PUCHAR base, xr;
    ULONG len, off;
    PUCHAR list;
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

    base = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    xr = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!base || !xr) { Status = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    if (!NT_SUCCESS(Status = NtfsReadMftRecord(Vcb, Fcb->MftRecord, base))) goto out;
    list = ReadAttrList(Vcb, (PMFT_RECORD)base, &len);
    Status = STATUS_NOT_IMPLEMENTED;
    if (list == NULL) goto out;
    for (off = 0; off + sizeof(ATTRIBUTE_LIST_ENTRY) <= len; ) {
        PATTRIBUTE_LIST_ENTRY x = (PATTRIBUTE_LIST_ENTRY)(list + off);
        if (x->Length == 0) break;
        if (x->Type == ATTR_DATA && x->NameLength == 0 && x->LowestVcn == 0) {
            ULONGLONG r = NTFS_MREF_RECORD(x->AttributeReference);
            PUCHAR rb = (r == Fcb->MftRecord) ? base : xr;
            PATTRIBUTE_RECORD a;
            if (rb == xr && !NT_SUCCESS(Status = NtfsReadMftRecord(Vcb, r, xr))) break;
            a = FindDataExtent((PMFT_RECORD)rb, 0);
            if (a && a->NonResident) {
                a->Form.NonResident.DataSize = Size;
                a->Form.NonResident.InitializedSize = Size;
                Status = NtfsWriteMftRecord(Vcb, r, rb);
            }
            break;
        }
        off += x->Length;
    }
    ExFreePool(list);
out:
    if (base) ExFreePool(base);
    if (xr) ExFreePool(xr);
    return Status;
}
