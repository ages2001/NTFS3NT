/*
 * NTFS3NT - directory index ($I30) B-tree maintenance, any depth.
 *
 * A directory's names live in a B-tree: the root node is the resident
 * $INDEX_ROOT inside the directory's MFT record, every other node is an INDX
 * block in $INDEX_ALLOCATION (in use per the $I30 $BITMAP). Entries are kept
 * in collation order; an entry with a subnode points at the node holding the
 * names that sort before it, and each node's END entry points at the node
 * holding the names after its last key. All leaves are at the same depth.
 *
 *   insert : descend to the leaf; a full node is split at its median, which
 *            moves up into the parent (recursively); a full root is pushed
 *            down into a new block, so the tree grows by one level.
 *   delete : a leaf entry is removed; an internal entry is replaced by its
 *            in-order predecessor (the last key of the rightmost leaf of its
 *            subtree). A node left with no keys is merged with a sibling
 *            through the parent separator, or refilled by a rotation from the
 *            sibling; an emptied root pulls its only child up, and a root
 *            that is a leaf again drops $INDEX_ALLOCATION (SMALL index).
 *   lookup : a plain descent, O(depth) blocks.
 *
 * Every operation works on a small cache of nodes and writes the touched
 * blocks and the directory record once at the end. Callers hold the volume
 * meta lock.
 */

#ifdef NTFS_IDX_TEST
#include "ntfsidxtest.h"      /* user-mode harness: test/ntfsbtree.c */
#else
#define NTFS_KERNEL
#include "ntfs.h"
#endif

static const WCHAR I30[4] = { '$', 'I', '3', '0' };

#define ROOTH      (-1L)        /* handle of the root node */
#define MAX_NODES  48
#define MAX_DEPTH  16
#define ENT_MAX    0x260        /* largest index entry (255-char name + subnode) */
#define ROOT_SLACK 0x80         /* record bytes kept free when the root grows */

typedef struct _TNODE {
    ULONGLONG Vcn;
    PUCHAR    Buf;
    BOOLEAN   Dirty;
    BOOLEAN   Freed;
} TNODE;

typedef struct _TREE {
    PNTFS_VCB Vcb;
    ULONGLONG Dir;
    PUCHAR    Rec;              /* the directory's MFT record */
    BOOLEAN   RecDirty;
    PNTFS_RUN Runs;             /* $INDEX_ALLOCATION runs */
    ULONG     Rc;
    ULONG     Ibs, Rs, VcnBytes, VcnPerBlk;
    PUCHAR    Bmp;              /* non-resident $BITMAP copy (else NULL) */
    ULONG     BmpLen;
    BOOLEAN   BmpDirty;
    TNODE     N[MAX_NODES];
    ULONG     Nn;
    BOOLEAN   Failed;
} TREE, *PTREE;

/* ---- tree context --------------------------------------------------------- */

static PMFT_RECORD M(PTREE t) { return (PMFT_RECORD)t->Rec; }

static PATTRIBUTE_RECORD
RootAttr(PTREE t)
{
    PATTRIBUTE_RECORD a = NtfsFindAttribute(M(t), ATTR_INDEX_ROOT, I30, 4);
    return (a && !a->NonResident) ? a : NULL;
}

static PINDEX_HEADER
Hdr(PTREE t, LONG h)
{
    if (h == ROOTH) return &((PINDEX_ROOT)NtfsResidentValue(RootAttr(t)))->Header;
    return &((PINDEX_BLOCK)t->N[h].Buf)->Header;
}

static ULONGLONG NodeVcn(PTREE t, LONG h) { return t->N[h].Vcn; }
static BOOLEAN   IsInternalNode(PTREE t, LONG h) { return (Hdr(t, h)->Flags & 1) != 0; }

static VOID
Touch(PTREE t, LONG h)
{
    if (h == ROOTH) t->RecDirty = TRUE; else t->N[h].Dirty = TRUE;
}

static VOID
ReloadRuns(PTREE t)
{
    PATTRIBUTE_RECORD ia = NtfsFindAttribute(M(t), ATTR_INDEX_ALLOCATION, I30, 4);
    t->Rc = 0;
    if (ia && ia->NonResident && t->Runs &&
        !NtfsDecodeRuns(ia, t->Runs, NTFS_MAX_IDX_RUNS, &t->Rc))
        t->Rc = 0;
}

static BOOLEAN
TreeOpen(PTREE t, PNTFS_VCB Vcb, ULONGLONG Dir)
{
    RtlZeroMemory(t, sizeof(*t));
    t->Vcb = Vcb; t->Dir = Dir;
    t->Ibs = Vcb->Geo.IndexBlockSize; t->Rs = Vcb->Geo.MftRecordSize;
    t->VcnBytes = (t->Ibs >= Vcb->Geo.ClusterSize) ? Vcb->Geo.ClusterSize : 512;
    t->VcnPerBlk = t->Ibs / t->VcnBytes;
    t->Rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, t->Rs, NTFS_TAG_BUF);
    t->Runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!t->Rec || !t->Runs) return FALSE;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, Dir, t->Rec))) return FALSE;
    if (!(M(t)->Flags & MFT_RECORD_IS_DIRECTORY) || RootAttr(t) == NULL) return FALSE;
    ReloadRuns(t);
    return TRUE;
}

static VOID
TreeClose(PTREE t)
{
    ULONG i;
    for (i = 0; i < t->Nn; i++) if (t->N[i].Buf) ExFreePool(t->N[i].Buf);
    if (t->Rec) ExFreePool(t->Rec);
    if (t->Runs) ExFreePool(t->Runs);
    if (t->Bmp) ExFreePool(t->Bmp);
}

/* ---- INDX block I/O ------------------------------------------------------- */

