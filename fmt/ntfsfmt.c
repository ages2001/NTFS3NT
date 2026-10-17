/*
 * NTFS3NT - ntfsfmt: lay out a new NTFS 3.1 volume from scratch (the
 * mkntfs-equivalent). Produces a volume Windows NT 3.1 - 2000/XP and ntfs-3g
 * mount: boot sector, the system metadata files ($MFT, $MFTMirr, $LogFile,
 * $Volume, $AttrDef, root, $Bitmap, $Boot, $BadClus, $Secure, $UpCase,
 * $Extend), the cluster bitmap and the MFT record bitmap, plus a standard
 * $AttrDef and a generated $UpCase table.
 *
 * Dual build:
 *   Linux test:  cc -I../src ntfsfmt.c -o ntfsfmt      (writes an image file)
 *   NT tool:     built CRT-free like ntfschk/ntfsinst (formats a volume/file)
 *
 * Usage: ntfsfmt <image-or-device> [size-in-MB] [/L:label]
 * (On a real NT partition the size is taken from the volume; the size arg is
 *  used when creating/formatting an image file.)
 */

#include "ntfsfmtdata.h"

/* ---- environment: Win32 console tool vs. Linux test build ---------------- */
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
typedef unsigned __int64 u64;
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
typedef uint64_t u64;
#endif
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---- little-endian stores -------------------------------------------- */
static void p16(u8 *b, u16 v){ b[0]=(u8)v; b[1]=(u8)(v>>8); }
static void p32(u8 *b, u32 v){ b[0]=(u8)v; b[1]=(u8)(v>>8); b[2]=(u8)(v>>16); b[3]=(u8)(v>>24); }
static void p64(u8 *b, u64 v){ p32(b,(u32)v); p32(b+4,(u32)(v>>32)); }

/* ---- NTFS constants --------------------------------------------------- */
#define SECTOR        512
#define IBS           4096u   /* index block size (always 4 KB) */
/* clusters-per-index-block field: clusters when a block spans >= 1 cluster,
   else 512-byte units (clusters larger than 4 KB) */
#define IB_CPB(cl)    ((u8)(((cl) <= IBS) ? IBS / (cl) : IBS / 512))
/* $MFTMirr: 4 records, or a whole cluster of them when a cluster holds more */
#define MIRR_BYTES(cl) (((cl) > 4u * MREC) ? (u32)(cl) : 4u * MREC)
#define FILE_SIG      0x454C4946u     /* "FILE" */
#define MREC          1024            /* MFT record size */

/* attribute type codes */
#define A_SI    0x10
#define A_AL    0x20
#define A_FN    0x30
#define A_OBJID 0x40
#define A_SD    0x50
#define A_VNAME 0x60
#define A_VINFO 0x70
#define A_DATA  0x80
#define A_IROOT 0x90
#define A_IALLOC 0xA0
#define A_BITMAP 0xB0
#define A_END   0xFFFFFFFFu

#define F_INUSE 0x0001
#define F_DIR   0x0002

/* system record numbers */
enum { R_MFT=0,R_MFTMIRR,R_LOGFILE,R_VOLUME,R_ATTRDEF,R_ROOT,R_BITMAP,R_BOOT,
       R_BADCLUS,R_SECURE,R_UPCASE,R_EXTEND, N_SYS=16,
       R_QUOTA=24, R_OBJID=25, R_REPARSE=26 };
/* $FILE_NAME attributes of the $Extend view-index files: hidden, system,
 * archive, "view index present" */
#define EXT_FATTR 0x20000026u

/* ---- volume geometry and metadata layout (filled by fmt_compute) ------- */
typedef struct {
    u64 total_sectors;
    u32 spc;              /* sectors per cluster */
    u32 cluster;          /* bytes per cluster   */
    u64 total_clusters;
    u64 serial;
    /* metadata cluster counts */
    u32 mft_clusters, log_clusters, upcase_clusters, attrdef_clusters, bitmap_clusters, sds_clusters;
    /* metadata LCNs (contiguous, from the low clusters) */
    u64 boot_lcn, mft_lcn, mftbmp_lcn, mftmirr_lcn, logfile_lcn,
        attrdef_lcn, upcase_lcn, bitmap_lcn, root_indx_lcn, sds_lcn;
    u64 meta_clusters;    /* total clusters occupied by all metadata */
    u8  bootsec[512];     /* the boot sector (also backed up at volume end) */
} GEO;

/* A buffer-backed writer so the same code serves the Linux and NT builds. */
typedef struct { u8 *base; u64 bytes; } VOL;

static void vwrite(VOL *v, u64 off, const void *data, u32 len)
{
    u32 i; const u8 *s=(const u8*)data;
    if (off+len > v->bytes) return;
    for (i=0;i<len;i++) v->base[off+i]=s[i];
}
static void vzero(VOL *v, u64 off, u64 len)
{
    u64 i; for(i=0;i<len;i++){ if(off+i<v->bytes) v->base[off+i]=0; }
}

/* ---- the volume's single security descriptor ($Secure, id 0x100) --------
 * Owner/group BUILTIN\Administrators, DACL: Everyone full control, inherited
 * by files and subdirectories (what a fresh NT volume effectively grants). */
static const u8 SdDefault[80] = {
    1,0, 0x04,0x80, 48,0,0,0, 64,0,0,0, 0,0,0,0, 20,0,0,0,      /* header */
    2,0, 28,0, 1,0, 0,0,                                         /* ACL */
    0,0x03, 20,0, 0xFF,0x01,0x1F,0x00,                           /* ACE allow OI|CI */
    1,1, 0,0,0,0,0,1, 0,0,0,0,                                   /* S-1-1-0 */
    1,2, 0,0,0,0,0,5, 32,0,0,0, 0x20,0x02,0,0,                   /* owner S-1-5-32-544 */
    1,2, 0,0,0,0,0,5, 32,0,0,0, 0x20,0x02,0,0                    /* group */
};
#define SDS_ENTRY   112                       /* 20-byte header + 80, 16-aligned */
#define SDS_SIZE    (0x40000 + SDS_ENTRY)     /* entry + its mirror 256 KB later */
static u32 sd_hash(const u8 *sd, u32 len)
{
    u32 h = 0, i;
    for (i = 0; i + 4 <= len; i += 4) {
        u32 w = sd[i] | (sd[i+1]<<8) | (sd[i+2]<<16) | ((u32)sd[i+3]<<24);
        h = w + ((h >> 29) | (h << 3));
    }
    return h;
}

/* format time (FILETIME) stamped into every system file; set by main() */
static u64 g_now = 0x01DC7A0000000000ULL;
#define SECID_SYS 0x100           /* the one security descriptor in $Secure */

/* ---- MFT record builder ---------------------------------------------- */
typedef struct {
    u8  rec[MREC];
    u32 used;        /* first free offset (attributes appended here) */
    u16 nextid;      /* next attribute instance number (unique per record) */
} REC;

