/*
 * NTFS3NT - shared, kernel-free NTFS logic: boot parsing, record fixups,
 * attribute walking, data-run decoding, index iteration. No I/O, no kernel
 * calls (x86 little-endian, so on-disk fields are read directly). Built into
 * the driver, the tools and the Linux test.
 */

#if defined(NTFS_CROSS) || defined(NTFS_KERNEL)
#include "ntfs.h"
#else
#include "ntfsusr.h"
#include "ntfssup.h"
#endif

/* ------------------------------------------------------------------ */

static int IsPow2(ULONG v) { return v != 0 && (v & (v - 1)) == 0; }

/* Decode NTFS's signed "clusters per X" field into a byte size. */
static ULONG RecSizeFromField(CHAR field, ULONG clusterSize)
{
    if (field > 0) {
        return (ULONG)field * clusterSize;
    }
    /* negative: size is 1 << (-field) bytes */
    if (field >= -31 && field < 0) {
        return (ULONG)1 << (ULONG)(-field);
    }
    return 0;
}

BOOLEAN
NtfsParseBoot(const void *Sector, ULONG SectorBytes, PNTFS_GEOMETRY Geo)
{
    const NTFS_BOOT_SECTOR *b = (const NTFS_BOOT_SECTOR *)Sector;
    ULONG spc;

    if (SectorBytes < sizeof(NTFS_BOOT_SECTOR)) {
        return FALSE;
    }
    if (memcmp(b->Oem, "NTFS    ", 8) != 0) {
        return FALSE;
    }
    if (!IsPow2(b->BytesPerSector) ||
        b->BytesPerSector < 256 || b->BytesPerSector > 4096) {
        return FALSE;
    }

    /* SectorsPerCluster: literal if <=0x80, else 1 << (256 - value). */
    if (b->SectorsPerCluster <= 0x80) {
        spc = b->SectorsPerCluster;
    } else {
        spc = (ULONG)1 << (256 - b->SectorsPerCluster);
    }
    if (!IsPow2(spc)) {
        return FALSE;
    }

    RtlZeroMemory(Geo, sizeof(*Geo));
    Geo->BytesPerSector    = b->BytesPerSector;
    Geo->SectorsPerCluster = spc;
    Geo->ClusterSize       = b->BytesPerSector * spc;
    Geo->TotalSectors      = b->TotalSectors;
    Geo->TotalClusters     = spc ? (b->TotalSectors / spc) : 0;
    Geo->MftLcn            = b->MftLcn;
    Geo->MftMirrLcn        = b->MftMirrLcn;
    Geo->SerialNumber      = b->SerialNumber;
    Geo->MftRecordSize     = RecSizeFromField(b->ClustersPerMftRecord, Geo->ClusterSize);
    Geo->IndexBlockSize    = RecSizeFromField(b->ClustersPerIndexBlock, Geo->ClusterSize);

    if (Geo->MftRecordSize == 0 || Geo->IndexBlockSize == 0) {
        return FALSE;
    }
    return TRUE;
}

BOOLEAN
NtfsApplyFixup(void *Record, ULONG RecordSize, ULONG BytesPerSector)
{
    PNTFS_RECORD_HEADER rh = (PNTFS_RECORD_HEADER)Record;
    PUCHAR base = (PUCHAR)Record;
    PUSHORT usa;
    USHORT usn;
    ULONG sectors, i;

    if (rh->Type != NTFS_FILE_SIGNATURE && rh->Type != NTFS_INDX_SIGNATURE) {
        return FALSE;
    }
    if (BytesPerSector == 0 || (RecordSize % BytesPerSector) != 0) {
        return FALSE;
    }

    sectors = RecordSize / BytesPerSector;

    /* UsaCount is the USN entry plus one fixup per sector. */
    if (rh->UsaCount == 0 || (ULONG)(rh->UsaCount - 1) != sectors) {
        return FALSE;
    }
    if ((ULONG)rh->UsaOffset + (ULONG)rh->UsaCount * 2 > RecordSize) {
        return FALSE;
    }

    usa = (PUSHORT)(base + rh->UsaOffset);
    usn = usa[0];

    for (i = 0; i < sectors; i++) {
        PUSHORT tail = (PUSHORT)(base + i * BytesPerSector + BytesPerSector - 2);
        if (*tail != usn) {
            return FALSE;          /* torn / corrupt record */
        }
        *tail = usa[i + 1];        /* restore the real last two bytes */
    }
    return TRUE;
}

void
NtfsSetFixup(void *Record, ULONG RecordSize, ULONG BytesPerSector)
{
    PNTFS_RECORD_HEADER rh = (PNTFS_RECORD_HEADER)Record;
    PUCHAR base = (PUCHAR)Record;
    PUSHORT usa = (PUSHORT)(base + rh->UsaOffset);
    ULONG sectors = RecordSize / BytesPerSector;
    USHORT usn;
    ULONG i;

    /* advance the USN, skipping 0 and 0xFFFF which are reserved */
    usn = (USHORT)(usa[0] + 1);
    if (usn == 0 || usn == 0xFFFF) {
        usn = 1;
    }
    usa[0] = usn;

    for (i = 0; i < sectors; i++) {
        PUSHORT tail = (PUSHORT)(base + i * BytesPerSector + BytesPerSector - 2);
        usa[i + 1] = *tail;        /* stash the real last two bytes */
        *tail = usn;               /* stamp the sequence number     */
    }
}

/* ------------------------------------------------------------------ */

PATTRIBUTE_RECORD
NtfsFirstAttribute(PMFT_RECORD Record)
{
    return (PATTRIBUTE_RECORD)((PUCHAR)Record + Record->FirstAttributeOffset);
}

PATTRIBUTE_RECORD
NtfsNextAttribute(PMFT_RECORD Record, PATTRIBUTE_RECORD Attr)
{
    PUCHAR base = (PUCHAR)Record;
    ULONG off, next;

    if (Attr->Type == ATTR_END || Attr->Length == 0) {
        return NULL;
    }
    off = (ULONG)((PUCHAR)Attr - base);
    next = off + Attr->Length;

    /* must stay inside the used part of the record and leave room for a header */
    if (next + 8 > Record->BytesInUse || next >= Record->BytesAllocated) {
        return NULL;
    }
    return (PATTRIBUTE_RECORD)(base + next);
}

PATTRIBUTE_RECORD
NtfsFindAttribute(PMFT_RECORD Record, ULONG Type,
                  const WCHAR *Name, ULONG NameLength)
{
    PATTRIBUTE_RECORD a;

    for (a = NtfsFirstAttribute(Record); a != NULL; a = NtfsNextAttribute(Record, a)) {
        if (a->Type == ATTR_END) {
            break;
        }
        if (a->Type != Type) {
            continue;
        }
        if (Name == NULL) {
            if (a->NameLength == 0) {
                return a;
            }
        } else {
            if (a->NameLength == NameLength) {
                PWCHAR an = NtfsAttributeName(a);
                ULONG k;
                for (k = 0; k < NameLength; k++) {
                    if (an[k] != Name[k]) break;
                }
                if (k == NameLength) {
                    return a;
                }
            }
        }
    }
    return NULL;
}