static NTSTATUS
ReadBlk(PTREE t, ULONGLONG Vcn, PUCHAR Buf)
{
    NTSTATUS st = NtfsReadAttributeData(t->Vcb, t->Runs, t->Rc, Vcn * t->VcnBytes, t->Ibs, Buf);
    if (!NT_SUCCESS(st)) return st;
    if (((PNTFS_RECORD_HEADER)Buf)->Type != NTFS_INDX_SIGNATURE ||
        !NtfsApplyFixup(Buf, t->Ibs, t->Vcb->Geo.BytesPerSector))
        return STATUS_FILE_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS
WriteBlk(PTREE t, ULONGLONG Vcn, PUCHAR Buf)
{
    NTSTATUS st;
    NtfsSetFixup(Buf, t->Ibs, t->Vcb->Geo.BytesPerSector);
    st = NtfsWriteAttributeData(t->Vcb, t->Runs, t->Rc, Vcn * t->VcnBytes, t->Ibs, Buf);
    NtfsApplyFixup(Buf, t->Ibs, t->Vcb->Geo.BytesPerSector);
    return st;
}

/* node handle for block Vcn (read through the cache), or -1 on failure */
static LONG
Load(PTREE t, ULONGLONG Vcn)
{
    ULONG i;
    for (i = 0; i < t->Nn; i++)
        if (!t->N[i].Freed && t->N[i].Vcn == Vcn) return (LONG)i;
    if (t->Nn >= MAX_NODES || t->Rc == 0) { t->Failed = TRUE; return -2; }
    t->N[t->Nn].Buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, t->Ibs, NTFS_TAG_BUF);
    if (t->N[t->Nn].Buf == NULL || !NT_SUCCESS(ReadBlk(t, Vcn, t->N[t->Nn].Buf))) {
        if (t->N[t->Nn].Buf) { ExFreePool(t->N[t->Nn].Buf); t->N[t->Nn].Buf = NULL; }
        t->Failed = TRUE; return -2;
    }
    t->N[t->Nn].Vcn = Vcn; t->N[t->Nn].Dirty = FALSE; t->N[t->Nn].Freed = FALSE;
    return (LONG)t->Nn++;
}

/* ---- the $I30 block bitmap (resident or not) ------------------------------ */

static PUCHAR
BmpBytes(PTREE t, PULONG Len)
{
    PATTRIBUTE_RECORD a = NtfsFindAttribute(M(t), ATTR_BITMAP, I30, 4);
    if (a == NULL) { *Len = 0; return NULL; }
    if (!a->NonResident) { *Len = NtfsResidentValueLength(a); return (PUCHAR)NtfsResidentValue(a); }
    if (t->Bmp == NULL) {
        NTFS_RUN br[16]; ULONG bc;
        ULONG n = (ULONG)a->Form.NonResident.AllocatedSize;
        if (n == 0 || n > 0x10000 || !NtfsDecodeRuns(a, br, 16, &bc)) { *Len = 0; return NULL; }
        t->Bmp = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, n, NTFS_TAG_BUF);
        if (t->Bmp == NULL) { *Len = 0; return NULL; }
        RtlZeroMemory(t->Bmp, n);
        if (!NT_SUCCESS(NtfsReadAttributeData(t->Vcb, br, bc, 0,
                (ULONG)a->Form.NonResident.DataSize, t->Bmp))) {
            ExFreePool(t->Bmp); t->Bmp = NULL; *Len = 0; return NULL;
        }
        t->BmpLen = (ULONG)a->Form.NonResident.DataSize;
    }
    *Len = t->BmpLen;
    return t->Bmp;
}

/* make the bitmap cover block b (8-byte steps) */
static BOOLEAN
BmpCover(PTREE t, ULONG b)
{
    PATTRIBUTE_RECORD a = NtfsFindAttribute(M(t), ATTR_BITMAP, I30, 4);
    ULONG len, need = ((b >> 6) + 1) * 8;
    if (a == NULL) return FALSE;
    if (!a->NonResident) {
        if (NtfsResidentValueLength(a) >= need) return TRUE;
        if (!NtfsResizeResidentValue(M(t), t->Rs, a, need)) return FALSE;
        t->RecDirty = TRUE;
        return TRUE;
    }
    if (BmpBytes(t, &len) == NULL) return FALSE;
    if (len >= need) return TRUE;
    if (need > a->Form.NonResident.AllocatedSize) return FALSE;
    a->Form.NonResident.DataSize = need;
    a->Form.NonResident.InitializedSize = need;
    t->BmpLen = need; t->BmpDirty = TRUE; t->RecDirty = TRUE;
    return TRUE;
}

static BOOLEAN
BmpTest(PTREE t, ULONG b)
{
    ULONG len; PUCHAR p = BmpBytes(t, &len);
    return (p && (b >> 3) < len) ? ((p[b >> 3] >> (b & 7)) & 1) != 0 : FALSE;
}

static VOID
BmpSet(PTREE t, ULONG b, BOOLEAN v)
{
    ULONG len; PUCHAR p = BmpBytes(t, &len);
    if (p == NULL || (b >> 3) >= len) return;
    if (v) p[b >> 3] |= (UCHAR)(1u << (b & 7)); else p[b >> 3] &= (UCHAR)~(1u << (b & 7));
    if (t->Bmp) t->BmpDirty = TRUE; else t->RecDirty = TRUE;
}

/* ---- index allocation ----------------------------------------------------- */

static VOID
RemoveAttrRec(PMFT_RECORD m, PATTRIBUTE_RECORD a)
{
    PUCHAR p = (PUCHAR)m, at = (PUCHAR)a;
    ULONG len = a->Length, end = (ULONG)(at - p) + len;
    memmove(at, at + len, m->BytesInUse - end);
    m->BytesInUse -= len;
}

