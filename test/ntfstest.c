/*
 * NTFS3NT - user-mode read-path test for the shared NTFS logic (ntfssup.c).
 * Mounts a real NTFS image (made with mkntfs), reads $MFT, walks system
 * records, enumerates the root directory index and reads a file's data - all
 * through the same parsing code the kernel driver uses.
 *
 * Build:  cc -I../src ntfstest.c ../src/ntfssup.c -o ntfstest
 * Run:    ./ntfstest <image>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "ntfsusr.h"
#include "ntfssup.h"

static int g_fd;
static NTFS_GEOMETRY g_geo;
static NTFS_RUN g_mftRuns[256];
static ULONG g_mftRunCount;

static void die(const char *m) { fprintf(stderr, "ERROR: %s\n", m); exit(1); }

static void read_at(void *buf, ULONG len, ULONGLONG off)
{
    ssize_t r = pread(g_fd, buf, len, (off_t)off);
    if (r != (ssize_t)len) die("pread short/failed");
}

/* Read `len` bytes starting at byte offset `vbo` within a run list. */
static int read_runs(PNTFS_RUN runs, ULONG count, ULONGLONG vbo, void *buf, ULONG len)
{
    PUCHAR out = (PUCHAR)buf;
    ULONG cs = g_geo.ClusterSize;

    while (len > 0) {
        ULONGLONG vcn = vbo / cs;
        ULONG inClu = (ULONG)(vbo % cs);
        ULONGLONG lcn, left;
        BOOLEAN sparse;
        ULONG chunk;

        if (!NtfsMapVcn(runs, count, vcn, &lcn, &left, &sparse)) {
            if (sparse) {               /* hole reads as zero */
                chunk = cs - inClu;
                if (chunk > len) chunk = len;
                memset(out, 0, chunk);
                out += chunk; vbo += chunk; len -= chunk;
                continue;
            }
            return 0;                   /* unmapped */
        }
        chunk = cs - inClu;
        if (chunk > len) chunk = len;
        read_at(out, chunk, lcn * (ULONGLONG)cs + inClu);
        out += chunk; vbo += chunk; len -= chunk;
    }
    return 1;
}

/* Read MFT record `rn` into buf (MftRecordSize bytes) and apply fixups. */
static int read_mft_record(ULONGLONG rn, void *buf)
{
    ULONGLONG vbo = rn * (ULONGLONG)g_geo.MftRecordSize;
    if (!read_runs(g_mftRuns, g_mftRunCount, vbo, buf, g_geo.MftRecordSize)) {
        return 0;
    }
    return NtfsApplyFixup(buf, g_geo.MftRecordSize, g_geo.BytesPerSector);
}

/* Print a UTF-16 name as ASCII (test output only). */
static void print_name(const WCHAR *name, int n)
{
    int i;
    for (i = 0; i < n; i++) putchar(name[i] < 128 ? (char)name[i] : '?');
}