PVOID
NtfsResidentValue(PATTRIBUTE_RECORD Attr)
{
    return (PUCHAR)Attr + Attr->Form.Resident.ValueOffset;
}

ULONG
NtfsResidentValueLength(PATTRIBUTE_RECORD Attr)
{
    return Attr->Form.Resident.ValueLength;
}

PWCHAR
NtfsAttributeName(PATTRIBUTE_RECORD Attr)
{
    if (Attr->NameLength == 0) {
        return NULL;
    }
    return (PWCHAR)((PUCHAR)Attr + Attr->NameOffset);
}

/* ------------------------------------------------------------------ */

BOOLEAN
NtfsDecodeRuns(PATTRIBUTE_RECORD Attr, PNTFS_RUN Out, ULONG MaxRuns, PULONG Count)
{
    PUCHAR p;
    PUCHAR end;
    ULONGLONG vcn;
    LONGLONG lcn = 0;
    ULONG n = 0;

    *Count = 0;
    if (!Attr->NonResident) {
        return FALSE;
    }

    p   = (PUCHAR)Attr + Attr->Form.NonResident.MappingPairsOffset;
    end = (PUCHAR)Attr + Attr->Length;
    vcn = Attr->Form.NonResident.LowestVcn;

    while (p < end && *p != 0) {
        ULONG lenBytes = *p & 0x0F;
        ULONG offBytes = (*p >> 4) & 0x0F;
        ULONGLONG runLen = 0;
        LONGLONG runOff = 0;
        ULONG i;

        p++;
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8) {
            return FALSE;
        }
        if (p + lenBytes + offBytes > end) {
            return FALSE;
        }

        for (i = 0; i < lenBytes; i++) {
            runLen |= (ULONGLONG)p[i] << (8 * i);
        }
        p += lenBytes;

        if (offBytes > 0) {
            for (i = 0; i < offBytes; i++) {
                runOff |= (LONGLONG)p[i] << (8 * i);
            }
            /* sign-extend from offBytes */
            if ((p[offBytes - 1] & 0x80) && offBytes < 8) {
                runOff |= (LONGLONG)(~(ULONGLONG)0 << (8 * offBytes));
            }
            p += offBytes;
        }

        if (n >= MaxRuns) {
            return FALSE;
        }

        Out[n].Vcn    = vcn;
        Out[n].Length = runLen;
        if (offBytes == 0) {
            Out[n].Sparse = TRUE;
            Out[n].Lcn    = 0;
        } else {
            lcn += runOff;
            Out[n].Sparse = FALSE;
            Out[n].Lcn    = (ULONGLONG)lcn;
        }
        vcn += runLen;
        n++;
    }

    *Count = n;
    return TRUE;
}

/* number of bytes needed to hold v as an unsigned little-endian value (>=1) */
static ULONG UnsignedLen(ULONGLONG v)
{
    ULONG n = 1;
    while (v >> (8 * n)) n++;
    return n;
}

/* number of bytes needed to hold v as a signed little-endian value */
static ULONG SignedLen(LONGLONG v)
{
    ULONG n = 1;
    while (1) {
        LONGLONG lo = -(((LONGLONG)1) << (8 * n - 1));
        LONGLONG hi =  (((LONGLONG)1) << (8 * n - 1)) - 1;
        if (v >= lo && v <= hi) break;
        n++;
        if (n >= 8) break;
    }
    return n;
}

BOOLEAN
NtfsEncodeRuns(PNTFS_RUN Runs, ULONG Count, PUCHAR Out, ULONG OutMax, PULONG OutLen)
{
    ULONG o = 0, i, k;
    LONGLONG prevLcn = 0;

    for (i = 0; i < Count; i++) {
        /* lengths are signed in NTFS (Windows and ntfs-3g reject a length
         * byte string with the top bit set): encode like the LCN deltas */
        ULONG lenBytes = SignedLen((LONGLONG)Runs[i].Length);
        ULONG offBytes;
        LONGLONG delta;

        if (Runs[i].Sparse) {
            offBytes = 0;              /* sparse run: zero offset field */
            delta = 0;
        } else {
            delta = (LONGLONG)Runs[i].Lcn - prevLcn;
            offBytes = SignedLen(delta);
        }
        if (o + 1 + lenBytes + offBytes + 1 > OutMax) return FALSE;

        Out[o++] = (UCHAR)((offBytes << 4) | lenBytes);
        for (k = 0; k < lenBytes; k++) Out[o++] = (UCHAR)(Runs[i].Length >> (8 * k));
        for (k = 0; k < offBytes; k++) Out[o++] = (UCHAR)(((ULONGLONG)delta) >> (8 * k));
        if (!Runs[i].Sparse) prevLcn = (LONGLONG)Runs[i].Lcn;
    }
    if (o + 1 > OutMax) return FALSE;
    Out[o++] = 0;                      /* terminator */
    *OutLen = o;
    return TRUE;
}

/*
 * Rewrite the unnamed non-resident $DATA attribute of record m with a new run
 * list and sizes: re-encode the mapping pairs, grow/shrink the attribute in
 * place (shifting the attributes that follow and updating BytesInUse), and
 * update HighestVcn/AllocatedSize/DataSize/InitializedSize. Returns FALSE if the
 * new mapping pairs don't fit in the record. Pure in-memory work (no I/O), so it
 * is shared by the kernel write path and the user-mode write test.
 */
/* Rewrite a GIVEN non-resident attribute `a` in record m with a new run list and
 * sizes (shifting following attributes, updating BytesInUse). Works for the
 * unnamed $DATA, a named $DATA (ADS) or any other non-resident attribute. */
BOOLEAN
NtfsRewriteAttrRuns(PMFT_RECORD m, ULONG RecSize, PATTRIBUTE_RECORD a,
                    PNTFS_RUN Runs, ULONG RunCount, ULONGLONG AllocSize, ULONGLONG DataSize)
{
    UCHAR mp[1024];
    ULONG mpLen, mpOff, oldLen, newLen, attrOff, attrEnd, recUsed, tail;
    LONG delta;
    PUCHAR ra;

    if (a == NULL || !a->NonResident) return FALSE;
    if (!NtfsEncodeRuns(Runs, RunCount, mp, sizeof(mp), &mpLen)) return FALSE;

    ra = (PUCHAR)a;
    mpOff = a->Form.NonResident.MappingPairsOffset;
    oldLen = a->Length;
    newLen = (mpOff + mpLen + 7) & ~7u;
    delta = (LONG)newLen - (LONG)oldLen;
    recUsed = m->BytesInUse;
    if ((LONG)recUsed + delta > (LONG)RecSize) return FALSE;   /* would overflow */

    attrOff = (ULONG)(ra - (PUCHAR)m);
    attrEnd = attrOff + oldLen;
    tail = recUsed - attrEnd;                 /* following attrs incl END marker */
    if (delta != 0 && tail > 0) {
        memmove(ra + newLen, ra + oldLen, tail);
    }
    if (newLen > mpOff) RtlZeroMemory(ra + mpOff, newLen - mpOff);
    RtlCopyMemory(ra + mpOff, mp, mpLen);

    a->Length = newLen;
    a->Form.NonResident.HighestVcn =
        Runs[RunCount - 1].Vcn + Runs[RunCount - 1].Length - 1;
    a->Form.NonResident.AllocatedSize = AllocSize;
    a->Form.NonResident.DataSize = DataSize;
    a->Form.NonResident.InitializedSize = DataSize;
    m->BytesInUse = (ULONG)((LONG)recUsed + delta);
    return TRUE;
}