/* give a SMALL directory an (empty) $INDEX_ALLOCATION + $BITMAP "$I30" */
static BOOLEAN
EnsureIA(PTREE t)
{
    ULONG cs = t->Vcb->Geo.ClusterSize, blk = (t->Ibs + cs - 1) / cs, mpLen, i;
    UCHAR ab[0x60];
    NTFS_RUN run;
    ULONGLONG lcn;
    PATTRIBUTE_RECORD a;

    if (NtfsFindAttribute(M(t), ATTR_INDEX_ALLOCATION, I30, 4)) return TRUE;
    if (!NT_SUCCESS(NtfsAllocClusters(t->Vcb, blk, &lcn))) return FALSE;
    RtlZeroMemory(ab, sizeof(ab));
    run.Vcn = 0; run.Lcn = lcn; run.Length = blk; run.Sparse = FALSE;
    a = (PATTRIBUTE_RECORD)ab;
    if (!NtfsEncodeRuns(&run, 1, ab + 0x48, sizeof(ab) - 0x48, &mpLen)) goto fail;
    a->Type = ATTR_INDEX_ALLOCATION; a->NonResident = 1; a->NameLength = 4; a->NameOffset = 0x40;
    a->Form.NonResident.HighestVcn = blk - 1;
    a->Form.NonResident.MappingPairsOffset = 0x48;
    a->Form.NonResident.AllocatedSize = (ULONGLONG)blk * cs;
    a->Form.NonResident.DataSize = (ULONGLONG)blk * cs;
    a->Form.NonResident.InitializedSize = (ULONGLONG)blk * cs;
    for (i = 0; i < 4; i++) ((PWCHAR)(ab + 0x40))[i] = I30[i];
    a->Length = (0x48 + mpLen + 7) & ~7u;
    if (!NtfsInsertAttribute(M(t), t->Rs, ab, a->Length)) goto fail;
    if (!NtfsFindAttribute(M(t), ATTR_BITMAP, I30, 4)) {
        RtlZeroMemory(ab, sizeof(ab));
        a->Type = ATTR_BITMAP; a->NameLength = 4; a->NameOffset = 0x18;
        a->Form.Resident.ValueOffset = 0x20; a->Form.Resident.ValueLength = 8;
        for (i = 0; i < 4; i++) ((PWCHAR)(ab + 0x18))[i] = I30[i];
        a->Length = 0x28;
        if (!NtfsInsertAttribute(M(t), t->Rs, ab, a->Length)) {
            RemoveAttrRec(M(t), NtfsFindAttribute(M(t), ATTR_INDEX_ALLOCATION, I30, 4));
            goto fail;
        }
    }
    t->RecDirty = TRUE;
    ReloadRuns(t);
    return TRUE;
fail:
    NtfsFreeClusters(t->Vcb, lcn, blk);
    return FALSE;
}

/* an empty node block: a reused free block or a new one at the end */
static LONG
NewBlock(PTREE t, BOOLEAN internal, ULONGLONG endSub)
{
    PATTRIBUTE_RECORD ia;
    ULONG cs = t->Vcb->Geo.ClusterSize, nblk, b;
    ULONGLONG vcn;
    PUCHAR buf;
    PINDEX_HEADER h;

    if (t->Nn >= MAX_NODES || !EnsureIA(t)) { t->Failed = TRUE; return -2; }
    ia = NtfsFindAttribute(M(t), ATTR_INDEX_ALLOCATION, I30, 4);
    nblk = (ULONG)(ia->Form.NonResident.DataSize / t->Ibs);
    for (b = 0; b < nblk; b++) {
        ULONG i; BOOLEAN cached = FALSE;
        if (BmpTest(t, b)) continue;
        for (i = 0; i < t->Nn; i++)          /* freed earlier in this operation */
            if (t->N[i].Vcn == (ULONGLONG)b * t->VcnPerBlk && !t->N[i].Freed) cached = TRUE;
        if (!cached) break;
    }
    if (b >= nblk) {
        /* extend $INDEX_ALLOCATION: by 1/8 of its size (1..16 blocks), placed
         * right after its last run when possible, so the run list stays short
         * enough for the directory's MFT record */
        ULONG nb = nblk / 8, blk, rc = t->Rc;
        ULONGLONG lcn, oldAlloc = ia->Form.NonResident.AllocatedSize, hint = 0;
        if (nb < 1) nb = 1;
        if (nb > 16) nb = 16;
        blk = (nb * t->Ibs + cs - 1) / cs;
        if (rc && !t->Runs[rc - 1].Sparse) hint = t->Runs[rc - 1].Lcn + t->Runs[rc - 1].Length;
        if (rc == 0 || rc >= NTFS_MAX_IDX_RUNS - 1 ||
            (!NT_SUCCESS(NtfsAllocClustersNear(t->Vcb, blk, hint, &lcn)) &&
             (blk = (t->Ibs + cs - 1) / cs,
              !NT_SUCCESS(NtfsAllocClustersNear(t->Vcb, blk, hint, &lcn))))) { t->Failed = TRUE; return -2; }
        if (!t->Runs[rc - 1].Sparse && t->Runs[rc - 1].Lcn + t->Runs[rc - 1].Length == lcn) {
            t->Runs[rc - 1].Length += blk;
        } else {
            t->Runs[rc].Vcn = t->Runs[rc - 1].Vcn + t->Runs[rc - 1].Length;
            t->Runs[rc].Lcn = lcn; t->Runs[rc].Length = blk; t->Runs[rc].Sparse = FALSE; rc++;
        }
        if (!NtfsRewriteAttrRuns(M(t), t->Rs, ia, t->Runs, rc,
                                 oldAlloc + (ULONGLONG)blk * cs, oldAlloc + (ULONGLONG)blk * cs)) {
            NtfsFreeClusters(t->Vcb, lcn, blk); ReloadRuns(t); t->Failed = TRUE; return -2;
        }
        t->RecDirty = TRUE;
        ReloadRuns(t);
        b = nblk;
    }
    if (!BmpCover(t, b)) { t->Failed = TRUE; return -2; }
    BmpSet(t, b, TRUE);
    vcn = (ULONGLONG)b * t->VcnPerBlk;

    buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, t->Ibs, NTFS_TAG_BUF);
    if (buf == NULL) { t->Failed = TRUE; return -2; }
    NtfsInitIndexLeaf(buf, t->Ibs, t->Vcb->Geo.BytesPerSector, vcn);
    h = &((PINDEX_BLOCK)buf)->Header;
    if (internal) {
        PINDEX_ENTRY e = NtfsFirstIndexEntry(h);
        e->Length = 0x18; e->Flags = INDEX_ENTRY_END | INDEX_ENTRY_HAS_SUBNODE;
        *(ULONGLONG *)((PUCHAR)e + 0x10) = endSub;
        h->IndexLength += 8;
        h->Flags = 1;
    }
    t->N[t->Nn].Vcn = vcn; t->N[t->Nn].Buf = buf;
    t->N[t->Nn].Dirty = TRUE; t->N[t->Nn].Freed = FALSE;
    return (LONG)t->Nn++;
}