int main(int argc, char **argv)
{
    unsigned char boot[512];
    unsigned char *rec, *root, *indx;
    PATTRIBUTE_RECORD a;
    PMFT_RECORD mrec;
    ULONGLONG rn;
    int entries;
    static const WCHAR I30[4] = { '$', 'I', '3', '0' };   /* directory index name */

    if (argc < 2) { fprintf(stderr, "usage: %s <image>\n", argv[0]); return 2; }
    g_fd = open(argv[1], O_RDONLY);
    if (g_fd < 0) die("open image");

    /* 1. boot sector */
    read_at(boot, sizeof(boot), 0);
    if (!NtfsParseBoot(boot, sizeof(boot), &g_geo)) die("not NTFS / bad boot");
    printf("== boot ==\n");
    printf("  bytes/sector   %u\n", g_geo.BytesPerSector);
    printf("  sectors/clust  %u  (cluster %u)\n", g_geo.SectorsPerCluster, g_geo.ClusterSize);
    printf("  MFT record     %u\n", g_geo.MftRecordSize);
    printf("  index block    %u\n", g_geo.IndexBlockSize);
    printf("  total clusters %llu\n", (unsigned long long)g_geo.TotalClusters);
    printf("  $MFT LCN       %llu\n", (unsigned long long)g_geo.MftLcn);

    rec  = malloc(g_geo.MftRecordSize);
    root = malloc(g_geo.MftRecordSize);
    if (!rec || !root) die("oom");

    /* 2. MFT record 0 -> $MFT $DATA run list (bootstrap) */
    {
        unsigned char r0[4096];
        if (g_geo.MftRecordSize > sizeof(r0)) die("record too big");
        read_at(r0, g_geo.MftRecordSize, g_geo.MftLcn * (ULONGLONG)g_geo.ClusterSize);
        if (!NtfsApplyFixup(r0, g_geo.MftRecordSize, g_geo.BytesPerSector))
            die("MFT rec0 fixup");
        mrec = (PMFT_RECORD)r0;
        a = NtfsFindAttribute(mrec, ATTR_DATA, NULL, 0);
        if (!a || !a->NonResident) die("$MFT $DATA not non-resident");
        if (!NtfsDecodeRuns(a, g_mftRuns, 256, &g_mftRunCount)) die("decode MFT runs");
        printf("== $MFT ==\n  data size %llu, %u run(s)\n",
               (unsigned long long)a->Form.NonResident.DataSize, g_mftRunCount);
    }

    /* 3. walk system records 0..15: print name + flags */
    printf("== system records ==\n");
    for (rn = 0; rn < 16; rn++) {
        if (!read_mft_record(rn, rec)) { printf("  [%2llu] <unreadable>\n", (unsigned long long)rn); continue; }
        mrec = (PMFT_RECORD)rec;
        if (mrec->Rhdr.Type != NTFS_FILE_SIGNATURE) continue;
        if (!(mrec->Flags & MFT_RECORD_IN_USE)) { printf("  [%2llu] <free>\n", (unsigned long long)rn); continue; }
        a = NtfsFindAttribute(mrec, ATTR_FILE_NAME, NULL, 0);
        printf("  [%2llu] %s  ", (unsigned long long)rn,
               (mrec->Flags & MFT_RECORD_IS_DIRECTORY) ? "<DIR> " : "<file>");
        if (a) {
            PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)NtfsResidentValue(a);
            print_name((WCHAR *)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)), fn->NameLength);
        }
        putchar('\n');
    }

    /* 4. root directory (record 5): enumerate its index */
    printf("== root directory ==\n");
    if (!read_mft_record(MFT_RN_ROOT, root)) die("read root");
    mrec = (PMFT_RECORD)root;
    entries = 0;

    a = NtfsFindAttribute(mrec, ATTR_INDEX_ROOT, I30, 4);
    if (a) {
        PINDEX_ROOT ir = (PINDEX_ROOT)NtfsResidentValue(a);
        PINDEX_ENTRY e;
        for (e = NtfsFirstIndexEntry(&ir->Header); e; e = NtfsNextIndexEntry(e)) {
            PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)((PUCHAR)e + sizeof(INDEX_ENTRY));
            if (e->KeyLength && fn->NameType != NTFS_FNAME_DOS) {
                printf("  "); print_name((WCHAR *)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)), fn->NameLength);
                printf("  (rec %llu, %llu bytes)\n",
                       (unsigned long long)NTFS_MREF_RECORD(e->FileReference),
                       (unsigned long long)fn->DataSize);
                entries++;
            }
        }
    }

    /* 5. large index: read $INDEX_ALLOCATION blocks via its run list */
    a = NtfsFindAttribute(mrec, ATTR_INDEX_ALLOCATION, I30, 4);
    if (a && a->NonResident) {
        NTFS_RUN iruns[64];
        ULONG ic, blk;
        ULONGLONG nblocks;
        indx = malloc(g_geo.IndexBlockSize);
        if (!indx) die("oom");
        if (NtfsDecodeRuns(a, iruns, 64, &ic)) {
            nblocks = a->Form.NonResident.DataSize / g_geo.IndexBlockSize;
            for (blk = 0; blk < nblocks; blk++) {
                PINDEX_BLOCK ib = (PINDEX_BLOCK)indx;
                PINDEX_ENTRY e;
                if (!read_runs(iruns, ic, (ULONGLONG)blk * g_geo.IndexBlockSize,
                               indx, g_geo.IndexBlockSize)) continue;
                if (ib->Rhdr.Type != NTFS_INDX_SIGNATURE) continue;
                if (!NtfsApplyFixup(indx, g_geo.IndexBlockSize, g_geo.BytesPerSector)) continue;
                for (e = NtfsFirstIndexEntry(&ib->Header); e; e = NtfsNextIndexEntry(e)) {
                    PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)((PUCHAR)e + sizeof(INDEX_ENTRY));
                    if (e->KeyLength && fn->NameType != NTFS_FNAME_DOS) {
                        printf("  "); print_name((WCHAR *)((PUCHAR)fn + sizeof(FILE_NAME_ATTR)), fn->NameLength);
                        printf("  (rec %llu, %llu bytes)\n",
                               (unsigned long long)NTFS_MREF_RECORD(e->FileReference),
                               (unsigned long long)fn->DataSize);
                        entries++;
                    }
                }
            }
        }
        free(indx);
    }
    printf("  %d entr%s\n", entries, entries == 1 ? "y" : "ies");

    /* 6. read file data: resident (hello.txt, rec 64) and non-resident ($UpCase) */
    printf("== file read ==\n");
    if (read_mft_record(64, rec)) {
        mrec = (PMFT_RECORD)rec;
        a = NtfsFindAttribute(mrec, ATTR_DATA, NULL, 0);
        if (a && !a->NonResident) {
            ULONG vl = NtfsResidentValueLength(a);
            printf("  rec 64 $DATA resident, %u bytes: \"", vl);
            fwrite(NtfsResidentValue(a), 1, vl > 60 ? 60 : vl, stdout);
            printf("\"\n");
        } else if (a) {
            printf("  rec 64 $DATA non-resident, %llu bytes\n",
                   (unsigned long long)a->Form.NonResident.DataSize);
        }
    }
    if (read_mft_record(MFT_RN_UPCASE, rec)) {
        mrec = (PMFT_RECORD)rec;
        a = NtfsFindAttribute(mrec, ATTR_DATA, NULL, 0);
        if (a && a->NonResident) {
            NTFS_RUN uruns[32]; ULONG uc; unsigned char head[16];
            if (NtfsDecodeRuns(a, uruns, 32, &uc) &&
                read_runs(uruns, uc, 0, head, sizeof(head))) {
                printf("  $UpCase $DATA non-resident, %llu bytes, %u run(s); first upcase U+0000=%04X U+0001=%04X\n",
                       (unsigned long long)a->Form.NonResident.DataSize, uc,
                       head[0] | (head[1] << 8), head[2] | (head[3] << 8));
            }
        }
    }

    printf("\nALL CHECKS RAN\n");
    return 0;
}