/* Rewrite the unnamed non-resident $DATA run list + sizes (common case). */
BOOLEAN
NtfsRewriteDataRuns(PMFT_RECORD m, ULONG RecSize, PNTFS_RUN Runs, ULONG RunCount,
                    ULONGLONG AllocSize, ULONGLONG DataSize)
{
    return NtfsRewriteAttrRuns(m, RecSize, NtfsFindAttribute(m, ATTR_DATA, NULL, 0),
                               Runs, RunCount, AllocSize, DataSize);
}

/*
 * Build a fresh base FILE record for a new file: $STANDARD_INFORMATION +
 * $FILE_NAME (indexed) + a resident $DATA holding `data`. Pure memory work
 * (no fixup, no I/O); the caller stamps the fixup and writes it. Shared by the
 * kernel create path and the user-mode create test.
 */
/*
 * NTFS 1.x (the format of NT 3.1 - 4.0) keeps the file record's update
 * sequence array at 0x2A and has no record-number field; 3.x moved the array
 * to 0x30. Converts a record just built in the 3.x layout (attributes are
 * moved down, the header fields set) so that NT 3.x/4.0 and their CHKDSK see
 * a record laid out as they make them.
 */
void
NtfsRecordToV1(PVOID Buf, ULONG RecSize)
{
    PUCHAR p = (PUCHAR)Buf;
    PMFT_RECORD m = (PMFT_RECORD)Buf;
    ULONG oldFirst = m->FirstAttributeOffset, used = m->BytesInUse, i, shift;
    ULONG newFirst = (0x2A + (ULONG)m->Rhdr.UsaCount * 2 + 7) & ~7u;
    if (m->Rhdr.UsaOffset == 0x2A || newFirst >= oldFirst || used > RecSize || used < oldFirst) return;
    shift = oldFirst - newFirst;
    for (i = oldFirst; i < used; i++) p[i - shift] = p[i];
    for (i = used - shift; i < used; i++) p[i] = 0;
    m->BytesInUse = used - shift;
    m->FirstAttributeOffset = (USHORT)newFirst;
    m->Rhdr.UsaOffset = 0x2A;
    m->Align = 0;
    m->MftRecordNumber = 0;                 /* (inside the array in 1.x) */
}

void
NtfsBuildFileRecord(PVOID Buf, ULONG RecSize, ULONG SecSize,
                    ULONGLONG RecNo, USHORT Seq, ULONGLONG ParentRef,
                    const WCHAR *Name, ULONG NameLen, ULONGLONG FileTime,
                    const void *Data, ULONG DataLen)
{
    PUCHAR p = (PUCHAR)Buf;
    PMFT_RECORD m = (PMFT_RECORD)Buf;
    ULONG off, usaCount = RecSize / SecSize + 1, usaOff = 0x30, i;
    PATTRIBUTE_RECORD a;

    for (i = 0; i < RecSize; i++) p[i] = 0;
    m->Rhdr.Type = NTFS_FILE_SIGNATURE;
    m->Rhdr.UsaOffset = (USHORT)usaOff;
    m->Rhdr.UsaCount = (USHORT)usaCount;
    m->SequenceNumber = Seq;
    m->HardLinkCount = 1;
    m->FirstAttributeOffset = (USHORT)((usaOff + usaCount * 2 + 7) & ~7u);
    m->Flags = MFT_RECORD_IN_USE;
    m->BytesAllocated = RecSize;
    m->NextAttributeId = 0;
    m->MftRecordNumber = (ULONG)RecNo;
    off = m->FirstAttributeOffset;

    /* $STANDARD_INFORMATION */
    a = (PATTRIBUTE_RECORD)(p + off);
    a->Type = ATTR_STANDARD_INFORMATION;
    a->NonResident = 0; a->NameLength = 0; a->NameOffset = 0x18;
    a->AttributeId = m->NextAttributeId++;
    a->Form.Resident.ValueLength = 0x48; a->Form.Resident.ValueOffset = 0x18;
    a->Length = (0x18 + 0x48 + 7) & ~7u;
    { PSTANDARD_INFORMATION si = (PSTANDARD_INFORMATION)(p + off + 0x18);
      si->CreationTime = si->ChangeTime = si->MftChangeTime = si->LastAccessTime = FileTime;
      si->FileAttributes = 0x20; }                /* archive, as NT creates files */
    off += a->Length;

    /* $FILE_NAME (indexed) */
    a = (PATTRIBUTE_RECORD)(p + off);
    a->Type = ATTR_FILE_NAME;
    a->NonResident = 0; a->NameLength = 0; a->NameOffset = 0x18;
    a->AttributeId = m->NextAttributeId++;
    a->Form.Resident.IndexedFlag = 1;
    { ULONG vlen = sizeof(FILE_NAME_ATTR) + NameLen * 2;
      PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)(p + off + 0x18);
      a->Form.Resident.ValueLength = vlen; a->Form.Resident.ValueOffset = 0x18;
      a->Length = (0x18 + vlen + 7) & ~7u;
      for (i = 0; i < sizeof(FILE_NAME_ATTR); i++) ((PUCHAR)fn)[i] = 0;
      fn->ParentDirectory = ParentRef;
      fn->CreationTime = fn->ChangeTime = fn->MftChangeTime = fn->LastAccessTime = FileTime;
      fn->AllocatedSize = (DataLen + 7) & ~7u; fn->DataSize = DataLen;
      fn->FileAttributes = 0x20;
      fn->NameLength = (UCHAR)NameLen;
      fn->NameType = NtfsIsDos83Name(Name, NameLen) ? NTFS_FNAME_WIN32DOS : NTFS_FNAME_WIN32;
      for (i = 0; i < NameLen; i++) ((PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)))[i] = Name[i];
    }
    off += a->Length;

    /* $DATA (resident) */
    a = (PATTRIBUTE_RECORD)(p + off);
    a->Type = ATTR_DATA;
    a->NonResident = 0; a->NameLength = 0; a->NameOffset = 0x18;
    a->AttributeId = m->NextAttributeId++;
    a->Form.Resident.ValueLength = DataLen; a->Form.Resident.ValueOffset = 0x18;
    a->Length = (0x18 + DataLen + 7) & ~7u;
    for (i = 0; i < DataLen; i++) (p + off + 0x18)[i] = ((const UCHAR *)Data)[i];
    off += a->Length;

    *(PULONG)(p + off) = ATTR_END;
    off += 8;
    m->BytesInUse = off;
}

/* Update just the DataSize/InitializedSize of the unnamed non-resident $DATA
 * (used when a write grows the valid length within the current allocation). */