static VOID
FreeNode(PTREE t, LONG h)
{
    BmpSet(t, (ULONG)(t->N[h].Vcn / t->VcnPerBlk), FALSE);
    t->N[h].Freed = TRUE;
}

/* ---- entries -------------------------------------------------------------- */

static PINDEX_ENTRY First(PINDEX_HEADER h) { return NtfsFirstIndexEntry(h); }
static PINDEX_ENTRY Next(PINDEX_ENTRY e) { return (PINDEX_ENTRY)((PUCHAR)e + e->Length); }
static BOOLEAN      IsEnd(PINDEX_ENTRY e) { return (e->Flags & INDEX_ENTRY_END) != 0; }
static ULONGLONG    Sub(PINDEX_ENTRY e) { return *(ULONGLONG *)((PUCHAR)e + e->Length - 8); }
static VOID         SetSub(PINDEX_ENTRY e, ULONGLONG v) { *(ULONGLONG *)((PUCHAR)e + e->Length - 8) = v; }

static PINDEX_ENTRY
EndOf(PINDEX_HEADER h)
{
    PINDEX_ENTRY e = First(h);
    while (!IsEnd(e) && e->Length) e = Next(e);
    return e;
}

static PINDEX_ENTRY
PrevOf(PINDEX_HEADER h, PINDEX_ENTRY x)
{
    PINDEX_ENTRY e = First(h), p = NULL;
    while (e != x && !IsEnd(e) && e->Length) { p = e; e = Next(e); }
    return (e == x) ? p : NULL;
}

static BOOLEAN HasKeys(PINDEX_HEADER h) { return !IsEnd(First(h)); }

static PINDEX_ENTRY
ChildPtr(PINDEX_HEADER h, ULONGLONG vcn)
{
    PINDEX_ENTRY e;
    for (e = First(h); ; e = Next(e)) {
        if ((e->Flags & INDEX_ENTRY_HAS_SUBNODE) && Sub(e) == vcn) return e;
        if (IsEnd(e) || e->Length == 0) return NULL;
    }
}

static PWCHAR KeyName(PINDEX_ENTRY e, PULONG n)
{
    PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)((PUCHAR)e + sizeof(INDEX_ENTRY));
    *n = fn->NameLength;
    return (PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR));
}

/* name vs entry: <0 before, 0 equal, >0 after (END sorts last) */
static int
Cmp(const WCHAR *n, ULONG nl, PINDEX_ENTRY e)
{
    ULONG el; PWCHAR en;
    if (IsEnd(e)) return -1;
    en = KeyName(e, &el);
    return NtfsCollateName(n, nl, en, el);
}

/* copy of entry src's key as a leaf entry, or with subnode `sub` */
static ULONG
MakeEnt(PUCHAR out, PINDEX_ENTRY src, BOOLEAN withSub, ULONGLONG sub)
{
    ULONG len = (sizeof(INDEX_ENTRY) + src->KeyLength + 7) & ~7u;
    if (withSub) len += 8;
    RtlZeroMemory(out, len);
    RtlCopyMemory(out, src, sizeof(INDEX_ENTRY) + src->KeyLength);
    ((PINDEX_ENTRY)out)->Length = (USHORT)len;
    ((PINDEX_ENTRY)out)->Flags = withSub ? INDEX_ENTRY_HAS_SUBNODE : 0;
    if (withSub) *(ULONGLONG *)(out + len - 8) = sub;
    return len;
}

static PINDEX_ENTRY
FindName(PINDEX_HEADER h, const WCHAR *n, ULONG nl)
{
    PINDEX_ENTRY e;
    for (e = First(h); !IsEnd(e) && e->Length; e = Next(e))
        if (Cmp(n, nl, e) == 0) return e;
    return NULL;
}

/* ---- node edits ----------------------------------------------------------- */

static BOOLEAN
NodeInsert(PTREE t, LONG h, PUCHAR ent, ULONG len)
{
    if (h == ROOTH) {
        /* keep some slack in the directory record for the run list and the
         * $I30 bitmap to grow; a fuller root is pushed down instead */
        if (M(t)->BytesInUse + len + ROOT_SLACK > t->Rs) return FALSE;
        if (!NtfsIndexRootInsert(M(t), t->Rs, ent, len)) return FALSE;
        t->RecDirty = TRUE;
        return TRUE;
    }
    if (!NtfsIndexInsert(Hdr(t, h), t->Ibs - 0x18, ent, len)) return FALSE;
    t->N[h].Dirty = TRUE;
    return TRUE;
}

static VOID
NodeRemove(PTREE t, LONG h, PINDEX_ENTRY e)
{
    PINDEX_HEADER hd = Hdr(t, h);
    ULONG len = e->Length;
    PUCHAR end = (PUCHAR)hd + hd->IndexLength;
    memmove(e, (PUCHAR)e + len, (ULONG)(end - ((PUCHAR)e + len)));
    hd->IndexLength -= len;
    if (h == ROOTH) {
        hd->AllocatedLength = hd->IndexLength;
        NtfsResizeResidentValue(M(t), t->Rs, RootAttr(t), 0x10 + hd->IndexLength);
        t->RecDirty = TRUE;
    } else {
        t->N[h].Dirty = TRUE;
    }
}

/* the root as one END entry pointing at block `sub` (LARGE index) */
static BOOLEAN
RootToEnd(PTREE t, ULONGLONG sub)
{
    PINDEX_HEADER hd;
    PINDEX_ENTRY e;
    if (!NtfsResizeResidentValue(M(t), t->Rs, RootAttr(t), 0x10 + 0x10 + 0x18)) return FALSE;
    hd = Hdr(t, ROOTH);
    hd->EntriesOffset = 0x10; hd->IndexLength = hd->AllocatedLength = 0x10 + 0x18; hd->Flags = 1;
    e = First(hd);
    RtlZeroMemory(e, 0x18);
    e->Length = 0x18; e->Flags = INDEX_ENTRY_END | INDEX_ENTRY_HAS_SUBNODE;
    SetSub(e, sub);
    t->RecDirty = TRUE;
    return TRUE;
}

