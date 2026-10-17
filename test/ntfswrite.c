/*
 * NTFS3NT - user-mode write-path check. Proves the record read-modify-write
 * mechanics (attribute edit + update-sequence fixup recompute + write back)
 * by overwriting a file's resident $DATA in place, through the same engine the
 * kernel driver uses. ntfs-3g then verifies the change is well-formed.
 *
 * Build: cc -I../src ntfswrite.c ../src/ntfssup.c -o ntfswrite
 * Run:   ./ntfswrite <image> <mft-record> <new-text>
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
static void rd(void *b, ULONG n, ULONGLONG o) { if (pread(g_fd,b,n,(off_t)o)!=(ssize_t)n) die("pread"); }
static void wr(const void *b, ULONG n, ULONGLONG o) { if (pwrite(g_fd,b,n,(off_t)o)!=(ssize_t)n) die("pwrite"); }

/* map a byte offset within a run list to an absolute disk offset (contiguous assumed within one cluster) */
static ULONGLONG map_vbo(PNTFS_RUN runs, ULONG cnt, ULONGLONG vbo)
{
    ULONG cs = g_geo.ClusterSize;
    ULONGLONG lcn, left; BOOLEAN sp;
    if (!NtfsMapVcn(runs, cnt, vbo / cs, &lcn, &left, &sp)) die("unmapped vbo");
    return lcn * (ULONGLONG)cs + (vbo % cs);
}

int main(int argc, char **argv)
{
    unsigned char boot[512], r0[4096], *rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    ULONGLONG rn, recVbo, diskOff;
    const char *text;
    ULONG vlen;

    if (argc < 4) { fprintf(stderr,"usage: %s <image> <rec> <text>\n",argv[0]); return 2; }
    g_fd = open(argv[1], O_RDWR);
    if (g_fd < 0) die("open");
    rn   = strtoull(argv[2], 0, 0);
    text = argv[3];

    rd(boot, sizeof(boot), 0);
    if (!NtfsParseBoot(boot, sizeof(boot), &g_geo)) die("not NTFS");

    /* bootstrap $MFT runs from record 0 */
    rd(r0, g_geo.MftRecordSize, g_geo.MftLcn * (ULONGLONG)g_geo.ClusterSize);
    if (!NtfsApplyFixup(r0, g_geo.MftRecordSize, g_geo.BytesPerSector)) die("rec0 fixup");
    a = NtfsFindAttribute((PMFT_RECORD)r0, ATTR_DATA, NULL, 0);
    if (!a || !a->NonResident) die("$MFT data");
    if (!NtfsDecodeRuns(a, g_mftRuns, 256, &g_mftRunCount)) die("mft runs");

    /* read the target record */
    rec = malloc(g_geo.MftRecordSize);
    recVbo = rn * (ULONGLONG)g_geo.MftRecordSize;
    diskOff = map_vbo(g_mftRuns, g_mftRunCount, recVbo);
    rd(rec, g_geo.MftRecordSize, diskOff);
    if (!NtfsApplyFixup(rec, g_geo.MftRecordSize, g_geo.BytesPerSector)) die("rec fixup");
    m = (PMFT_RECORD)rec;

    a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    if (!a) die("no $DATA");
    if (a->NonResident) die("$DATA not resident (this test only does in-place resident edits)");

    vlen = NtfsResidentValueLength(a);
    if ((ULONG)strlen(text) != vlen)
        fprintf(stderr, "note: new text %zu bytes, resident value %u bytes; writing min\n",
                strlen(text), vlen);
    {
        ULONG n = (ULONG)strlen(text);
        if (n > vlen) n = vlen;
        memcpy(NtfsResidentValue(a), text, n);
    }

    /* write the record back with a fresh fixup */
    NtfsSetFixup(rec, g_geo.MftRecordSize, g_geo.BytesPerSector);
    wr(rec, g_geo.MftRecordSize, diskOff);
    fsync(g_fd);
    close(g_fd);
    printf("wrote record %llu in place\n", (unsigned long long)rn);
    return 0;
}
