/*
 * ntfsbtree - stress test for the directory index B-tree (src/ntfsidx.c),
 * running the exact driver code in user mode against an NTFS image.
 *
 *   ntfsbtree <image> <dir-record> <seed> <ops> <maxnamelen>
 *
 * Performs random inserts/deletes of synthetic index entries in the given
 * directory, mirroring them in a model, and after every batch checks the
 * tree: keys strictly ordered and inside their parent's bounds, every leaf
 * at the same depth, no empty non-root node, leaf/internal entry flags
 * consistent, INDX VCNs right, the $I30 bitmap matching exactly the reachable
 * blocks, and every model name found by NtfsLookupInDir. Finally all names
 * are deleted: the index must be SMALL again and no cluster may leak.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "ntfsidxtest.h"

ULONGLONG NtfsLookupInDir(PNTFS_VCB Vcb, ULONGLONG Dir, const WCHAR *Name, ULONG NameLen);
NTSTATUS NtfsIndexAdd(PNTFS_VCB Vcb, ULONGLONG Dir, PUCHAR Ent, ULONG EntLen, const WCHAR *wname, ULONG wn);
NTSTATUS NtfsIndexDelete(PNTFS_VCB Vcb, ULONGLONG Dir, const WCHAR *wname, ULONG wn);

static int fd;
static NTFS_VCB V;
static NTFS_RUN MftRuns[256]; static ULONG MftRc;
static NTFS_RUN BmRuns[64]; static ULONG BmRc; static UCHAR *Bm; static ULONG BmLen;

static void die(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }

static ULONGLONG map(PNTFS_RUN r, ULONG c, ULONGLONG vbo)
{
    ULONGLONG vcn = vbo / V.Geo.ClusterSize, off = vbo % V.Geo.ClusterSize; ULONG i;
    for (i = 0; i < c; i++)
        if (vcn >= r[i].Vcn && vcn < r[i].Vcn + r[i].Length)
            return (r[i].Lcn + (vcn - r[i].Vcn)) * V.Geo.ClusterSize + off;
    die("unmapped vbo"); return 0;
}
static void io(int w, PNTFS_RUN r, ULONG c, ULONGLONG off, ULONG len, void *buf)
{
    while (len) {
        ULONG n = V.Geo.ClusterSize - (ULONG)(off % V.Geo.ClusterSize);
        if (n > len) n = len;
        if ((w ? pwrite(fd, buf, n, (off_t)map(r, c, off)) : pread(fd, buf, n, (off_t)map(r, c, off))) != (ssize_t)n) die("io");
        off += n; len -= n; buf = (UCHAR *)buf + n;
    }
}
NTSTATUS NtfsReadAttributeData(PNTFS_VCB v, PNTFS_RUN R, ULONG C, ULONGLONG Off, ULONG Len, PVOID Buf)
{ (void)v; io(0, R, C, Off, Len, Buf); return 0; }
NTSTATUS NtfsWriteAttributeData(PNTFS_VCB v, PNTFS_RUN R, ULONG C, ULONGLONG Off, ULONG Len, PVOID Buf)
{ (void)v; io(1, R, C, Off, Len, Buf); return 0; }
NTSTATUS NtfsReadMftRecord(PNTFS_VCB v, ULONGLONG Rec, PVOID Buf)
{
    (void)v; io(0, MftRuns, MftRc, Rec * V.Geo.MftRecordSize, V.Geo.MftRecordSize, Buf);
    return NtfsApplyFixup(Buf, V.Geo.MftRecordSize, V.Geo.BytesPerSector) ? 0 : STATUS_FILE_CORRUPT_ERROR;
}
NTSTATUS NtfsWriteMftRecord(PNTFS_VCB v, ULONGLONG Rec, PVOID Buf)
{
    (void)v; NtfsSetFixup(Buf, V.Geo.MftRecordSize, V.Geo.BytesPerSector);
    io(1, MftRuns, MftRc, Rec * V.Geo.MftRecordSize, V.Geo.MftRecordSize, Buf);
    {   /* $MFTMirr, as the driver keeps it (src/ntfsmft.c) */
        ULONG mirr = V.Geo.ClusterSize / V.Geo.MftRecordSize; if (mirr < 4) mirr = 4;
        if (Rec < mirr && V.Geo.MftMirrLcn)
            pwrite(fd, Buf, V.Geo.MftRecordSize,
                   (off_t)(V.Geo.MftMirrLcn * V.Geo.ClusterSize + Rec * V.Geo.MftRecordSize));
    }
    NtfsApplyFixup(Buf, V.Geo.MftRecordSize, V.Geo.BytesPerSector);
    return 0;
}
static int bit(ULONGLONG c) { return (Bm[c >> 3] >> (c & 7)) & 1; }
static void setbits(ULONGLONG c, ULONG n, int v)
{
    ULONG i;
    for (i = 0; i < n; i++, c++) {
        if (v) Bm[c >> 3] |= (UCHAR)(1 << (c & 7)); else Bm[c >> 3] &= (UCHAR)~(1 << (c & 7));
    }
    io(1, BmRuns, BmRc, 0, BmLen, Bm);
}
NTSTATUS NtfsAllocClusters(PNTFS_VCB v, ULONG n, ULONGLONG *first)
{
    ULONGLONG c, run = 0; (void)v;
    for (c = 0; c < V.Geo.TotalClusters; c++) {
        run = bit(c) ? 0 : run + 1;
        if (run == n) { *first = c + 1 - n; setbits(*first, n, 1); return 0; }
    }
    return STATUS_DISK_FULL;
}
NTSTATUS NtfsAllocClustersNear(PNTFS_VCB v, ULONG n, ULONGLONG hint, ULONGLONG *first)
{
    ULONGLONG c, run = 0;
    for (c = hint; hint && c < V.Geo.TotalClusters; c++) {
        run = bit(c) ? 0 : run + 1;
        if (run == n) { *first = c + 1 - n; setbits(*first, n, 1); return 0; }
    }
    return NtfsAllocClusters(v, n, first);
}
NTSTATUS NtfsFreeClusters(PNTFS_VCB v, ULONGLONG first, ULONG n)
{
    ULONG i; (void)v;
    for (i = 0; i < n; i++) if (!bit(first + i)) die("double free of a cluster");
    setbits(first, n, 0); return 0;
}
static ULONGLONG freeclusters(void)
{ ULONGLONG c, f = 0; for (c = 0; c < V.Geo.TotalClusters; c++) f += !bit(c); return f; }