BOOLEAN
NtfsSetDataSize(PMFT_RECORD m, ULONGLONG DataSize)
{
    PATTRIBUTE_RECORD a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    if (a == NULL || !a->NonResident) return FALSE;
    a->Form.NonResident.DataSize = DataSize;
    a->Form.NonResident.InitializedSize = DataSize;
    return TRUE;
}

/*
 * Convert the unnamed resident $DATA of record m into a non-resident attribute
 * backed by a single run (Lcn, Clusters). The record is rewritten in place
 * (the attribute's following neighbours shift and BytesInUse updates). The
 * caller must copy the old resident bytes to the new clusters and set DataSize.
 * Pure memory work; shared by the kernel write path. FALSE if it won't fit.
 */
BOOLEAN
NtfsConvertDataToNonResident(PMFT_RECORD m, ULONG RecSize, ULONGLONG Lcn,
                             ULONGLONG Clusters, ULONGLONG AllocSize, ULONGLONG DataSize)
{
    PATTRIBUTE_RECORD a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    NTFS_RUN run;
    UCHAR mp[64];
    ULONG mpLen, mpOff = 0x40, oldLen, newLen, recUsed, attrOff, attrEnd, tail;
    LONG delta;
    PUCHAR ra;
    ULONG type; USHORT id;

    if (a == NULL || a->NonResident) return FALSE;
    run.Vcn = 0; run.Lcn = Lcn; run.Length = Clusters; run.Sparse = FALSE;
    if (!NtfsEncodeRuns(&run, 1, mp, sizeof(mp), &mpLen)) return FALSE;

    ra = (PUCHAR)a;
    oldLen = a->Length;
    newLen = (mpOff + mpLen + 7) & ~7u;
    delta = (LONG)newLen - (LONG)oldLen;
    recUsed = m->BytesInUse;
    if ((LONG)recUsed + delta > (LONG)RecSize) return FALSE;

    attrOff = (ULONG)(ra - (PUCHAR)m);
    attrEnd = attrOff + oldLen;
    tail = recUsed - attrEnd;
    if (delta != 0 && tail > 0) memmove(ra + newLen, ra + oldLen, tail);

    type = a->Type; id = a->AttributeId;
    RtlZeroMemory(ra, newLen);
    a->Type = type;
    a->Length = newLen;
    a->NonResident = 1;
    a->NameLength = 0;
    a->NameOffset = (USHORT)mpOff;
    a->Flags = 0;
    a->AttributeId = id;
    a->Form.NonResident.LowestVcn = 0;
    a->Form.NonResident.HighestVcn = Clusters - 1;
    a->Form.NonResident.MappingPairsOffset = (USHORT)mpOff;
    a->Form.NonResident.CompressionUnit = 0;
    a->Form.NonResident.AllocatedSize = AllocSize;
    a->Form.NonResident.DataSize = DataSize;
    a->Form.NonResident.InitializedSize = DataSize;
    RtlCopyMemory(ra + mpOff, mp, mpLen);
    m->BytesInUse = (ULONG)((LONG)recUsed + delta);
    return TRUE;
}

/*
 * Insert a prebuilt index entry into a SMALL (resident) $INDEX_ROOT $I30 of
 * record m, growing the attribute (and the record) to make room. EntryLen must
 * be 8-aligned. FALSE if it won't fit the record. For a directory whose index is
 * still resident in the root (no $INDEX_ALLOCATION).
 */
BOOLEAN
NtfsIndexRootInsert(PMFT_RECORD m, ULONG RecSize, const void *Entry, ULONG EntryLen)
{
    static const WCHAR i30[4] = { '$','I','3','0' };
    PATTRIBUTE_RECORD a = NtfsFindAttribute(m, ATTR_INDEX_ROOT, i30, 4);
    PINDEX_ROOT r;
    PUCHAR ra;
    ULONG oldLen, recUsed, attrEnd, tail, cap, hdrOff;

    if (a == NULL || a->NonResident) return FALSE;
    ra = (PUCHAR)a;
    oldLen = a->Length;
    recUsed = m->BytesInUse;
    if ((LONG)recUsed + (LONG)EntryLen > (LONG)RecSize) return FALSE;

    /* make room: shift following attributes up by EntryLen */
    attrEnd = (ULONG)(ra - (PUCHAR)m) + oldLen;
    tail = recUsed - attrEnd;
    if (tail > 0) memmove(ra + oldLen + EntryLen, ra + oldLen, tail);

    a->Length = oldLen + EntryLen;
    a->Form.Resident.ValueLength += EntryLen;
    m->BytesInUse = recUsed + EntryLen;

    r = (PINDEX_ROOT)((PUCHAR)a + a->Form.Resident.ValueOffset);
    hdrOff = (ULONG)((PUCHAR)&r->Header - (PUCHAR)r);     /* 0x10 */
    r->Header.AllocatedLength += EntryLen;
    cap = a->Form.Resident.ValueLength - hdrOff;
    return NtfsIndexInsert(&r->Header, cap, Entry, EntryLen);
}

/*
 * Remove a $FILE_NAME key from a SMALL (resident) $INDEX_ROOT and shrink the
 * attribute/record. FALSE if the name isn't present.
 */
BOOLEAN
NtfsIndexRootRemove(PMFT_RECORD m, ULONG RecSize, const WCHAR *Name, ULONG NameLen)
{
    static const WCHAR i30[4] = { '$','I','3','0' };
    PATTRIBUTE_RECORD a = NtfsFindAttribute(m, ATTR_INDEX_ROOT, i30, 4);
    PINDEX_ROOT r;
    PUCHAR ra;
    ULONG before, removed, recUsed, attrEnd, tail;

    if (a == NULL || a->NonResident) return FALSE;
    ra = (PUCHAR)a;
    r = (PINDEX_ROOT)(ra + a->Form.Resident.ValueOffset);
    before = r->Header.IndexLength;
    if (!NtfsIndexRemove(&r->Header, Name, NameLen)) return FALSE;
    removed = before - r->Header.IndexLength;      /* bytes freed (8-aligned) */
    r->Header.AllocatedLength -= removed;

    recUsed = m->BytesInUse;
    attrEnd = (ULONG)(ra - (PUCHAR)m) + a->Length;
    tail = recUsed - attrEnd;
    if (tail > 0) memmove(ra + a->Length - removed, ra + a->Length, tail);
    a->Length -= removed;
    a->Form.Resident.ValueLength -= removed;
    m->BytesInUse = recUsed - removed;
    (void)RecSize;
    return TRUE;
}

/*
 * Resize the value of a RESIDENT attribute `a` in record m to NewLen bytes,
 * shifting the following attributes and updating BytesInUse. Growing zero-fills
 * the new tail. FALSE if it would overflow the record.
 */