/* move the root's whole content into a new block C; root -> END(C).
 * The root is emptied first so the record has room for $INDEX_ALLOCATION. */
static LONG
PushRootDown(PTREE t)
{
    PINDEX_HEADER rh = Hdr(t, ROOTH), ch;
    ULONG bytes = rh->IndexLength - rh->EntriesOffset;
    UCHAR flags = rh->Flags;
    PUCHAR save;
    LONG c;

    save = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, bytes, NTFS_TAG_BUF);
    if (save == NULL) { t->Failed = TRUE; return -2; }
    RtlCopyMemory(save, (PUCHAR)rh + rh->EntriesOffset, bytes);
    if (!RootToEnd(t, 0)) { ExFreePool(save); t->Failed = TRUE; return -2; }
    c = NewBlock(t, FALSE, 0);
    if (c < 0 || Hdr(t, c)->EntriesOffset + bytes > t->Ibs - 0x18) {
        /* put the root back as it was */
        if (NtfsResizeResidentValue(M(t), t->Rs, RootAttr(t), 0x10 + 0x10 + bytes)) {
            rh = Hdr(t, ROOTH);
            rh->EntriesOffset = 0x10; rh->IndexLength = rh->AllocatedLength = 0x10 + bytes;
            rh->Flags = flags;
            RtlCopyMemory((PUCHAR)rh + 0x10, save, bytes);
        }
        ExFreePool(save); t->Failed = TRUE; return -2;
    }
    ch = Hdr(t, c);
    RtlCopyMemory((PUCHAR)ch + ch->EntriesOffset, save, bytes);
    ch->IndexLength = ch->EntriesOffset + bytes;
    ch->Flags = flags & 1;
    ExFreePool(save);
    SetSub(First(Hdr(t, ROOTH)), NodeVcn(t, c));
    t->RecDirty = TRUE;
    return c;
}

/*
 * Split block h that cannot take `ent`: h keeps the lower half, a new block
 * the upper half; the median comes back in med (with h as its subnode) for
 * the caller to insert into the parent. *rh gets the new block.
 */
static BOOLEAN
SplitNode(PTREE t, LONG h, PUCHAR ent, ULONG len, PUCHAR med, PULONG medLen, PLONG rh)
{
    PINDEX_HEADER hd = Hdr(t, h);
    BOOLEAN internal = (hd->Flags & 1) != 0, placed = FALSE;
    ULONGLONG endSub = internal ? Sub(EndOf(hd)) : 0, vcn = NodeVcn(t, h);
    PUCHAR tmp;
    PULONG off;
    ULONG cnt = 0, pos = 0, total, run = 0, m = 0, i, nl;
    PWCHAR nn = KeyName((PINDEX_ENTRY)ent, &nl);
    PINDEX_ENTRY e;
    LONG r;

    /* scratch (pool, not stack): the entries in order + their offsets */
    tmp = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, t->Ibs + ENT_MAX + 300 * sizeof(ULONG), NTFS_TAG_BUF);
    if (tmp == NULL) return FALSE;
    off = (PULONG)(tmp + t->Ibs + ENT_MAX);
    for (e = First(hd); !IsEnd(e) && e->Length && cnt < 298; e = Next(e)) {
        if (!placed && Cmp(nn, nl, e) <= 0) {
            off[cnt++] = pos; RtlCopyMemory(tmp + pos, ent, len); pos += len; placed = TRUE;
        }
        off[cnt++] = pos; RtlCopyMemory(tmp + pos, e, e->Length); pos += e->Length;
    }
    if (!placed) { off[cnt++] = pos; RtlCopyMemory(tmp + pos, ent, len); pos += len; }
    total = pos;
    if (cnt < 3) { ExFreePool(tmp); return FALSE; }
    for (i = 0; i < cnt; i++) {
        ULONG l = ((PINDEX_ENTRY)(tmp + off[i]))->Length;
        if (run + l > total / 2) { m = i; break; }
        run += l;
    }
    if (m < 1) m = 1;
    if (m > cnt - 2) m = cnt - 2;

    r = NewBlock(t, internal, endSub);
    if (r < 0) { ExFreePool(tmp); return FALSE; }
    /* lower half back into h (its END now leads to the median's old child) */
    NtfsInitIndexLeaf(t->N[h].Buf, t->Ibs, t->Vcb->Geo.BytesPerSector, vcn);
    hd = Hdr(t, h);
    if (internal) {
        e = First(hd);
        e->Length = 0x18; e->Flags = INDEX_ENTRY_END | INDEX_ENTRY_HAS_SUBNODE;
        SetSub(e, Sub((PINDEX_ENTRY)(tmp + off[m])));
        hd->IndexLength += 8; hd->Flags = 1;
    }
    for (i = 0; i < m; i++)
        NtfsIndexInsert(hd, t->Ibs - 0x18, tmp + off[i], ((PINDEX_ENTRY)(tmp + off[i]))->Length);
    for (i = m + 1; i < cnt; i++)
        NtfsIndexInsert(Hdr(t, r), t->Ibs - 0x18, tmp + off[i], ((PINDEX_ENTRY)(tmp + off[i]))->Length);
    *medLen = MakeEnt(med, (PINDEX_ENTRY)(tmp + off[m]), TRUE, vcn);
    t->N[h].Dirty = TRUE; t->N[r].Dirty = TRUE;
    *rh = r;
    ExFreePool(tmp);
    return TRUE;
}

/* insert ent into path[lvl], splitting upward as needed */
static BOOLEAN InsertAt2(PTREE t, LONG *path, int lvl, PUCHAR ent, ULONG len, PUCHAR med);

static BOOLEAN
InsertAt(PTREE t, LONG *path, int lvl, PUCHAR ent, ULONG len)
{
    BOOLEAN ok;
    PUCHAR med;
    if (NodeInsert(t, path[lvl], ent, len)) return TRUE;
    med = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, ENT_MAX, NTFS_TAG_BUF);   /* keep the stack small */
    if (med == NULL) { t->Failed = TRUE; return FALSE; }
    ok = InsertAt2(t, path, lvl, ent, len, med);
    ExFreePool(med);
    return ok;
}