static void rec_init(REC *r, u64 recnum, u16 flags, u16 seq)
{
    u32 i; for(i=0;i<MREC;i++) r->rec[i]=0;
    p32(r->rec+0x00, FILE_SIG);
    p16(r->rec+0x06, 3);            /* USA count = 1 + MREC/SECTOR = 3 for 1024 */
    p16(r->rec+0x04, 0x30);         /* USA offset */
    p64(r->rec+0x08, 0);            /* $LogFile seq */
    p16(r->rec+0x10, seq);          /* sequence number */
    p16(r->rec+0x12, 1);            /* hard link count */
    p16(r->rec+0x14, 0x38);         /* first attribute offset */
    p16(r->rec+0x16, flags);        /* flags */
    /* 0x18 bytes-in-use set at finalize; 0x1C allocated size */
    p32(r->rec+0x1C, MREC);
    p64(r->rec+0x20, 0);            /* base record */
    p16(r->rec+0x28, 0);            /* next attr id (set at finalize) */
    p32(r->rec+0x2C, (u32)recnum);  /* record number */
    /* USA signature at 0x30 (2 bytes) + 2 fixup entries at 0x32,0x34 */
    p16(r->rec+0x30, 1);            /* USN */
    r->used = 0x38;
    r->nextid = 0;
}

/* append a resident attribute; returns pointer to its value area */
static u8 *attr_res(REC *r, u32 type, const void *val, u32 vlen,
                    const u16 *name, u32 namelen)
{
    u8 *a = r->rec + r->used;
    u32 nameoff = 0x18;
    u32 valoff  = nameoff + namelen*2;
    u32 reclen;
    u32 i;
    valoff = (valoff + 7) & ~7u;
    reclen = (valoff + vlen + 7) & ~7u;
    p32(a+0x00, type);
    p32(a+0x04, reclen);
    a[0x08]=0;                      /* resident */
    a[0x09]=(u8)namelen;
    p16(a+0x0A, (u16)nameoff);      /* Windows sets it even when unnamed */
    p16(a+0x0C, 0);                 /* flags */
    p16(a+0x0E, r->nextid++);       /* unique attribute instance */
    p32(a+0x10, vlen);             /* value length */
    p16(a+0x14, (u16)valoff);      /* value offset */
    a[0x16]=(u8)(type==A_FN ? 1 : 0); /* $FILE_NAME is always indexed */
    a[0x17]=0;
    for(i=0;i<namelen;i++) p16(a+nameoff+i*2, name[i]);
    if (val) for(i=0;i<vlen;i++) a[valoff+i]=((const u8*)val)[i];
    r->used += reclen;
    return a+valoff;
}

/* append a full NTFS 3.x $STANDARD_INFORMATION (0x48 bytes): format time,
 * the given attributes and security id */
static void attr_si(REC *r, u32 fattr, u32 secid)
{
    u8 val[0x48]; u32 i;
    for(i=0;i<0x48;i++) val[i]=0;
    p64(val+0x00,g_now); p64(val+0x08,g_now); p64(val+0x10,g_now); p64(val+0x18,g_now);
    p32(val+0x20,fattr);
    p32(val+0x34,secid);
    attr_res(r,A_SI,val,0x48,0,0);
}

/* append a non-resident attribute with a single-run mapping pair */
static void attr_nonres(REC *r, u32 type, u64 start_vcn, u64 last_vcn,
                        u64 alloc_size, u64 data_size, u64 lcn, u64 clusters,
                        const u16 *name, u32 namelen, u16 flags)
{
    u8 *a = r->rec + r->used;
    u32 nameoff = 0x40;
    u32 runoff  = nameoff + namelen*2;
    u8  runs[32]; u32 rl=0;
    u32 reclen, i;
    /* build one mapping pair: length then LCN (signed, first run absolute) */
    {
        u8 lb[8], cb[8]; u32 ln=0, cn=0; u64 t;
        t=clusters; do{ lb[ln++]=(u8)t; t>>=8; }while(t);
        if (lb[ln-1] & 0x80) lb[ln++]=0;   /* run lengths are signed */
        { long long sl=(long long)lcn; u64 uc=(u64)sl;
          do{ cb[cn++]=(u8)uc; uc=(u64)((long long)uc>>8);}while(!((cn>=1)&&((cb[cn-1]&0x80)==0&&(sl>>(8*cn))==0)) && cn<8);
        }
        runs[rl++]=(u8)((cn<<4)|ln);
        for(i=0;i<ln;i++) runs[rl++]=lb[i];
        for(i=0;i<cn;i++) runs[rl++]=cb[i];
        runs[rl++]=0;              /* run list terminator */
    }
    runoff = (runoff + 7) & ~7u;
    reclen = (runoff + rl + 7) & ~7u;
    p32(a+0x00, type);
    p32(a+0x04, reclen);
    a[0x08]=1;                     /* non-resident */
    a[0x09]=(u8)namelen;
    p16(a+0x0A, (u16)nameoff);
    p16(a+0x0C, flags);
    p16(a+0x0E, r->nextid++);
    p64(a+0x10, start_vcn);
    p64(a+0x18, last_vcn);
    p16(a+0x20, (u16)runoff);
    a[0x22]=0;                     /* compression unit */
    p64(a+0x28, alloc_size);
    p64(a+0x30, data_size);
    p64(a+0x38, data_size);       /* initialized size */
    for(i=0;i<namelen;i++) p16(a+nameoff+i*2, name[i]);
    for(i=0;i<rl;i++) a[runoff+i]=runs[i];
    r->used += reclen;
}

static void rec_end(REC *r)
{
    u32 off = r->used;
    p32(r->rec+off, A_END);        /* end marker */
    p32(r->rec+off+4, 0);
    off += 8;
    p32(r->rec+0x18, off);         /* bytes in use */
    p16(r->rec+0x28, r->nextid);   /* next attribute instance */
}

/* apply the update sequence (fixup): save last 2 bytes of each 512 sector into
 * the USA, write the USN into each sector's last word. */
static void rec_fixup(REC *r)
{
    u16 usn; u32 s;
    usn = (u16)(r->rec[0x30] | (r->rec[0x31]<<8));
    for (s=0; s<MREC/SECTOR; s++) {
        u8 *tail = r->rec + s*SECTOR + SECTOR-2;
        r->rec[0x32 + s*2]     = tail[0];     /* save original */
        r->rec[0x32 + s*2 + 1] = tail[1];
        tail[0]=(u8)usn; tail[1]=(u8)(usn>>8);
    }
}