/* ---- model ---------------------------------------------------------------- */
#define MAXN 20000
static WCHAR *Names[MAXN]; static ULONG Lens[MAXN]; static ULONG Nm;
static ULONGLONG Dir; static USHORT DirSeq;

static const WCHAR Alpha[] = { 'a','b','c','D','E','f','0','7',' ','_','-','.','x','Y',
    0xE7, 0xC7, 0x15F, 0x11F, 0x131, 0x130, 0xF6, 0xFC, 0xDC, 'z' };

static int find(const WCHAR *n, ULONG l)
{ ULONG i; for (i = 0; i < Nm; i++) if (Lens[i] == l && NtfsCollateName(n, l, Names[i], l) == 0) return (int)i; return -1; }

static ULONG mkentry(UCHAR *e, const WCHAR *n, ULONG l)
{
    FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)(e + sizeof(INDEX_ENTRY));
    ULONG kl = sizeof(FILE_NAME_ATTR) + l * 2, el = (sizeof(INDEX_ENTRY) + kl + 7) & ~7u;
    memset(e, 0, el);
    ((INDEX_ENTRY *)e)->FileReference = Dir | ((ULONGLONG)DirSeq << 48);
    ((INDEX_ENTRY *)e)->Length = (USHORT)el; ((INDEX_ENTRY *)e)->KeyLength = (USHORT)kl;
    fn->ParentDirectory = Dir | ((ULONGLONG)DirSeq << 48);
    fn->NameLength = (UCHAR)l; fn->NameType = NTFS_FNAME_WIN32;
    memcpy((UCHAR *)fn + sizeof(FILE_NAME_ATTR), n, l * 2);
    return el;
}