static BOOLEAN
InsertAt2(PTREE t, LONG *path, int lvl, PUCHAR ent, ULONG len, PUCHAR med)
{
    LONG h = path[lvl], r, c;
    ULONG mlen;
    PINDEX_ENTRY p;

    if (h == ROOTH) {
        /* the root is full: the tree grows one level */
        c = PushRootDown(t);
        if (c < 0) return FALSE;
        if (NodeInsert(t, c, ent, len)) return TRUE;
        path[0] = ROOTH; path[1] = c;
        if (!SplitNode(t, c, ent, len, med, &mlen, &r)) return FALSE;
        p = ChildPtr(Hdr(t, ROOTH), NodeVcn(t, c));
        if (p == NULL) return FALSE;
        SetSub(p, NodeVcn(t, r)); t->RecDirty = TRUE;
        return NodeInsert(t, ROOTH, med, mlen);
    }
    if (!SplitNode(t, h, ent, len, med, &mlen, &r)) return FALSE;
    p = ChildPtr(Hdr(t, path[lvl - 1]), NodeVcn(t, h));
    if (p == NULL) return FALSE;
    SetSub(p, NodeVcn(t, r)); Touch(t, path[lvl - 1]);
    return InsertAt(t, path, lvl - 1, med, mlen);
}

/*
 * Descend for name: path[0..*depth] are the nodes visited (path[0] = root).
 * Returns the matching entry in path[*depth], or NULL with path[*depth] the
 * leaf where the name belongs.
 */
static PINDEX_ENTRY
Descend(PTREE t, const WCHAR *n, ULONG nl, LONG *path, int *depth)
{
    LONG h = ROOTH;
    int d = 0;
    path[0] = ROOTH;
    for (;;) {
        PINDEX_ENTRY e;
        for (e = First(Hdr(t, h)); ; e = Next(e)) {
            int c = Cmp(n, nl, e);
            if (c == 0) { *depth = d; return e; }
            if (c < 0) break;
            if (e->Length == 0) { *depth = d; return NULL; }
        }
        if (!(e->Flags & INDEX_ENTRY_HAS_SUBNODE) || d + 1 >= MAX_DEPTH) { *depth = d; return NULL; }
        h = Load(t, Sub(e));
        if (h < 0) { *depth = d; return NULL; }
        path[++d] = h;
    }
}

/* the root has no keys and one child: pull the child up if it fits;
 * a root that is a leaf again with no blocks in use drops $INDEX_ALLOCATION */
static VOID
RootCollapse(PTREE t)
{
    PINDEX_HEADER rh = Hdr(t, ROOTH);
    PATTRIBUTE_RECORD ia, bm;
    ULONG len, i;
    PUCHAR bits;

    while (!HasKeys(rh) && (rh->Flags & 1)) {
        LONG c = Load(t, Sub(First(rh)));
        PINDEX_HEADER ch;
        ULONG bytes;
        if (c < 0) return;
        ch = Hdr(t, c);
        bytes = ch->IndexLength - ch->EntriesOffset;
        if (!NtfsResizeResidentValue(M(t), t->Rs, RootAttr(t), 0x10 + 0x10 + bytes)) return;
        rh = Hdr(t, ROOTH);
        rh->EntriesOffset = 0x10;
        rh->IndexLength = rh->AllocatedLength = 0x10 + bytes;
        rh->Flags = ch->Flags;
        RtlCopyMemory((PUCHAR)rh + 0x10, (PUCHAR)ch + ch->EntriesOffset, bytes);
        FreeNode(t, c);
        t->RecDirty = TRUE;
    }
    if (rh->Flags & 1) return;
    ia = NtfsFindAttribute(M(t), ATTR_INDEX_ALLOCATION, I30, 4);
    if (ia == NULL) return;
    bits = BmpBytes(t, &len);
    for (i = 0; bits && i < len; i++) if (bits[i]) return;
    /* nothing left in the allocation: back to a SMALL index */
    for (i = 0; i < t->Rc; i++)
        if (!t->Runs[i].Sparse && t->Runs[i].Length)
            NtfsFreeClusters(t->Vcb, t->Runs[i].Lcn, (ULONG)t->Runs[i].Length);
    RemoveAttrRec(M(t), ia);
    bm = NtfsFindAttribute(M(t), ATTR_BITMAP, I30, 4);
    if (bm) {
        if (bm->NonResident) {
            NTFS_RUN br[16]; ULONG bc;
            if (NtfsDecodeRuns(bm, br, 16, &bc))
                for (i = 0; i < bc; i++)
                    if (!br[i].Sparse && br[i].Length) NtfsFreeClusters(t->Vcb, br[i].Lcn, (ULONG)br[i].Length);
            if (t->Bmp) { ExFreePool(t->Bmp); t->Bmp = NULL; t->BmpDirty = FALSE; }
        }
        RemoveAttrRec(M(t), bm);
    }
    t->Rc = 0;
    t->RecDirty = TRUE;
}

/*
 * path[lvl] may have lost its last key. Merge it with a sibling through the
 * parent separator, or rotate a key in from the sibling; recurse upward.
 */
static BOOLEAN FixUnderflow2(PTREE t, LONG *path, int lvl, PUCHAR k, PUCHAR k2);

static BOOLEAN
FixUnderflow(PTREE t, LONG *path, int lvl)
{
    BOOLEAN ok;
    PUCHAR k;
    if (path[lvl] == ROOTH) { RootCollapse(t); return TRUE; }
    if (HasKeys(Hdr(t, path[lvl]))) return TRUE;
    k = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 2 * ENT_MAX, NTFS_TAG_BUF);
    if (k == NULL) { t->Failed = TRUE; return FALSE; }
    ok = FixUnderflow2(t, path, lvl, k, k + ENT_MAX);
    ExFreePool(k);
    return ok;
}