/* ---- $FILE_NAME value builder ---------------------------------------- */
static u32 build_fn(u8 *out, u64 parent, u16 seq, const char *name, u32 flags)
{
    u32 n=0, i; u8 nl=0;
    p64(out+0x00, (parent & 0xFFFFFFFFFFFFULL) | ((u64)seq<<48));
    p64(out+0x08, g_now); p64(out+0x10,g_now); p64(out+0x18,g_now); p64(out+0x20,g_now); /* times */
    p64(out+0x28, 0);              /* alloc size */
    p64(out+0x30, 0);              /* data size */
    p32(out+0x38, flags);
    p32(out+0x3C, 0);
    for(i=0;name[i];i++) nl++;
    out[0x40]=nl;
    out[0x41]=3;                   /* namespace: Win32+DOS */
    for(i=0;i<nl;i++) p16(out+0x42+i*2, (u16)name[i]);
    n = 0x42 + nl*2;
    return n;
}

/* ---- root directory index ($I30) ------------------------------------- *
 * The root index holds entries for the system metadata files, exactly like a
 * real NTFS volume, so ntfs-3g (and tools) can resolve "$Secure", "$Extend"
 * etc. by path. They do not fit resident, so the index is a LARGE index: the
 * $INDEX_ROOT carries only an END entry pointing at VCN 0 of an INDX block in
 * $INDEX_ALLOCATION, and that INDX block holds the sorted entries.          */

/* one INDEX_ENTRY (header + embedded $FILE_NAME key); returns padded length */
static u32 idx_entry(u8 *out, u64 recno, u16 seq, const char *name, u32 fflags)
{
    u8 key[256];
    u32 klen = build_fn(key, R_ROOT, 5, name, fflags);
    u32 elen = (0x10 + klen + 7) & ~7u;
    u32 i;
    for(i=0;i<elen;i++) out[i]=0;
    p64(out+0x00, (recno & 0xFFFFFFFFFFFFULL) | ((u64)seq<<48));
    p16(out+0x08, (u16)elen);
    p16(out+0x0A, (u16)klen);
    p16(out+0x0C, 0);              /* leaf, not last */
    for(i=0;i<klen;i++) out[0x10+i]=key[i];
    return elen;
}

/* build the single INDX block for the root directory into buf (cluster sized) */
static void build_root_indx(u8 *buf, u32 blocksize)
{
    /* system files in NTFS filename-collation order */
    static const struct { int rn; u16 seq; const char *nm; } sys[] = {
        /* sequence numbers equal the record numbers (record 0 uses 1) */
        {4, 4,"$AttrDef"}, {8, 8,"$BadClus"}, {6, 6,"$Bitmap"}, {7, 7,"$Boot"},
        {11,11,"$Extend"}, {2, 2,"$LogFile"}, {0, 1,"$MFT"},    {1, 1,"$MFTMirr"},
        {9, 9,"$Secure"},  {10,10,"$UpCase"}, {3, 3,"$Volume"}, {5, 5, "."}
    };
    u32 nsys = (u32)(sizeof(sys)/sizeof(sys[0]));
    u32 usn_count = blocksize/SECTOR + 1;
    u32 entries_off = (0x18 + 0x10 + usn_count*2 + 7) & ~7u; /* abs offset */
    u32 off = entries_off;
    u32 i, j, k;
    u16 usn = 1;

    for(i=0;i<blocksize;i++) buf[i]=0;
    buf[0]='I'; buf[1]='N'; buf[2]='D'; buf[3]='X';
    p16(buf+0x04, 0x28);                       /* USA offset */
    p16(buf+0x06, (u16)usn_count);
    p64(buf+0x08, 0);                          /* LSN */
    p64(buf+0x10, 0);                          /* this block's VCN = 0 */
    /* INDEX_HEADER at 0x18 (offsets relative to 0x18) */
    p32(buf+0x18, entries_off - 0x18);         /* entries offset */
    /* 0x1C index_length, 0x20 allocated_size set below */
    p32(buf+0x20, blocksize - 0x18);           /* allocated size of entries area */
    buf[0x24]=0;                               /* flags: leaf node */

    /* emit sorted system-file entries */
    for(k=0;k<nsys;k++)
        off += idx_entry(buf+off, (u64)sys[k].rn, sys[k].seq, sys[k].nm,
                         (sys[k].rn==5 || sys[k].rn==11) ? 0x10000006u : 0x06u);
    /* trailing END entry (leaf: no VCN) */
    { u8 *e=buf+off; p16(e+0x08, 0x10); p16(e+0x0A,0); p16(e+0x0C, 0x02); off += 0x10; }

    p32(buf+0x1C, off - 0x18);                 /* index_length (incl header) */

    /* USA fixup across the INDX block */
    p16(buf+0x28, usn);
    for(j=0;j<blocksize/SECTOR;j++){
        u8 *tail = buf + j*SECTOR + SECTOR-2;
        buf[0x2A + j*2]     = tail[0];
        buf[0x2A + j*2 + 1] = tail[1];
        tail[0]=(u8)usn; tail[1]=(u8)(usn>>8);
    }
}

/* Start the MFT at 64 records: records 0-15 are the system files, 16-63 are
 * pre-initialized free records so a writer (ntfs-3g / our write path) can
 * allocate new files without first extending $MFT. */
#define MFT_RECS 64

/* ---- the formatter ---------------------------------------------------- */
static void fmt_compute(GEO *g, u64 total_sectors, u32 cluster)
{
    u64 cl, next, log_bytes, bitmap_bytes;

    g->total_sectors = total_sectors;
    g->spc = cluster / SECTOR;     /* 512 B .. 64 KB clusters (default 4 KB) */
    g->cluster = g->spc * SECTOR;
    cl = g->cluster;
    /* the boot sector's count excludes the backup boot sector (last sector) */
    g->total_clusters = (total_sectors - 1) / g->spc;
    g->serial = 0x4e54465300000001ULL ^ total_sectors;

    /* metadata sizes */
    g->mft_clusters     = (MFT_RECS*MREC + (u32)cl - 1)/(u32)cl;
    g->upcase_clusters  = (131072 + (u32)cl -1)/(u32)cl;      /* 128 KB */
    g->attrdef_clusters = (2560 + (u32)cl -1)/(u32)cl;
    bitmap_bytes        = (g->total_clusters + 7)/8;
    g->bitmap_clusters  = (u32)((bitmap_bytes + cl -1)/cl);
    /* $LogFile: ~volume/64, clamped to [2MB, 16MB] */
    log_bytes = (g->total_clusters * cl) / 64;
    if (log_bytes < 0x200000)  log_bytes = 0x200000;    /* 2 MB, as NT 5 uses on small volumes */
    if (log_bytes > 0x1000000) log_bytes = 0x1000000;
    g->log_clusters = (u32)((log_bytes + cl - 1)/cl);

    /* contiguous allocation from the low clusters */
    g->boot_lcn      = 0;                               /* $Boot (2 clusters) */
    next             = (0x2000 + cl - 1)/cl;            /* 8 KB boot area */
    g->mft_lcn       = next; next += g->mft_clusters;
    g->mftbmp_lcn    = next; next += 1;                 /* $MFT:$BITMAP */
    g->mftmirr_lcn   = next; next += (MIRR_BYTES(cl) + cl - 1)/cl;  /* mirror */
    g->logfile_lcn   = next; next += g->log_clusters;
    g->attrdef_lcn   = next; next += g->attrdef_clusters;
    g->upcase_lcn    = next; next += g->upcase_clusters;
    g->bitmap_lcn    = next; next += g->bitmap_clusters;
    g->root_indx_lcn = next; next += (IBS + cl - 1)/cl;  /* root $I30 INDX block */
    g->sds_clusters  = (u32)((SDS_SIZE + cl - 1)/cl);
    g->sds_lcn       = next; next += g->sds_clusters;   /* $Secure:$SDS */
    g->meta_clusters = next;
}