BOOLEAN
NtfsResizeResidentValue(PMFT_RECORD m, ULONG RecSize, PATTRIBUTE_RECORD a, ULONG NewLen)
{
    PUCHAR ra = (PUCHAR)a;
    ULONG oldLen, newAttrLen, recUsed, attrEnd, tail, valOff, oldVal;
    LONG delta;

    if (a == NULL || a->NonResident) return FALSE;
    valOff = a->Form.Resident.ValueOffset;
    oldVal = a->Form.Resident.ValueLength;
    oldLen = a->Length;
    newAttrLen = (valOff + NewLen + 7) & ~7u;
    delta = (LONG)newAttrLen - (LONG)oldLen;
    recUsed = m->BytesInUse;
    if ((LONG)recUsed + delta > (LONG)RecSize) return FALSE;

    attrEnd = (ULONG)(ra - (PUCHAR)m) + oldLen;
    tail = recUsed - attrEnd;
    if (delta != 0 && tail > 0) memmove(ra + newAttrLen, ra + oldLen, tail);
    if (NewLen > oldVal) RtlZeroMemory(ra + valOff + oldVal, NewLen - oldVal);
    /* clear any slack between the value end and the new attribute end */
    if (newAttrLen > valOff + NewLen) RtlZeroMemory(ra + valOff + NewLen, newAttrLen - (valOff + NewLen));
    a->Length = newAttrLen;
    a->Form.Resident.ValueLength = NewLen;
    m->BytesInUse = (ULONG)((LONG)recUsed + delta);
    return TRUE;
}

/* Initialise an empty INDX leaf node in Buf (ibs bytes) for block VCN. */
void
NtfsInitIndexLeaf(PUCHAR Buf, ULONG ibs, ULONG SecSize, ULONGLONG Vcn)
{
    PINDEX_BLOCK ib = (PINDEX_BLOCK)Buf;
    ULONG usaCount = ibs / SecSize + 1;
    ULONG entriesOff = (0x18 + 0x10 + usaCount * 2 + 7) & ~7u;
    PINDEX_ENTRY e;
    RtlZeroMemory(Buf, ibs);
    ib->Rhdr.Type = NTFS_INDX_SIGNATURE;
    ib->Rhdr.UsaOffset = 0x28;
    ib->Rhdr.UsaCount = (USHORT)usaCount;
    ib->IndexBlockVcn = Vcn;
    ib->Header.EntriesOffset = entriesOff - 0x18;
    ib->Header.IndexLength = (entriesOff - 0x18) + 0x10;
    ib->Header.AllocatedLength = ibs - 0x18;
    ib->Header.Flags = 0;
    e = (PINDEX_ENTRY)(Buf + entriesOff);
    e->Length = 0x10; e->KeyLength = 0; e->Flags = INDEX_ENTRY_END;
}

/*
 * Split a full directory leaf (OldLeaf) that must take NewEntry: fill LowBuf and
 * NewBuf (freshly initialised leaf blocks for leafVcn and newVcn), choose the
 * median, and promote it into the root's $INDEX_ROOT with a subnode pointer to
 * the low leaf, repointing the old separator at the new (high) leaf. Pure memory
 * work; the caller handles block allocation and the three writes. FALSE if the
 * root cannot take the promoted entry (tree would need another level).
 */
BOOLEAN
NtfsPromoteSplit(PMFT_RECORD root, ULONG RecSize, PUCHAR OldLeaf, PUCHAR LowBuf,
                 PUCHAR NewBuf, ULONG ibs, ULONG SecSize, ULONGLONG leafVcn,
                 ULONGLONG newVcn, const void *NewEntry, ULONG EntLen)
{
    static const WCHAR i30[4] = { '$','I','3','0' };
    PINDEX_BLOCK oldIb = (PINDEX_BLOCK)OldLeaf;
    PINDEX_ENTRY e, p;
    PATTRIBUTE_RECORD ir;
    PINDEX_ROOT r;
    const INDEX_ENTRY *ne = (const INDEX_ENTRY *)NewEntry;
    const WCHAR *nname = (const WCHAR *)((const UCHAR *)ne + sizeof(INDEX_ENTRY) + sizeof(FILE_NAME_ATTR));
    ULONG nlen = ((const FILE_NAME_ATTR *)((const UCHAR *)ne + sizeof(INDEX_ENTRY)))->NameLength;
    ULONG total = 0, running = 0, half, lowCap = ibs - 0x18, newCap = ibs - 0x18, medLen = 0;
    BOOLEAN inserted = FALSE, pastMedian = FALSE;
    UCHAR med[600], med2[600];
    ULONG medWithVcn;

    NtfsInitIndexLeaf(LowBuf, ibs, SecSize, leafVcn);
    NtfsInitIndexLeaf(NewBuf, ibs, SecSize, newVcn);

    for (e = NtfsFirstIndexEntry(&oldIb->Header); !(e->Flags & INDEX_ENTRY_END); e = NtfsNextIndexEntry(e))
        total += e->Length;
    total += EntLen;
    half = total / 2;

    #define SPLIT_EMIT(ent,len) do { \
        if (!pastMedian && running >= half) { RtlCopyMemory(med,(ent),(len)); medLen=(len); pastMedian=TRUE; } \
        else if (!pastMedian) NtfsIndexInsert(&((PINDEX_BLOCK)LowBuf)->Header, lowCap, (ent),(len)); \
        else NtfsIndexInsert(&((PINDEX_BLOCK)NewBuf)->Header, newCap, (ent),(len)); \
        running += (len); } while (0)
    for (e = NtfsFirstIndexEntry(&oldIb->Header); !(e->Flags & INDEX_ENTRY_END); e = NtfsNextIndexEntry(e)) {
        const FILE_NAME_ATTR *efn = (const FILE_NAME_ATTR *)((PUCHAR)e + sizeof(INDEX_ENTRY));
        const WCHAR *en = (const WCHAR *)((const UCHAR *)efn + sizeof(FILE_NAME_ATTR));
        if (!inserted && NtfsCollateName(nname, nlen, en, efn->NameLength) <= 0) {
            SPLIT_EMIT(NewEntry, EntLen); inserted = TRUE;
        }
        SPLIT_EMIT(e, e->Length);
    }
    if (!inserted) SPLIT_EMIT(NewEntry, EntLen);
    #undef SPLIT_EMIT
    if (medLen == 0) return FALSE;

    /* promote median into the root */
    ir = NtfsFindAttribute(root, ATTR_INDEX_ROOT, i30, 4);
    if (!ir) return FALSE;
    r = (PINDEX_ROOT)NtfsResidentValue(ir);
    p = NULL;
    for (e = NtfsFirstIndexEntry(&r->Header); ; e = NtfsNextIndexEntry(e)) {
        if ((e->Flags & INDEX_ENTRY_HAS_SUBNODE) && NtfsIndexSubnodeVcn(e) == leafVcn) { p = e; break; }
        if (e->Flags & INDEX_ENTRY_END) break;
    }
    if (p == NULL) return FALSE;
    *(ULONGLONG *)((PUCHAR)p + p->Length - 8) = newVcn;     /* old sep -> high leaf */

    RtlCopyMemory(med2, med, medLen);
    medWithVcn = (medLen + 8 + 7) & ~7u;
    ((PINDEX_ENTRY)med2)->Flags |= INDEX_ENTRY_HAS_SUBNODE;
    ((PINDEX_ENTRY)med2)->Length = (USHORT)medWithVcn;
    *(ULONGLONG *)(med2 + medWithVcn - 8) = leafVcn;        /* median -> low leaf */
    return NtfsIndexRootInsert(root, RecSize, med2, medWithVcn);
}