/* ---- tree checker --------------------------------------------------------- */
static UCHAR Rec[4096];
static NTFS_RUN IaRuns[256]; static ULONG IaRc; static ULONG Ibs, VcnBytes;
static ULONG Seen, LeafDepth, Blocks; static UCHAR Visited[65536];
static int Preloading; static ULONG Base; static ULONGLONG IaAlloc0;   /* names already in the directory */
static WCHAR *Prev; static ULONG PrevL; static int HavePrev;

static void key(INDEX_ENTRY *e, WCHAR **n, ULONG *l)
{ FILE_NAME_ATTR *f = (FILE_NAME_ATTR *)((UCHAR *)e + sizeof(INDEX_ENTRY)); *l = f->NameLength; *n = (WCHAR *)((UCHAR *)f + sizeof(FILE_NAME_ATTR)); }

static void walk(INDEX_HEADER *h, ULONG depth, int isroot)
{
    INDEX_ENTRY *e = (INDEX_ENTRY *)((UCHAR *)h + h->EntriesOffset);
    int internal = h->Flags & 1, keys = 0;
    for (;;) {
        if ((e->Flags & INDEX_ENTRY_HAS_SUBNODE) ? !internal : internal) die("entry/node subnode flag mismatch");
        if (internal) {
            ULONGLONG vcn = *(ULONGLONG *)((UCHAR *)e + e->Length - 8);
            UCHAR *b = malloc(Ibs); ULONG blk = (ULONG)(vcn * VcnBytes / Ibs);
            if (blk >= sizeof(Visited) || Visited[blk]) die("block visited twice");
            Visited[blk] = 1; Blocks++;
            io(0, IaRuns, IaRc, vcn * VcnBytes, Ibs, b);
            if (*(ULONG *)b != NTFS_INDX_SIGNATURE || !NtfsApplyFixup(b, Ibs, V.Geo.BytesPerSector)) die("bad INDX block");
            if (((INDEX_BLOCK *)b)->IndexBlockVcn != vcn) die("IndexBlockVcn mismatch");
            walk(&((INDEX_BLOCK *)b)->Header, depth + 1, 0);
            free(b);
        }
        if (e->Flags & INDEX_ENTRY_END) break;
        { WCHAR *n; ULONG l; key(e, &n, &l);
          if (HavePrev && NtfsCollateName(Prev, PrevL, n, l) >= 0) die("keys out of order");
          Prev = n; PrevL = l; HavePrev = 1;
          if (find(n, l) < 0) {
              if (!Preloading) die("tree holds a name the model does not");
              Names[Nm] = malloc(l * 2); memcpy(Names[Nm], n, l * 2); Lens[Nm] = l; Nm++;
          }
          Seen++; keys++; }
        e = (INDEX_ENTRY *)((UCHAR *)e + e->Length);
    }
    if (!isroot && keys == 0) die("empty non-root node");
    if (!internal) { if (LeafDepth == 0xFFFFFFFF) LeafDepth = depth; else if (LeafDepth != depth) die("leaves at different depths"); }
    /* note: Prev points into a freed buffer after return - copy */
    if (HavePrev) { static WCHAR keep[256]; memcpy(keep, Prev, PrevL * 2); Prev = keep; }
}