/*
 * Lay out the metadata region in v->base (which spans clusters [0, meta_clusters)
 * only; the backup boot sector is returned in g->bootsec for the caller to write
 * at the end of the volume). Returns 0 on ok.
 */
static int fmt_build(VOL *v, GEO *g, const char *label)
{
    u64 cl = g->cluster;
    u64 next          = g->meta_clusters;
    u64 mft_lcn       = g->mft_lcn;
    u64 mftmirr_lcn   = g->mftmirr_lcn;
    u64 logfile_lcn   = g->logfile_lcn;
    u64 upcase_lcn    = g->upcase_lcn;
    u64 attrdef_lcn   = g->attrdef_lcn;
    u64 bitmap_lcn    = g->bitmap_lcn;
    u64 boot_lcn      = g->boot_lcn;
    u64 root_indx_lcn = g->root_indx_lcn;
    u64 mftbmp_lcn    = g->mftbmp_lcn;
    u32 mft_clusters  = g->mft_clusters;
    u32 log_clusters  = g->log_clusters;
    u32 upcase_clusters = g->upcase_clusters;
    u32 attrdef_clusters = g->attrdef_clusters;
    u32 bitmap_clusters = g->bitmap_clusters;
    REC r;
    u8 fn[256]; u32 fnlen;
    u8 val[256];
    int i;

    /* --- boot sector ($Boot data, LCN 0) --- */
    {
        u8 b[SECTOR]; for(i=0;i<SECTOR;i++) b[i]=0;
        b[0]=0xEB; b[1]=0x52; b[2]=0x90;          /* jump */
        /* OEM "NTFS    " */
        { const char *o="NTFS    "; for(i=0;i<8;i++) b[3+i]=o[i]; }
        p16(b+0x0B, SECTOR);                      /* bytes/sector */
        b[0x0D]=(u8)g->spc;                       /* sectors/cluster */
        p16(b+0x0E,0);                            /* reserved sectors */
        b[0x15]=0xF8;                             /* media */
        p16(b+0x18,63); p16(b+0x1A,255);          /* sectors/track, heads */
        p32(b+0x1C,0);                            /* hidden sectors */
        p64(b+0x28, g->total_sectors-1);          /* total sectors */
        p64(b+0x30, mft_lcn);                     /* $MFT LCN */
        p64(b+0x38, mftmirr_lcn);                 /* $MFTMirr LCN */
        b[0x40]=(u8)((cl < MREC) ? MREC/cl : (cl == MREC ? 1 : 0xF6)); /* per MFT record (-10 => 1024) */
        b[0x44]=(u8)((cl <= IBS) ? IBS/cl : 0xF4);   /* per index record (-12 => 4096) */
        p64(b+0x48, g->serial);                   /* volume serial */
        b[0x1FE]=0x55; b[0x1FF]=0xAA;             /* signature */
        vwrite(v, boot_lcn*cl, b, SECTOR);
        /* hand the boot sector back for the backup copy at the volume's end */
        for(i=0;i<SECTOR;i++) g->bootsec[i]=b[i];
        /* the rest of $Boot (bootstrap) stays zero */
    }

    /* helper macro to place a finished record into the MFT image */
    #define PUT_REC(rn) do { rec_end(&r); rec_fixup(&r); \
        vwrite(v, mft_lcn*cl + (u64)(rn)*MREC, r.rec, MREC); } while(0)

    /* --- record 0: $MFT --- */
    rec_init(&r, R_MFT, F_INUSE, 1);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$MFT", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    attr_nonres(&r,A_DATA,0,mft_clusters-1,(u64)mft_clusters*cl,(u64)mft_clusters*cl,
                mft_lcn,mft_clusters,0,0,0);
    /* $BITMAP of the MFT (non-resident, 1 cluster): mark records 0..15 used.
     * ntfs-3g maps this runlist when allocating records, so it must be
     * non-resident. */
    attr_nonres(&r,A_BITMAP,0,0,cl,8,mftbmp_lcn,1,0,0,0);
    PUT_REC(R_MFT);
    /* fill the MFT bitmap cluster: bits 0..15 set (records 0-15 in use) */
    vzero(v, mftbmp_lcn*cl, cl);
    v->base[mftbmp_lcn*cl+0]=0xFF; v->base[mftbmp_lcn*cl+1]=0xFF;
    v->base[mftbmp_lcn*cl+3]=0x07;                 /* records 24-26: $Extend files */

    /* --- record 1: $MFTMirr --- */
    rec_init(&r, R_MFTMIRR, F_INUSE, 1);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$MFTMirr", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    { u64 mc = (MIRR_BYTES(cl) + cl - 1)/cl;
      attr_nonres(&r,A_DATA,0,mc-1,mc*cl,MIRR_BYTES(cl),mftmirr_lcn,mc,0,0,0); }
    PUT_REC(R_MFTMIRR);

    /* --- record 2: $LogFile --- */
    rec_init(&r, R_LOGFILE, F_INUSE, R_LOGFILE);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$LogFile", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    attr_nonres(&r,A_DATA,0,log_clusters-1,(u64)log_clusters*cl,(u64)log_clusters*cl,
                logfile_lcn,log_clusters,0,0,0);
    PUT_REC(R_LOGFILE);
    /* fill $LogFile with 0xFF (uninitialized log) */
    { u64 o; for(o=0;o<(u64)log_clusters*cl;o++) v->base[logfile_lcn*cl+o]=0xFF; }

    /* --- record 3: $Volume --- */
    rec_init(&r, R_VOLUME, F_INUSE, R_VOLUME);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$Volume", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    /* $VOLUME_NAME (label, UTF-16) */
    { u8 nm[64]; u32 ln=0; const char *L=label?label:"";
      while(L[ln] && ln<31){ p16(nm+ln*2,(u16)L[ln]); ln++; }
      attr_res(&r,A_VNAME, nm, ln*2, 0,0); }
    /* $VOLUME_INFORMATION: major 3, minor 1, flags 0 */
    { u8 vi[12]; for(i=0;i<12;i++) vi[i]=0; vi[8]=3; vi[9]=1; attr_res(&r,A_VINFO,vi,12,0,0); }
    attr_res(&r,A_DATA,0,0,0,0);              /* empty $DATA */
    PUT_REC(R_VOLUME);

    /* --- record 4: $AttrDef --- */
    rec_init(&r, R_ATTRDEF, F_INUSE, R_ATTRDEF);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$AttrDef", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    attr_nonres(&r,A_DATA,0,attrdef_clusters-1,(u64)attrdef_clusters*cl,sizeof(NtfsAttrDef),
                attrdef_lcn,attrdef_clusters,0,0,0);
    PUT_REC(R_ATTRDEF);
    vwrite(v, attrdef_lcn*cl, NtfsAttrDef, sizeof(NtfsAttrDef));

    /* --- record 5: . (root directory) --- */
    rec_init(&r, R_ROOT, F_INUSE|F_DIR, 5);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, ".", 0x10000006u); /* hidden|system, index present */ attr_res(&r,A_FN,fn,fnlen,0,0);
    /* $INDEX_ROOT $I30: LARGE index - one END entry with a subnode pointer to
     * VCN 0 of the INDX block that holds the real (system-file) entries. */
    {
        u16 i30[4]={'$','I','3','0'};
        u8 ir[0x20 + 0x18]; for(i=0;i<(int)sizeof(ir);i++) ir[i]=0;
        p32(ir+0x00, A_FN);           /* indexed attribute type */
        p32(ir+0x04, 1);              /* collation: filename */
        p32(ir+0x08, IBS);            /* index block size */
        ir[0x0C]=IB_CPB((u32)cl);     /* clusters (or 512-byte units) per block */
        /* index node header at 0x10 (offsets relative to 0x10) */
        p32(ir+0x10, 0x10);           /* entries offset */
        p32(ir+0x14, 0x10+0x18);      /* index_length */
        p32(ir+0x18, 0x10+0x18);      /* allocated size */
        ir[0x1C]=1;                   /* flags: LARGE index (has INDX) */
        /* END entry at 0x20 with subnode flag + 8-byte VCN at end */
        p16(ir+0x20+0x08, 0x18);      /* entry length */
        p16(ir+0x20+0x0A, 0);         /* key length */
        p32(ir+0x20+0x0C, 0x03);      /* flags: last entry + has subnode */
        p64(ir+0x20+0x10, 0);         /* subnode VCN = 0 */
        attr_res(&r, A_IROOT, ir, sizeof(ir), i30, 4);
    }
    /* $INDEX_ALLOCATION $I30: non-resident, one cluster at root_indx_lcn */
    { u16 i30[4]={'$','I','3','0'};
      u64 rc = (IBS + cl - 1)/cl;
      attr_nonres(&r, A_IALLOC, 0, rc-1, rc*cl, IBS, root_indx_lcn, rc, i30, 4, 0); }
    /* $BITMAP $I30: resident, one bit (VCN 0 in use) */
    { u16 i30[4]={'$','I','3','0'}; u8 bm[8]; for(i=0;i<8;i++) bm[i]=0; bm[0]=1;
      attr_res(&r, A_BITMAP, bm, 8, i30, 4); }
    PUT_REC(R_ROOT);
    /* write the INDX block holding the system-file entries */
    { u8 *ib = (u8*)0; (void)ib;
      build_root_indx(v->base + root_indx_lcn*cl, IBS); }

    /* --- record 6: $Bitmap (cluster allocation bitmap) --- */
    rec_init(&r, R_BITMAP, F_INUSE, R_BITMAP);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$Bitmap", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    { u64 bitmap_bytes = (g->total_clusters + 7)/8;
      attr_nonres(&r,A_DATA,0,bitmap_clusters-1,(u64)bitmap_clusters*cl,bitmap_bytes,
                  bitmap_lcn,bitmap_clusters,0,0,0); }
    PUT_REC(R_BITMAP);

    /* --- record 7: $Boot --- */
    rec_init(&r, R_BOOT, F_INUSE, R_BOOT);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$Boot", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    { u64 bc = (0x2000 + cl - 1)/cl;
      attr_nonres(&r,A_DATA,0,bc-1,bc*cl,0x2000,boot_lcn,bc,0,0,0); }
    PUT_REC(R_BOOT);

    /* --- record 8: $BadClus (empty, sparse $DATA named $Bad) --- */
    rec_init(&r, R_BADCLUS, F_INUSE, R_BADCLUS);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$BadClus", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    attr_res(&r,A_DATA,0,0,0,0);                   /* unnamed empty */
    {   /* $DATA "$Bad": one sparse run spanning the whole volume, no bad clusters */
        static const u16 bad[4]={'$','B','a','d'};
        u8 *a = r.rec + r.used; u64 n = g->total_clusters, t; u32 lb=0, o;
        for(i=0;i<0x60;i++) a[i]=0;
        p32(a+0x00, A_DATA); a[0x08]=1; a[0x09]=4; p16(a+0x0A,0x40); p16(a+0x0E, r.nextid++);
        p64(a+0x10, 0); p64(a+0x18, n-1); p16(a+0x20, 0x48);
        p64(a+0x28, n*cl); p64(a+0x30, n*cl); p64(a+0x38, 0);
        for(i=0;i<4;i++) p16(a+0x40+i*2, bad[i]);
        o = 0x49; t = n; do { a[o++]=(u8)t; t>>=8; lb++; } while(t);
        if (a[o-1] & 0x80) { a[o++]=0; lb++; }        /* signed length */
        a[0x48] = (u8)lb;                              /* length only: sparse */
        a[o++] = 0;
        p32(a+0x04, (o + 7) & ~7u);
        r.used += (o + 7) & ~7u;
    }
    PUT_REC(R_BADCLUS);

    /* --- record 9: $Secure: $SDS stream + $SDH/$SII view indexes holding the
     *     single descriptor every system file references (id 0x100) --- */
    rec_init(&r, R_SECURE, F_INUSE|0x0008, R_SECURE);      /* 0x08: view index */
    attr_si(&r, 0x20000006u, SECID_SYS);                    /* + view index present */
    fnlen=build_fn(fn, R_ROOT, 5, "$Secure", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    {
        static const u16 sds[4]={'$','S','D','S'}, sdh[4]={'$','S','D','H'}, sii[4]={'$','S','I','I'};
        u8 hdr[20], e[SDS_ENTRY], ir[0x10+0x10+0x30+0x10];
        u32 h = sd_hash(SdDefault, sizeof(SdDefault));
        /* $SDS entry (and its mirror) */
        p32(hdr+0, h); p32(hdr+4, SECID_SYS); p64(hdr+8, 0); p32(hdr+16, 20+sizeof(SdDefault));
        for(i=0;i<SDS_ENTRY;i++) e[i]=0;
        for(i=0;i<20;i++) e[i]=hdr[i];
        for(i=0;i<(int)sizeof(SdDefault);i++) e[20+i]=SdDefault[i];
        vzero(v, g->sds_lcn*cl, (u64)g->sds_clusters*cl);
        vwrite(v, g->sds_lcn*cl, e, SDS_ENTRY);
        vwrite(v, g->sds_lcn*cl + 0x40000, e, SDS_ENTRY);
        attr_nonres(&r, A_DATA, 0, g->sds_clusters-1, (u64)g->sds_clusters*cl, SDS_SIZE,
                    g->sds_lcn, g->sds_clusters, sds, 4, 0);
        /* $SDH: key {hash, id}, data = the $SDS header, "II" padding */
        for(i=0;i<(int)sizeof(ir);i++) ir[i]=0;
        p32(ir+0x00, 0); p32(ir+0x04, 0x12); p32(ir+0x08, IBS); ir[0x0C]=IB_CPB((u32)cl);
        p32(ir+0x10, 0x10); p32(ir+0x14, 0x10+0x30+0x10); p32(ir+0x18, 0x10+0x30+0x10);
        { u8 *x = ir+0x20;
          p16(x+0, 0x18); p16(x+2, 0x14); p16(x+8, 0x30); p16(x+0x0A, 8); p16(x+0x0C, 0);
          p32(x+0x10, h); p32(x+0x14, SECID_SYS);
          for(i=0;i<20;i++) x[0x18+i]=hdr[i];
          x[0x2C]='I'; x[0x2E]='I';
          x += 0x30; p16(x+8, 0x10); p16(x+0x0C, 0x02); }
        attr_res(&r, A_IROOT, ir, 0x10+0x10+0x30+0x10, sdh, 4);
        /* $SII: key {id}, data = the $SDS header */
        for(i=0;i<(int)sizeof(ir);i++) ir[i]=0;
        p32(ir+0x00, 0); p32(ir+0x04, 0x10); p32(ir+0x08, IBS); ir[0x0C]=IB_CPB((u32)cl);
        p32(ir+0x10, 0x10); p32(ir+0x14, 0x10+0x28+0x10); p32(ir+0x18, 0x10+0x28+0x10);
        { u8 *x = ir+0x20;
          p16(x+0, 0x14); p16(x+2, 0x14); p16(x+8, 0x28); p16(x+0x0A, 4); p16(x+0x0C, 0);
          p32(x+0x10, SECID_SYS);
          for(i=0;i<20;i++) x[0x14+i]=hdr[i];
          x += 0x28; p16(x+8, 0x10); p16(x+0x0C, 0x02); }
        attr_res(&r, A_IROOT, ir, 0x10+0x10+0x28+0x10, sii, 4);
    }
    PUT_REC(R_SECURE);

    /* --- record 10: $UpCase --- */
    rec_init(&r, R_UPCASE, F_INUSE, R_UPCASE);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$UpCase", 0x06); attr_res(&r,A_FN,fn,fnlen,0,0);
    attr_nonres(&r,A_DATA,0,upcase_clusters-1,(u64)upcase_clusters*cl,131072,
                upcase_lcn,upcase_clusters,0,0,0);
    PUT_REC(R_UPCASE);
    /* generate $UpCase: identity then overrides */
    { u32 c; for(c=0;c<65536;c++) p16(v->base+upcase_lcn*cl+c*2,(u16)c);
      for(c=0;c<NTFS_UPCASE_OVERRIDES;c++)
          p16(v->base+upcase_lcn*cl + NtfsUpcaseOverride[c][0]*2, NtfsUpcaseOverride[c][1]); }

    /* --- record 11: $Extend (empty directory) --- */
    rec_init(&r, R_EXTEND, F_INUSE|F_DIR, R_EXTEND);
    attr_si(&r, 0x06, SECID_SYS);
    fnlen=build_fn(fn, R_ROOT, 5, "$Extend", 0x10000006u); attr_res(&r,A_FN,fn,fnlen,0,0);
    {
        /* SMALL index holding $ObjId, $Quota, $Reparse (collation order) */
        static const struct { u32 rn; const char *nm; } ext[3] =
            { {R_OBJID,"$ObjId"}, {R_QUOTA,"$Quota"}, {R_REPARSE,"$Reparse"} };
        u16 i30[4]={'$','I','3','0'};
        u8 ir[0x400]; u32 off = 0x20, k, j;
        for(i=0;i<(int)sizeof(ir);i++) ir[i]=0;
        p32(ir+0x00, A_FN); p32(ir+0x04, 1); p32(ir+0x08, IBS); ir[0x0C]=IB_CPB((u32)cl);
        for (k=0;k<3;k++) {
            u8 key[256]; u32 klen = build_fn(key, R_EXTEND, R_EXTEND, ext[k].nm, EXT_FATTR);
            u32 elen = (0x10 + klen + 7) & ~7u;
            p64(ir+off, (u64)ext[k].rn | ((u64)1<<48));
            p16(ir+off+8, (u16)elen); p16(ir+off+0x0A, (u16)klen);
            for(j=0;j<klen;j++) ir[off+0x10+j]=key[j];
            off += elen;
        }
        p16(ir+off+0x08, 0x10); p32(ir+off+0x0C, 0x02); off += 0x10;   /* END */
        p32(ir+0x10, 0x10); p32(ir+0x14, off-0x10); p32(ir+0x18, off-0x10); ir[0x1C]=0;
        attr_res(&r, A_IROOT, ir, off, i30, 4);
    }
    PUT_REC(R_EXTEND);

    /* --- records 24..26: $Extend\$Quota, $ObjId, $Reparse (NTFS 3.x view
     *     indexes, laid out like mkntfs) --- */
    {
        static const u16 nO[2]={'$','O'}, nQ[2]={'$','Q'}, nR[2]={'$','R'};
        static const u8 admins[16] = { 1,2, 0,0,0,0,0,5, 32,0,0,0, 0x20,0x02,0,0 };
        u8 ir[0x200]; u32 off, j;
        #define VIEW_ROOT(coll) do { for(i=0;i<(int)sizeof(ir);i++) ir[i]=0; \
            p32(ir+0x00,0); p32(ir+0x04,(coll)); p32(ir+0x08, IBS); ir[0x0C]=IB_CPB((u32)cl); off=0x20; } while(0)
        #define VIEW_END() do { p16(ir+off+0x08,0x10); p32(ir+off+0x0C,0x02); off+=0x10; \
            p32(ir+0x10,0x10); p32(ir+0x14,off-0x10); p32(ir+0x18,off-0x10); } while(0)

        /* $Quota: $O (owner SID -> owner id) and $Q (owner id -> quota entry) */
        rec_init(&r, R_QUOTA, F_INUSE|0x0004|0x0008, 1);
        attr_si(&r, EXT_FATTR, SECID_SYS);
        fnlen=build_fn(fn, R_EXTEND, R_EXTEND, "$Quota", EXT_FATTR); attr_res(&r,A_FN,fn,fnlen,0,0);
        VIEW_ROOT(0x11);                                   /* COLLATION_SID */
        p16(ir+off+0, 0x10+16); p16(ir+off+2, 4); p16(ir+off+8, 0x28); p16(ir+off+0x0A, 16);
        for(j=0;j<16;j++) ir[off+0x10+j]=admins[j];
        p32(ir+off+0x20, 0x100);                            /* owner id */
        off += 0x28;
        VIEW_END();
        attr_res(&r, A_IROOT, ir, off, nO, 2);
        VIEW_ROOT(0x10);                                   /* COLLATION_NTOFS_ULONG */
        /* owner id 1: the default limits */
        p16(ir+off+0, 0x14); p16(ir+off+2, 0x30); p16(ir+off+8, 0x48); p16(ir+off+0x0A, 4);
        p32(ir+off+0x10, 1);
        p32(ir+off+0x14, 2); p32(ir+off+0x18, 1);           /* version 2, DEFAULT_LIMITS */
        p64(ir+off+0x24, g_now);
        p64(ir+off+0x2C, ~0ULL); p64(ir+off+0x34, ~0ULL);   /* threshold, limit: none */
        off += 0x48;
        /* owner id 0x100: Administrators */
        p16(ir+off+0, 0x14); p16(ir+off+2, 0x40); p16(ir+off+8, 0x58); p16(ir+off+0x0A, 4);
        p32(ir+off+0x10, 0x100);
        p32(ir+off+0x14, 2); p32(ir+off+0x18, 1);
        p64(ir+off+0x24, g_now);
        p64(ir+off+0x2C, ~0ULL); p64(ir+off+0x34, ~0ULL);
        for(j=0;j<16;j++) ir[off+0x44+j]=admins[j];
        off += 0x58;
        VIEW_END();
        attr_res(&r, A_IROOT, ir, off, nQ, 2);
        PUT_REC(R_QUOTA);

        /* $ObjId: empty $O (object id -> file) */
        rec_init(&r, R_OBJID, F_INUSE|0x0004|0x0008, 1);
        attr_si(&r, EXT_FATTR, SECID_SYS);
        fnlen=build_fn(fn, R_EXTEND, R_EXTEND, "$ObjId", EXT_FATTR); attr_res(&r,A_FN,fn,fnlen,0,0);
        VIEW_ROOT(0x13); VIEW_END();                       /* COLLATION_NTOFS_ULONGS */
        attr_res(&r, A_IROOT, ir, off, nO, 2);
        PUT_REC(R_OBJID);

        /* $Reparse: empty $R (reparse tag + file -> nothing) */
        rec_init(&r, R_REPARSE, F_INUSE|0x0004|0x0008, 1);
        attr_si(&r, EXT_FATTR, SECID_SYS);
        fnlen=build_fn(fn, R_EXTEND, R_EXTEND, "$Reparse", EXT_FATTR); attr_res(&r,A_FN,fn,fnlen,0,0);
        VIEW_ROOT(0x13); VIEW_END();
        attr_res(&r, A_IROOT, ir, off, nR, 2);
        PUT_REC(R_REPARSE);
        #undef VIEW_ROOT
        #undef VIEW_END
    }

    /* --- records 12..15: reserved in-use records (SI + empty $DATA, no name,
     *     not referenced by any directory index) like mkntfs --- */
    for (i=12;i<16;i++){
        rec_init(&r,(u64)i,F_INUSE,(u16)i);   /* seq = record number, like Windows */
        attr_si(&r, 0x06, SECID_SYS);
        attr_res(&r,A_DATA,0,0,0,0);
        PUT_REC(i);
    }

    /* --- records 16..MFT_RECS-1: free (not in use) empty FILE records --- */
    for (i=16;i<MFT_RECS;i++){
        if (i>=R_QUOTA && i<=R_REPARSE) continue;   /* the $Extend files */
        rec_init(&r,(u64)i,0,1);        /* flags=0: not in use */
        PUT_REC(i);
    }

    /* --- $MFTMirr: copy of records 0..3 --- */
    for (i=0;i<(int)(MIRR_BYTES(cl)/MREC);i++)
        vwrite(v, mftmirr_lcn*cl + (u64)i*MREC, v->base + mft_lcn*cl + (u64)i*MREC, MREC);

    /* --- cluster bitmap: mark every allocated cluster [0, next) used --- */
    {
        u64 c;
        vzero(v, bitmap_lcn*cl, (u64)bitmap_clusters*cl);
        for (c=0;c<next;c++)
            v->base[bitmap_lcn*cl + (c>>3)] |= (u8)(1u << (c & 7));
        /* clusters beyond total are "used" too (padding bits) */
        for (c=g->total_clusters; c < (u64)bitmap_clusters*cl*8; c++)
            v->base[bitmap_lcn*cl + (c>>3)] |= (u8)(1u << (c & 7));
    }
    (void)val;
    return 0;
}