BOOLEAN
NtfsMapVcn(PNTFS_RUN Runs, ULONG Count, ULONGLONG Vcn,
           PULONGLONG Lcn, PULONGLONG RunClustersLeft, PBOOLEAN Sparse)
{
    ULONG i;

    *Sparse = FALSE;
    for (i = 0; i < Count; i++) {
        if (Vcn >= Runs[i].Vcn && Vcn < Runs[i].Vcn + Runs[i].Length) {
            ULONGLONG delta = Vcn - Runs[i].Vcn;
            if (Runs[i].Sparse) {
                *Sparse = TRUE;
                *RunClustersLeft = Runs[i].Length - delta;
                return FALSE;
            }
            *Lcn = Runs[i].Lcn + delta;
            *RunClustersLeft = Runs[i].Length - delta;
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */

extern const WCHAR *NtfsUpcaseTable;
static WCHAR UpChar(WCHAR c)
{
    if (NtfsUpcaseTable) return NtfsUpcaseTable[c];
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 0x20) : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (WCHAR)(c - 0x20);  /* Latin-1 */
    if (c == 0xFF) return 0x178;
    if (c == 0x131) return 'I';                                         /* dotless i */
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))       /* Latin Ext-A */
        return (c & 1) ? (WCHAR)(c - 1) : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1) ? c : (WCHAR)(c - 1);
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return (WCHAR)(c - 0x20); /* Greek */
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x430 && c <= 0x44F) return (WCHAR)(c - 0x20);             /* Cyrillic */
    if (c >= 0x450 && c <= 0x45F) return (WCHAR)(c - 0x50);
    return c;
}

int
NtfsCollateName(const WCHAR *A, ULONG ALen, const WCHAR *B, ULONG BLen)
{
    ULONG i, n = (ALen < BLen) ? ALen : BLen;
    for (i = 0; i < n; i++) {
        WCHAR ca = UpChar(A[i]), cb = UpChar(B[i]);
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
    }
    if (ALen == BLen) return 0;
    return (ALen < BLen) ? -1 : 1;
}

/* Key (FILE_NAME) of an index entry, and its name length. */
static PFILE_NAME_ATTR EntryName(PINDEX_ENTRY e, PWCHAR *name, ULONG *len)
{
    PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)((PUCHAR)e + sizeof(INDEX_ENTRY));
    *name = (PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR));
    *len = fn->NameLength;
    return fn;
}

BOOLEAN
NtfsIndexInsert(PINDEX_HEADER Header, ULONG Capacity, const void *Entry, ULONG EntryLen)
{
    PINDEX_ENTRY e;
    PUCHAR at;
    PFILE_NAME_ATTR nfn = (PFILE_NAME_ATTR)((PUCHAR)Entry + sizeof(INDEX_ENTRY));
    PWCHAR nname = (PWCHAR)((PUCHAR)nfn + sizeof(FILE_NAME_ATTR));
    ULONG nlen = nfn->NameLength;

    if (Header->IndexLength + EntryLen > Capacity) {
        return FALSE;                    /* would overflow the node */
    }

    /* find the first entry that collates >= the new key (or the end marker) */
    for (e = NtfsFirstIndexEntry(Header); ; e = (PINDEX_ENTRY)((PUCHAR)e + e->Length)) {
        if (e->Flags & INDEX_ENTRY_END) {
            break;
        }
        {
            PWCHAR en; ULONG el; EntryName(e, &en, &el);
            if (NtfsCollateName(nname, nlen, en, el) <= 0) {
                break;
            }
        }
        if (e->Length == 0) return FALSE;
    }

    /* insert before e: shift the tail (e..end of used) up by EntryLen */
    at = (PUCHAR)e;
    {
        PUCHAR usedEnd = (PUCHAR)Header + Header->IndexLength;
        memmove(at + EntryLen, at, (ULONG)(usedEnd - at));
        memcpy(at, Entry, EntryLen);
    }
    Header->IndexLength += EntryLen;
    return TRUE;
}

BOOLEAN
NtfsIndexRemove(PINDEX_HEADER Header, const WCHAR *Name, ULONG NameLen)
{
    PINDEX_ENTRY e;

    for (e = NtfsFirstIndexEntry(Header); e && !(e->Flags & INDEX_ENTRY_END); ) {
        PWCHAR en; ULONG el; EntryName(e, &en, &el);
        if (el == NameLen && NtfsCollateName(Name, NameLen, en, el) == 0) {
            PUCHAR at = (PUCHAR)e;
            PUCHAR usedEnd = (PUCHAR)Header + Header->IndexLength;
            ULONG elen = e->Length;
            memmove(at, at + elen, (ULONG)(usedEnd - (at + elen)));
            Header->IndexLength -= elen;
            return TRUE;
        }
        if (e->Length == 0) break;
        e = (PINDEX_ENTRY)((PUCHAR)e + e->Length);
    }
    return FALSE;
}

PINDEX_ENTRY
NtfsFirstIndexEntry(PINDEX_HEADER Header)
{
    return (PINDEX_ENTRY)((PUCHAR)Header + Header->EntriesOffset);
}

PINDEX_ENTRY
NtfsNextIndexEntry(PINDEX_ENTRY Entry)
{
    if ((Entry->Flags & INDEX_ENTRY_END) || Entry->Length == 0) {
        return NULL;
    }
    return (PINDEX_ENTRY)((PUCHAR)Entry + Entry->Length);
}

ULONGLONG
NtfsIndexSubnodeVcn(PINDEX_ENTRY Entry)
{
    /* child VCN is the last 8 bytes of the entry */
    return *(PULONGLONG)((PUCHAR)Entry + Entry->Length - 8);
}

/* ------------------------------------------------------------------ */
/* LZNT1 decompression (NTFS transparent compression - Win NT..11)     */
/* ------------------------------------------------------------------ */

/*
 * Decompress one 4 KB LZNT1 chunk body into Out. 'Compressed' comes from the
 * chunk header. Returns bytes produced (<= OutMax, normally 4096).
 */
static ULONG
NtfsLznt1Chunk(const UCHAR *In, ULONG InLen, UCHAR *Out, ULONG OutMax, int Compressed)
{
    ULONG ip = 0, op = 0;

    if (!Compressed) {
        ULONG n = InLen; if (n > OutMax) n = OutMax;
        RtlCopyMemory(Out, In, n);
        return n;
    }
    while (ip < InLen && op < OutMax) {
        UCHAR flags = In[ip++];
        int b;
        for (b = 0; b < 8 && op < OutMax; b++) {
            if (ip >= InLen) break;
            if (!(flags & (1 << b))) {
                Out[op++] = In[ip++];               /* literal byte */
            } else {
                USHORT pair;
                ULONG index, lmask, dshift, length, delta, k;
                if (ip + 1 >= InLen) { ip = InLen; break; }
                pair = (USHORT)(In[ip] | (In[ip + 1] << 8));
                ip += 2;
                index = op - 1;                     /* last written offset */
                lmask = 0xFFF; dshift = 12;
                while (index >= 0x10) { lmask >>= 1; dshift--; index >>= 1; }
                length = (ULONG)((pair & lmask) + 3);
                delta  = (ULONG)((pair >> dshift) + 1);
                if (delta > op) return op;          /* corrupt back-ref */
                for (k = 0; k < length && op < OutMax; k++) {
                    Out[op] = Out[op - delta];
                    op++;
                }
            }
        }
    }
    return op;
}