static BOOLEAN
FixUnderflow2(PTREE t, LONG *path, int lvl, PUCHAR k, PUCHAR k2)
{
    LONG h = path[lvl], par, s;
    PINDEX_HEADER hd = Hdr(t, h), ph, sh;
    PINDEX_ENTRY p, q, x;
    BOOLEAN internal;
    ULONG klen, k2len, cap = t->Ibs - 0x18;

    internal = (hd->Flags & 1) != 0;
    par = path[lvl - 1];
    ph = Hdr(t, par);
    p = ChildPtr(ph, NodeVcn(t, h));
    if (p == NULL) return FALSE;

    if (!IsEnd(p)) {
        /* right sibling: the subtree of the entry after p */
        s = Load(t, Sub(Next(p)));
        if (s < 0) return FALSE;
        sh = Hdr(t, s);
        klen = MakeEnt(k, p, internal, internal ? Sub(EndOf(hd)) : 0);
        if (sh->IndexLength + klen <= cap) {
            NodeInsert(t, s, k, klen);                     /* separator joins it */
            NodeRemove(t, par, ChildPtr(Hdr(t, par), NodeVcn(t, h)));
            FreeNode(t, h);
            return FixUnderflow(t, path, lvl - 1);
        }
        /* rotate: h takes the separator, the sibling's first key moves up */
        x = First(sh);
        NodeInsert(t, h, k, klen);
        if (internal) { SetSub(EndOf(Hdr(t, h)), Sub(x)); t->N[h].Dirty = TRUE; }
        k2len = MakeEnt(k2, x, TRUE, NodeVcn(t, h));
        NodeRemove(t, s, First(Hdr(t, s)));
        NodeRemove(t, par, ChildPtr(Hdr(t, par), NodeVcn(t, h)));
        return InsertAt(t, path, lvl - 1, k2, k2len);
    }

    /* h hangs off the parent's END: use the left sibling */
    q = PrevOf(ph, p);
    if (q == NULL) {
        /* the parent has no keys either: only the root can be like that */
        if (par != ROOTH) return TRUE;
        if (internal) {
            SetSub(p, Sub(EndOf(hd)));                     /* one level less */
            t->RecDirty = TRUE;
            FreeNode(t, h);
        } else {
            /* the whole index is empty */
            FreeNode(t, h);
            if (!NtfsResizeResidentValue(M(t), t->Rs, RootAttr(t), 0x30)) return FALSE;
            ph = Hdr(t, ROOTH);
            ph->EntriesOffset = 0x10; ph->IndexLength = ph->AllocatedLength = 0x20; ph->Flags = 0;
            p = First(ph); RtlZeroMemory(p, 0x10); p->Length = 0x10; p->Flags = INDEX_ENTRY_END;
            t->RecDirty = TRUE;
        }
        RootCollapse(t);
        return TRUE;
    }
    s = Load(t, Sub(q));
    if (s < 0) return FALSE;
    sh = Hdr(t, s);
    klen = MakeEnt(k, q, internal, internal ? Sub(EndOf(sh)) : 0);
    if (sh->IndexLength + klen <= cap) {
        ULONGLONG c0 = internal ? Sub(EndOf(hd)) : 0;
        NodeInsert(t, s, k, klen);                         /* goes last */
        if (internal) { SetSub(EndOf(Hdr(t, s)), c0); t->N[s].Dirty = TRUE; }
        NodeRemove(t, par, ChildPtr(Hdr(t, par), NodeVcn(t, s)));
        SetSub(EndOf(Hdr(t, par)), NodeVcn(t, s)); Touch(t, par);
        FreeNode(t, h);
        return FixUnderflow(t, path, lvl - 1);
    }
    /* rotate: h takes the separator, the sibling's last key moves up */
    x = PrevOf(sh, EndOf(sh));
    NodeInsert(t, h, k, klen);
    if (internal) { SetSub(EndOf(Hdr(t, s)), Sub(x)); t->N[s].Dirty = TRUE; }
    k2len = MakeEnt(k2, x, TRUE, NodeVcn(t, s));
    NodeRemove(t, s, PrevOf(Hdr(t, s), EndOf(Hdr(t, s))));
    NodeRemove(t, par, ChildPtr(Hdr(t, par), NodeVcn(t, s)));
    return InsertAt(t, path, lvl - 1, k2, k2len);
}

/* write every touched block, then the directory record */
static NTSTATUS
Commit(PTREE t)
{
    NTSTATUS st = STATUS_SUCCESS;
    ULONG i;
    if (t->Failed) return STATUS_DISK_FULL;
    for (i = 0; i < t->Nn; i++)
        if (t->N[i].Dirty && !t->N[i].Freed) {
            ((PINDEX_BLOCK)t->N[i].Buf)->IndexBlockVcn = t->N[i].Vcn;
            st = WriteBlk(t, t->N[i].Vcn, t->N[i].Buf);
            if (!NT_SUCCESS(st)) return st;
        }
    if (t->BmpDirty && t->Bmp) {
        PATTRIBUTE_RECORD a = NtfsFindAttribute(M(t), ATTR_BITMAP, I30, 4);
        NTFS_RUN br[16]; ULONG bc;
        if (a && a->NonResident && NtfsDecodeRuns(a, br, 16, &bc))
            NtfsWriteAttributeData(t->Vcb, br, bc, 0, (t->BmpLen + 511) & ~511u, t->Bmp);
    }
    if (t->RecDirty) st = NtfsWriteMftRecord(t->Vcb, t->Dir, t->Rec);
    return st;
}

/* ---- public operations ---------------------------------------------------- */