/* "/A:4096", "/A:64K" -> cluster bytes (0 if not a valid NTFS cluster size) */
static u32 parse_cluster(const char *s)
{
    u32 n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (u32)(*s++ - '0');
    if (*s == 'k' || *s == 'K') n *= 1024;
    if (n < 512 || n > 65536 || (n & (n - 1))) return 0;
    return n;
}
static int is_cluster_opt(const char *a)
{ return (a[0] == '/' || a[0] == '-') && (a[1] | 32) == 'a' && a[2] == ':'; }

/* =====================================================================
 * Platform wrappers
 * ===================================================================== */
#ifndef _WIN32
int main(int argc, char **argv)
{
    const char *path; u64 mb=64; const char *label="NTFS3NT"; u32 clus=4096; int i, pos=0;
    GEO g; VOL v; u64 bytes, meta_bytes; int fd;
    if (argc<2){ fprintf(stderr,"usage: %s <image> [sizeMB] [label] [/A:cluster]\n",argv[0]); return 2; }
    path=argv[1];
    for (i=2;i<argc;i++) {
        if (is_cluster_opt(argv[i])) {
            if (!(clus = parse_cluster(argv[i]+3))) { fprintf(stderr,"bad cluster size (512..64K)\n"); return 2; }
        } else if (pos++ == 0) mb=(u64)strtoull(argv[i],0,0);
        else label=argv[i];
    }
    bytes = mb*1024*1024;
    g_now = (u64)time(0) * 10000000ULL + 116444736000000000ULL;
    fmt_compute(&g, bytes/SECTOR, clus);
    meta_bytes = g.meta_clusters * g.cluster;
    v.base=(u8*)calloc(1,(size_t)meta_bytes); v.bytes=meta_bytes;
    if(!v.base){ fprintf(stderr,"oom\n"); return 1; }
    if (fmt_build(&v,&g,label)){ fprintf(stderr,"build failed\n"); return 1; }
    fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd<0){ perror("open"); return 1; }
    /* write the metadata region at the start */
    if(pwrite(fd,v.base,(size_t)meta_bytes,0)!=(ssize_t)meta_bytes){ perror("write"); return 1; }
    /* backup boot sector at the last sector; this also sizes the file */
    if(pwrite(fd,g.bootsec,SECTOR,(off_t)((bytes/SECTOR-1)*SECTOR))!=SECTOR){ perror("write bak"); return 1; }
    close(fd); free(v.base);
    printf("ntfsfmt: wrote NTFS 3.1 volume (%llu MB, cluster %u, %llu metadata clusters) to %s\n",
           (unsigned long long)mb, g.cluster, (unsigned long long)g.meta_clusters, path);
    return 0;
}
#else
/* NT console tool: format a volume/image at <path>. Buffers only the metadata
 * region (not the whole volume), so it works on large partitions. CRT-free,
 * NT 3.1-safe APIs only (no GetFileSizeEx). */