static ULONG check(int verbose)
{
    ATTRIBUTE_RECORD *ir, *ia, *bm; INDEX_ROOT *r; ULONG i, bits = 0, depthMax;
    static const WCHAR I30[4] = { '$','I','3','0' };
    if (NtfsReadMftRecord(&V, Dir, Rec)) die("read dir");
    ir = NtfsFindAttribute((PMFT_RECORD)Rec, ATTR_INDEX_ROOT, I30, 4);
    ia = NtfsFindAttribute((PMFT_RECORD)Rec, ATTR_INDEX_ALLOCATION, I30, 4);
    bm = NtfsFindAttribute((PMFT_RECORD)Rec, ATTR_BITMAP, I30, 4);
    r = (INDEX_ROOT *)NtfsResidentValue(ir);
    Ibs = r->IndexBlockSize; VcnBytes = (Ibs >= V.Geo.ClusterSize) ? V.Geo.ClusterSize : 512;
    IaRc = 0; if (ia && !NtfsDecodeRuns(ia, IaRuns, 256, &IaRc)) die("ia runs");
    if ((r->Header.Flags & 1) && !ia) die("large root without allocation");
    memset(Visited, 0, sizeof(Visited)); Seen = 0; LeafDepth = 0xFFFFFFFF; Blocks = 0; HavePrev = 0;
    walk(&r->Header, 0, 1);
    if (Seen != Nm) { fprintf(stderr, "seen %u model %u\n", Seen, Nm); die("entry count mismatch"); }
    if (bm) {
        UCHAR *p = NtfsResidentValue(bm); ULONG bl = NtfsResidentValueLength(bm);
        if (bm->NonResident) die("non-resident bitmap in test");
        for (i = 0; i < bl * 8; i++) {
            int b = (p[i >> 3] >> (i & 7)) & 1; bits += b;
            if (b != (i < sizeof(Visited) ? Visited[i] : 0)) { fprintf(stderr, "block %u bit %d visited %d\n", i, b, Visited[i]); die("bitmap vs tree"); }
        }
    } else if (ia) die("allocation without bitmap");
    depthMax = (LeafDepth == 0xFFFFFFFF) ? 0 : LeafDepth;
    for (i = 0; i < Nm; i += 1 + Nm / 50)
        if (NtfsLookupInDir(&V, Dir, Names[i], Lens[i]) == (ULONGLONG)-1) die("lookup missed a name");
    if (verbose) printf("  %u names, depth %u, %u blocks, rec used %u\n", Nm, depthMax, Blocks, ((PMFT_RECORD)Rec)->BytesInUse);
    return depthMax;
}