/*
 * Decompress a whole compression unit: the compressed bytes In[0..InLen) (the
 * mapped clusters of the unit) expand into Out[0..OutMax) (the unit size, e.g.
 * 64 KB). The remainder of Out is zero-filled. Returns OutMax.
 */
ULONG
NtfsLznt1Unit(const UCHAR *In, ULONG InLen, UCHAR *Out, ULONG OutMax)
{
    ULONG ip = 0, op = 0;

    while (ip + 2 <= InLen && op < OutMax) {
        USHORT hdr = (USHORT)(In[ip] | (In[ip + 1] << 8));
        ULONG csize, room, produced;
        int compressed;
        if (hdr == 0) break;                        /* end marker */
        csize = (ULONG)((hdr & 0x0FFF) + 1);        /* chunk body bytes */
        compressed = (hdr & 0x8000) != 0;
        ip += 2;
        if (ip + csize > InLen) csize = InLen - ip;
        room = OutMax - op; if (room > 4096) room = 4096;
        produced = NtfsLznt1Chunk(In + ip, csize, Out + op, room, compressed);
        ip += csize;
        op += produced;
        if (produced == 0) break;
    }
    if (op < OutMax) {
        RtlZeroMemory(Out + op, OutMax - op);
    }
    return OutMax;
}

/* ------------------------------------------------------------------ */
/* Directory records, attribute insertion, SMALL->LARGE index growth   */
/* ------------------------------------------------------------------ */

/*
 * Optional volume $UpCase table (65536 WCHARs). When set (the kernel driver
 * loads it at mount), name collation uses it exactly like Windows NTFS; when
 * NULL, a built-in table covering ASCII, Latin-1, Latin Extended-A (Turkish
 * letters included), Greek and Cyrillic is used.
 */
const WCHAR *NtfsUpcaseTable = NULL;

/* Build a fresh base FILE record for a new, empty directory: SI + indexed
 * FILE_NAME (dir flag) + an empty SMALL $INDEX_ROOT $I30. */
void
NtfsBuildDirRecord(PVOID Buf, ULONG RecSize, ULONG SecSize,
                   ULONGLONG RecNo, USHORT Seq, ULONGLONG ParentRef,
                   const WCHAR *Name, ULONG NameLen, ULONGLONG FileTime,
                   ULONG IndexBlockSize, ULONG ClusterSize)
{
    PUCHAR p = (PUCHAR)Buf;
    PMFT_RECORD m = (PMFT_RECORD)Buf;
    PATTRIBUTE_RECORD a, d;
    PFILE_NAME_ATTR fn;
    PINDEX_ROOT r;
    PINDEX_ENTRY e;
    ULONG off;
    USHORT id;

    NtfsBuildFileRecord(Buf, RecSize, SecSize, RecNo, Seq, ParentRef,
                        Name, NameLen, FileTime, "", 0);
    m->Flags |= MFT_RECORD_IS_DIRECTORY;
    a = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
    if (a) ((PSTANDARD_INFORMATION)NtfsResidentValue(a))->FileAttributes = NTFS_FILE_NAME_INDEX_PRESENT;
    a = NtfsFindAttribute(m, ATTR_FILE_NAME, NULL, 0);
    fn = (PFILE_NAME_ATTR)NtfsResidentValue(a);
    fn->FileAttributes = NTFS_FILE_NAME_INDEX_PRESENT;
    fn->AllocatedSize = 0; fn->DataSize = 0;

    /* replace the (empty, last) $DATA with $INDEX_ROOT "$I30" */
    d = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    off = (ULONG)((PUCHAR)d - p);
    id = d->AttributeId;
    RtlZeroMemory(p + off, RecSize - off);
    a = (PATTRIBUTE_RECORD)(p + off);
    a->Type = ATTR_INDEX_ROOT;
    a->NonResident = 0; a->NameLength = 4; a->NameOffset = 0x18;
    a->AttributeId = id;
    a->Form.Resident.ValueOffset = 0x20;
    a->Form.Resident.ValueLength = 0x30;
    a->Length = 0x50;
    ((PWCHAR)(p + off + 0x18))[0] = '$'; ((PWCHAR)(p + off + 0x18))[1] = 'I';
    ((PWCHAR)(p + off + 0x18))[2] = '3'; ((PWCHAR)(p + off + 0x18))[3] = '0';
    r = (PINDEX_ROOT)(p + off + 0x20);
    r->AttributeType = ATTR_FILE_NAME;
    r->CollationRule = 1;                          /* COLLATION_FILE_NAME */
    r->IndexBlockSize = IndexBlockSize;
    r->ClustersPerIndexBlock = (UCHAR)((IndexBlockSize >= ClusterSize) ?
                               IndexBlockSize / ClusterSize : IndexBlockSize / 512);
    r->Header.EntriesOffset = 0x10;
    r->Header.IndexLength = 0x20;
    r->Header.AllocatedLength = 0x20;
    r->Header.Flags = 0;
    e = (PINDEX_ENTRY)((PUCHAR)&r->Header + 0x10);
    e->Length = 0x10; e->KeyLength = 0; e->Flags = INDEX_ENTRY_END;
    off += a->Length;
    *(PULONG)(p + off) = ATTR_END;
    m->BytesInUse = off + 8;
}

/* Insert a prebuilt attribute (Len bytes, 8-aligned) into record m keeping the
 * attributes sorted by type; gives it a fresh AttributeId. FALSE if no room. */
BOOLEAN
NtfsInsertAttribute(PMFT_RECORD m, ULONG RecSize, const void *Attr, ULONG Len)
{
    return NtfsInsertAttributeEx(m, RecSize, Attr, Len, FALSE);
}

/* As NtfsInsertAttribute; First places it before attributes of the same type
 * (a DOS $FILE_NAME precedes its Win32 name, as NT writes them). */
BOOLEAN
NtfsInsertAttributeEx(PMFT_RECORD m, ULONG RecSize, const void *Attr, ULONG Len, BOOLEAN First)
{
    PUCHAR p = (PUCHAR)m;
    const ATTRIBUTE_RECORD *na = (const ATTRIBUTE_RECORD *)Attr;
    ULONG off = m->FirstAttributeOffset, used = m->BytesInUse;
    PATTRIBUTE_RECORD a;

    if (used + Len > RecSize) return FALSE;
    for (;;) {
        a = (PATTRIBUTE_RECORD)(p + off);
        if (a->Type == ATTR_END || a->Type > na->Type || a->Length == 0) break;
        if (First && a->Type == na->Type) break;
        off += a->Length;
        if (off >= used) return FALSE;
    }
    memmove(p + off + Len, p + off, used - off);
    memcpy(p + off, Attr, Len);
    ((PATTRIBUTE_RECORD)(p + off))->AttributeId = m->NextAttributeId++;
    m->BytesInUse = used + Len;
    return TRUE;
}