#include <windows.h>
int main(int argc, char **argv)
{
    HANDLE h; GEO g; VOL v; DWORD wr, hi=0, lo; u64 bytes, meta_bytes, off; int i, isdev=0;
    const char *path = argc>1?argv[1]:0;
    const char *label = "NTFS3NT"; u32 clus = 4096;
    if(!path){ printf("usage: ntfsfmt <drive|image> [/L:label] [/A:cluster]\n"); return 2; }
    for(i=2;i<argc;i++) {
        if(argv[i][0]=='/'&&(argv[i][1]|32)=='l'&&argv[i][2]==':') label=argv[i]+3;
        else if(is_cluster_opt(argv[i])) {
            if(!(clus=parse_cluster(argv[i]+3))){ printf("bad cluster size (512 .. 64K)\n"); return 2; }
        }
    }
    { /* "D:" is the volume \\.\D: (else it would open D:'s current directory) */
      static char dev[8];
      if(path[0] && path[1]==':' && !path[2]){ dev[0]=dev[1]=dev[3]='\\'; dev[2]='.'; dev[4]=path[0]; dev[5]=':'; dev[6]=0; path=dev; } }
    h=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,0,0);
    if(h==INVALID_HANDLE_VALUE){ printf("cannot open %s\n",path); return 1; }
    { FILETIME ft; SYSTEMTIME st; GetSystemTime(&st); SystemTimeToFileTime(&st,&ft); /* NT 3.1 has no GetSystemTimeAsFileTime */ g_now=((u64)ft.dwHighDateTime<<32)|ft.dwLowDateTime; }
    lo=GetFileSize(h,&hi);
    bytes=((u64)hi<<32)|lo;
    /* a volume or partition (\\.\X:, \\.\PhysicalDriveN...), whatever size a
       mounted file system reports for it */
    if((path[0]=='\\'&&path[1]=='\\'&&path[2]=='.')||bytes==0||lo==0xFFFFFFFF){
        PARTITION_INFORMATION pi; DISK_GEOMETRY dg; DWORD br;
        isdev=1; bytes=0;
        /* the partition's own length (not the whole disk's geometry) */
        if(DeviceIoControl(h,IOCTL_DISK_GET_PARTITION_INFO,0,0,&pi,sizeof(pi),&br,0))
            bytes=(u64)pi.PartitionLength.QuadPart;
        else if(DeviceIoControl(h,IOCTL_DISK_GET_DRIVE_GEOMETRY,0,0,&dg,sizeof(dg),&br,0))
            bytes=(u64)dg.Cylinders.QuadPart*dg.TracksPerCylinder*dg.SectorsPerTrack*dg.BytesPerSector;
        /* take the volume away from whatever file system has it mounted */
        if(!DeviceIoControl(h,FSCTL_LOCK_VOLUME,0,0,0,0,&br,0)){
            printf("cannot lock %s: close all files and windows on it first\n",path);
            CloseHandle(h); return 1;
        }
        /* and dismount it, so that the writes below go to the disk itself
           (a mounted file system refuses them, or caches over them) */
        DeviceIoControl(h,FSCTL_DISMOUNT_VOLUME,0,0,0,0,&br,0);
    }
    if(bytes<4*1024*1024){ printf("volume too small or size unknown\n"); CloseHandle(h); return 1; }
    fmt_compute(&g, bytes/SECTOR, clus);
    meta_bytes=g.meta_clusters*g.cluster;
    v.base=(u8*)VirtualAlloc(0,(SIZE_T)meta_bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    v.bytes=meta_bytes;
    if(!v.base){ printf("out of memory for %lu metadata bytes\n",(unsigned long)meta_bytes); CloseHandle(h); return 1; }
    fmt_build(&v,&g,label);
    /* metadata region */
    SetFilePointer(h,0,0,FILE_BEGIN);
    for(off=0;off<meta_bytes;off+=0x100000){
        DWORD n=(DWORD)((meta_bytes-off>0x100000)?0x100000:(meta_bytes-off));
        if(!WriteFile(h,v.base+off,n,&wr,0)||wr!=n){ printf("write failed\n"); CloseHandle(h); return 1; }
    }
    /* backup boot sector at the last sector */
    { LONG dhi=(LONG)(((bytes-SECTOR)>>32)&0xFFFFFFFF); DWORD dlo=(DWORD)((bytes-SECTOR)&0xFFFFFFFF);
      SetFilePointer(h,(LONG)dlo,&dhi,FILE_BEGIN);
      WriteFile(h,g.bootsec,SECTOR,&wr,0); }
    if(isdev){ DWORD br; PARTITION_INFORMATION pi; SET_PARTITION_INFORMATION sp;
        /* NTFS lives in partition type 07h */
        if(DeviceIoControl(h,IOCTL_DISK_GET_PARTITION_INFO,0,0,&pi,sizeof(pi),&br,0) && pi.PartitionType!=0x07){
            sp.PartitionType=0x07;
            if(DeviceIoControl(h,IOCTL_DISK_SET_PARTITION_INFO,&sp,sizeof(sp),0,0,&br,0)) printf("partition type set to 07h\n");
        }
        /* make the system mount the new file system afresh */
        DeviceIoControl(h,FSCTL_DISMOUNT_VOLUME,0,0,0,0,&br,0);
        DeviceIoControl(h,FSCTL_UNLOCK_VOLUME,0,0,0,0,&br,0); }
    CloseHandle(h);
    printf("ntfsfmt: formatted %s as NTFS 3.1\n",path);
    return 0;
}
#endif