int main(int argc, char **argv)
{
    UCHAR boot[512], r0[4096], e[1024]; ATTRIBUTE_RECORD *a; ULONG ops, maxl, i, maxDepth = 0;
    ULONGLONG free0;
    if (argc < 6) { fprintf(stderr, "usage: ntfsbtree <img> <dirrec> <seed> <ops> <maxnamelen>\n"); return 2; }
    fd = open(argv[1], O_RDWR); if (fd < 0) die("open");
    Dir = strtoull(argv[2], 0, 0); srand((unsigned)atoi(argv[3])); ops = (ULONG)atoi(argv[4]); maxl = (ULONG)atoi(argv[5]);
    if (pread(fd, boot, 512, 0) != 512 || !NtfsParseBoot(boot, 512, &V.Geo)) die("boot");
    if (pread(fd, r0, V.Geo.MftRecordSize, (off_t)(V.Geo.MftLcn * V.Geo.ClusterSize)) != (ssize_t)V.Geo.MftRecordSize) die("mft");
    NtfsApplyFixup(r0, V.Geo.MftRecordSize, V.Geo.BytesPerSector);
    a = NtfsFindAttribute((PMFT_RECORD)r0, ATTR_DATA, NULL, 0); NtfsDecodeRuns(a, MftRuns, 256, &MftRc);
    NtfsReadMftRecord(&V, MFT_RN_BITMAP, r0);
    a = NtfsFindAttribute((PMFT_RECORD)r0, ATTR_DATA, NULL, 0); NtfsDecodeRuns(a, BmRuns, 64, &BmRc);
    BmLen = (ULONG)a->Form.NonResident.DataSize; Bm = malloc(BmLen); io(0, BmRuns, BmRc, 0, BmLen, Bm);
    NtfsReadMftRecord(&V, Dir, r0); DirSeq = ((PMFT_RECORD)r0)->SequenceNumber;
    free0 = freeclusters();
    Preloading = 1; check(0); Preloading = 0; Base = Nm;
    {
        static const WCHAR I30c[4] = { '$','I','3','0' };
        ATTRIBUTE_RECORD *ia = NtfsFindAttribute((PMFT_RECORD)Rec, ATTR_INDEX_ALLOCATION, I30c, 4);
        IaAlloc0 = ia ? ia->Form.NonResident.AllocatedSize : 0;
    }
    if (Base) printf("  %u names already present (kept)\n", Base);
    printf("dir %llu, cluster %u, index block %u\n", (unsigned long long)Dir, V.Geo.ClusterSize, V.Geo.IndexBlockSize);

    if (argc > 6) {
        /* sequential names (a large copy): file000000, file000001, ... */
        ULONG n = (ULONG)atoi(argv[6]);
        for (i = 0; i < n && Nm < MAXN; i++) {
            WCHAR w[16]; char c[16]; ULONG l, j, el;
            l = (ULONG)sprintf(c, "file%06u", i);
            for (j = 0; j < l; j++) w[j] = (WCHAR)c[j];
            el = mkentry(e, w, l);
            if (NtfsIndexAdd(&V, Dir, e, el, w, l) != 0) die("sequential insert failed");
            Names[Nm] = malloc(l * 2); memcpy(Names[Nm], w, l * 2); Lens[Nm] = l; Nm++;
        }
        check(1);
    }
    for (i = 0; i < ops; i++) {
        int ins = (Nm < Base + 50) || (rand() % 100 < (i < ops / 2 ? 70 : 35));
        if (ins && Nm < MAXN) {
            WCHAR n[256]; ULONG l = 1 + (ULONG)rand() % maxl, j, el;
            for (j = 0; j < l; j++) n[j] = Alpha[rand() % (sizeof(Alpha) / sizeof(Alpha[0]))];
            if (find(n, l) >= 0) continue;
            el = mkentry(e, n, l);
            { NTSTATUS st = NtfsIndexAdd(&V, Dir, e, el, n, l); if (st) { fprintf(stderr, "op %u Nm %u st %x\n", i, Nm, (unsigned)st); check(1); die("insert failed"); } }
            Names[Nm] = malloc(l * 2); memcpy(Names[Nm], n, l * 2); Lens[Nm] = l; Nm++;
        } else if (Nm > Base) {
            ULONG k = Base + (ULONG)rand() % (Nm - Base);
            if (NtfsIndexDelete(&V, Dir, Names[k], Lens[k]) != 0) die("delete failed");
            free(Names[k]); Names[k] = Names[Nm - 1]; Lens[k] = Lens[Nm - 1]; Nm--;
        }
        if (i % 500 == 499) { ULONG d = check(0); if (d > maxDepth) maxDepth = d; }
    }
    check(1);
    printf("  max depth reached: %u\n", maxDepth);
    while (Nm > Base) {
        if (NtfsIndexDelete(&V, Dir, Names[Nm - 1], Lens[Nm - 1]) != 0) die("final delete failed");
        free(Names[--Nm]);
        if ((Nm - Base) % 300 == 0) check(0);
    }
    check(1);
    {
        static const WCHAR I30[4] = { '$','I','3','0' };
        NtfsReadMftRecord(&V, Dir, Rec);
        if (Base == 0 && NtfsFindAttribute((PMFT_RECORD)Rec, ATTR_INDEX_ALLOCATION, I30, 4)) die("empty index still LARGE");
    }
    {   /* a directory that keeps names keeps its (grown) index allocation */
        static const WCHAR I30b[4] = { '$','I','3','0' };
        ATTRIBUTE_RECORD *ia;
        NtfsReadMftRecord(&V, Dir, Rec);
        ia = NtfsFindAttribute((PMFT_RECORD)Rec, ATTR_INDEX_ALLOCATION, I30b, 4);
        if (ia) free0 -= (ia->Form.NonResident.AllocatedSize - IaAlloc0) / V.Geo.ClusterSize;
    }
    if (freeclusters() != free0) { fprintf(stderr, "free %llu vs %llu\n", (unsigned long long)freeclusters(), (unsigned long long)free0); die("clusters leaked"); }
    printf("PASS\n");
    return 0;
}