/* Find an index entry by (case-insensitive) name in one node. */
PINDEX_ENTRY
NtfsFindIndexEntry(PINDEX_HEADER Header, const WCHAR *Name, ULONG NameLen)
{
    PINDEX_ENTRY e;
    for (e = NtfsFirstIndexEntry(Header); e && !(e->Flags & INDEX_ENTRY_END); ) {
        PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)((PUCHAR)e + sizeof(INDEX_ENTRY));
        if (fn->NameLength == NameLen &&
            NtfsCollateName(Name, NameLen, (PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)), NameLen) == 0)
            return e;
        if (e->Length == 0) break;
        e = (PINDEX_ENTRY)((PUCHAR)e + e->Length);
    }
    return NULL;
}

/*
 * Grow a SMALL directory index into a LARGE one: every root entry moves into a
 * fresh INDX leaf (Leaf, block VCN 0, ibs bytes), the root keeps only an END
 * entry pointing at VCN 0, and $INDEX_ALLOCATION "$I30" (one run Lcn/Clusters,
 * AllocBytes) plus a resident $BITMAP "$I30" (bit 0 set) are added. Pure memory
 * work; the caller writes Leaf (with fixup) and the record. FALSE if no room.
 */
BOOLEAN
NtfsPromoteRootToLarge(PMFT_RECORD m, ULONG RecSize, PUCHAR Leaf, ULONG ibs,
                       ULONG SecSize, ULONGLONG Lcn, ULONGLONG Clusters,
                       ULONGLONG AllocBytes)
{
    static const WCHAR i30[4] = { '$','I','3','0' };
    PATTRIBUTE_RECORD ir;
    PINDEX_ROOT r;
    PINDEX_ENTRY e;
    PINDEX_BLOCK lb = (PINDEX_BLOCK)Leaf;
    ULONG cap = ibs - 0x18, i;
    UCHAR ab[0x60];
    NTFS_RUN run;
    ULONG mpLen;

    ir = NtfsFindAttribute(m, ATTR_INDEX_ROOT, i30, 4);
    if (ir == NULL || ir->NonResident) return FALSE;
    r = (PINDEX_ROOT)NtfsResidentValue(ir);
    if (r->Header.Flags & 1) return FALSE;                 /* already large */
    if (NtfsFindAttribute(m, ATTR_INDEX_ALLOCATION, i30, 4)) return FALSE;

    /* 1. move the entries into the new leaf */
    NtfsInitIndexLeaf(Leaf, ibs, SecSize, 0);
    for (e = NtfsFirstIndexEntry(&r->Header); e && !(e->Flags & INDEX_ENTRY_END); e = NtfsNextIndexEntry(e))
        if (!NtfsIndexInsert(&lb->Header, cap, e, e->Length)) return FALSE;

    /* 2. root: just an END entry with a subnode pointer to VCN 0 */
    if (!NtfsResizeResidentValue(m, RecSize, ir, 0x10 + 0x10 + 0x18)) return FALSE;
    r = (PINDEX_ROOT)NtfsResidentValue(ir);
    r->Header.EntriesOffset = 0x10;
    r->Header.IndexLength = 0x10 + 0x18;
    r->Header.AllocatedLength = 0x10 + 0x18;
    r->Header.Flags = 1;
    e = (PINDEX_ENTRY)((PUCHAR)&r->Header + 0x10);
    RtlZeroMemory(e, 0x18);
    e->Length = 0x18; e->KeyLength = 0; e->Flags = INDEX_ENTRY_END | INDEX_ENTRY_HAS_SUBNODE;
    *(ULONGLONG *)((PUCHAR)e + 0x10) = 0;

    /* 3. $INDEX_ALLOCATION "$I30" (non-resident, one run) */
    RtlZeroMemory(ab, sizeof(ab));
    run.Vcn = 0; run.Lcn = Lcn; run.Length = Clusters; run.Sparse = FALSE;
    if (!NtfsEncodeRuns(&run, 1, ab + 0x48, sizeof(ab) - 0x48, &mpLen)) return FALSE;
    {
        PATTRIBUTE_RECORD a = (PATTRIBUTE_RECORD)ab;
        a->Type = ATTR_INDEX_ALLOCATION;
        a->NonResident = 1; a->NameLength = 4; a->NameOffset = 0x40;
        a->Form.NonResident.LowestVcn = 0;
        a->Form.NonResident.HighestVcn = Clusters - 1;
        a->Form.NonResident.MappingPairsOffset = 0x48;
        a->Form.NonResident.AllocatedSize = AllocBytes;
        a->Form.NonResident.DataSize = AllocBytes;
        a->Form.NonResident.InitializedSize = AllocBytes;
        for (i = 0; i < 4; i++) ((PWCHAR)(ab + 0x40))[i] = i30[i];
        a->Length = (0x48 + mpLen + 7) & ~7u;
        if (!NtfsInsertAttribute(m, RecSize, ab, a->Length)) return FALSE;
    }
    /* 4. $BITMAP "$I30" (resident, 8 bytes, block 0 in use) */
    RtlZeroMemory(ab, sizeof(ab));
    {
        PATTRIBUTE_RECORD a = (PATTRIBUTE_RECORD)ab;
        a->Type = ATTR_BITMAP;
        a->NonResident = 0; a->NameLength = 4; a->NameOffset = 0x18;
        a->Form.Resident.ValueOffset = 0x20;
        a->Form.Resident.ValueLength = 8;
        for (i = 0; i < 4; i++) ((PWCHAR)(ab + 0x18))[i] = i30[i];
        ab[0x20] = 1;
        a->Length = 0x28;
        if (!NtfsInsertAttribute(m, RecSize, ab, a->Length)) return FALSE;
    }
    return TRUE;
}

/* TRUE if Name is a legal DOS 8.3 name (so one $FILE_NAME can serve as both
 * the Win32 and the DOS name); otherwise the name is stored Win32-only. */
BOOLEAN
NtfsIsDos83Name(const WCHAR *Name, ULONG Len)
{
    ULONG i, base = 0, ext = 0, dots = 0;
    if (Len == 0 || Len > 12) return FALSE;
    if (Name[0] == '.') return FALSE;
    for (i = 0; i < Len; i++) {
        WCHAR c = Name[i];
        if (c == '.') { if (++dots > 1) return FALSE; continue; }
        if (c <= 0x20 || c >= 0x7F) return FALSE;
        if (c == '"' || c == '*' || c == '+' || c == ',' || c == '/' || c == ':' ||
            c == ';' || c == '<' || c == '=' || c == '>' || c == '?' || c == '[' ||
            c == '\\' || c == ']' || c == '|')
            return FALSE;
        if (dots) ext++; else base++;
    }
    if (base == 0 || base > 8 || ext > 3) return FALSE;
    if (dots && ext == 0) return FALSE;              /* trailing dot */
    return TRUE;
}