/* Insert a prebuilt entry (name wname) into directory Dir's index. */
NTSTATUS
NtfsIndexAdd(PNTFS_VCB Vcb, ULONGLONG Dir, PUCHAR Ent, ULONG EntLen,
             const WCHAR *wname, ULONG wn)
{
    TREE *t;
    LONG path[MAX_DEPTH];
    int depth;
    NTSTATUS st;

    t = (TREE *)ExAllocatePoolWithTag(NonPagedPool, sizeof(TREE), NTFS_TAG_BUF);
    if (t == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (!TreeOpen(t, Vcb, Dir)) { TreeClose(t); ExFreePool(t); return STATUS_NOT_A_DIRECTORY; }
    if (Descend(t, wname, wn, path, &depth) != NULL) st = STATUS_OBJECT_NAME_COLLISION;
    else if (t->Failed) st = STATUS_FILE_CORRUPT_ERROR;
    else if (!InsertAt(t, path, depth, Ent, EntLen)) st = STATUS_DISK_FULL;
    else st = Commit(t);
    TreeClose(t); ExFreePool(t);
    return st;
}

/* Remove wname from directory Dir's index. */
NTSTATUS
NtfsIndexDelete(PNTFS_VCB Vcb, ULONGLONG Dir, const WCHAR *wname, ULONG wn)
{
    TREE *t;
    LONG path[MAX_DEPTH];
    int depth, d;
    PINDEX_ENTRY e;
    NTSTATUS st = STATUS_SUCCESS;

    t = (TREE *)ExAllocatePoolWithTag(NonPagedPool, sizeof(TREE), NTFS_TAG_BUF);
    if (t == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (!TreeOpen(t, Vcb, Dir)) { TreeClose(t); ExFreePool(t); return STATUS_NOT_A_DIRECTORY; }
    e = Descend(t, wname, wn, path, &depth);
    if (e == NULL) { st = t->Failed ? STATUS_FILE_CORRUPT_ERROR : STATUS_OBJECT_NAME_NOT_FOUND; goto out; }

    if (!(e->Flags & INDEX_ENTRY_HAS_SUBNODE)) {
        NodeRemove(t, path[depth], e);
        if (!FixUnderflow(t, path, depth)) st = STATUS_DISK_FULL;
    } else {
        /* internal key: replace it with its in-order predecessor */
        UCHAR rep[ENT_MAX];
        WCHAR pn[256];   /* NtfsIndexDelete is not recursive: these fit the stack */
        ULONG rlen, pnl, i;
        PWCHAR kn;
        LONG h, x = path[depth];
        PINDEX_ENTRY z;
        ULONGLONG esub = Sub(e);

        d = depth;
        h = Load(t, esub);
        if (h < 0) { st = STATUS_FILE_CORRUPT_ERROR; goto out; }
        path[++d] = h;
        while (IsInternalNode(t, h) && d + 1 < MAX_DEPTH) {
            h = Load(t, Sub(EndOf(Hdr(t, h))));
            if (h < 0) { st = STATUS_FILE_CORRUPT_ERROR; goto out; }
            path[++d] = h;
        }
        z = PrevOf(Hdr(t, h), EndOf(Hdr(t, h)));
        if (z == NULL) { st = STATUS_FILE_CORRUPT_ERROR; goto out; }
        rlen = MakeEnt(rep, z, TRUE, esub);
        kn = KeyName(z, &pnl);
        for (i = 0; i < pnl; i++) pn[i] = kn[i];
        NodeRemove(t, h, z);                               /* out of the leaf */
        NodeRemove(t, x, FindName(Hdr(t, x), wname, wn));  /* the deleted key */
        if (!InsertAt(t, path, depth, rep, rlen)) { st = STATUS_DISK_FULL; goto out; }
        /* the leaf may be empty now: find it again (the tree above may have
         * changed) - it is the rightmost leaf under the predecessor key */
        if (!HasKeys(Hdr(t, h))) {
            e = Descend(t, pn, pnl, path, &d);
            if (e == NULL || !(e->Flags & INDEX_ENTRY_HAS_SUBNODE)) { st = STATUS_FILE_CORRUPT_ERROR; goto out; }
            h = Load(t, Sub(e));
            if (h < 0) { st = STATUS_FILE_CORRUPT_ERROR; goto out; }
            path[++d] = h;
            while (IsInternalNode(t, h) && d + 1 < MAX_DEPTH) {
                h = Load(t, Sub(EndOf(Hdr(t, h))));
                if (h < 0) { st = STATUS_FILE_CORRUPT_ERROR; goto out; }
                path[++d] = h;
            }
            if (!FixUnderflow(t, path, d)) st = STATUS_DISK_FULL;
        }
    }
    if (NT_SUCCESS(st)) st = Commit(t);
out:
    TreeClose(t); ExFreePool(t);
    return st;
}

/* Refresh the duplicated information (times, sizes, attributes) in the index
 * entry for fn's name in its parent directory. */
NTSTATUS
NtfsIndexUpdate(PNTFS_VCB Vcb, const FILE_NAME_ATTR *fn)
{
    TREE *t;
    LONG path[MAX_DEPTH];
    int depth;
    PINDEX_ENTRY e;
    NTSTATUS st;
    const ULONG dupOff = FIELD_OFFSET(FILE_NAME_ATTR, CreationTime);
    const ULONG dupLen = FIELD_OFFSET(FILE_NAME_ATTR, NameLength) - dupOff;

    t = (TREE *)ExAllocatePoolWithTag(NonPagedPool, sizeof(TREE), NTFS_TAG_BUF);
    if (t == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (!TreeOpen(t, Vcb, NTFS_MREF_RECORD(fn->ParentDirectory))) {
        TreeClose(t); ExFreePool(t); return STATUS_NOT_A_DIRECTORY;
    }
    e = Descend(t, (const WCHAR *)((const UCHAR *)fn + sizeof(FILE_NAME_ATTR)),
                fn->NameLength, path, &depth);
    if (e == NULL) st = STATUS_OBJECT_NAME_NOT_FOUND;
    else {
        RtlCopyMemory((PUCHAR)e + sizeof(INDEX_ENTRY) + dupOff, (const UCHAR *)fn + dupOff, dupLen);
        Touch(t, path[depth]);
        st = Commit(t);
    }
    TreeClose(t); ExFreePool(t);
    return st;
}

/* Look Name up in directory Dir: its MFT record number, or -1. */
ULONGLONG
NtfsLookupInDir(PNTFS_VCB Vcb, ULONGLONG Dir, const WCHAR *Name, ULONG NameLen)
{
    TREE *t;
    LONG path[MAX_DEPTH];
    int depth;
    PINDEX_ENTRY e;
    ULONGLONG r = (ULONGLONG)-1;

    if (NameLen == 0) return r;
    t = (TREE *)ExAllocatePoolWithTag(NonPagedPool, sizeof(TREE), NTFS_TAG_BUF);
    if (t == NULL) return r;
    if (TreeOpen(t, Vcb, Dir)) {
        e = Descend(t, Name, NameLen, path, &depth);
        if (e) r = NTFS_MREF_RECORD(e->FileReference);
    }
    TreeClose(t); ExFreePool(t);
    return r;
}
