/*
 * NTFS3NT - user-mode check of the named-$DATA (ADS) parsing the kernel create/
 * read path relies on. Opens an image, reads a file's MFT record, and locates a
 * named $DATA attribute with NtfsFindAttribute (exactly as OpenNamedStream does
 * in the driver), then reads and prints the stream's bytes.
 *
 * Build:  cc -I../src ntfsads.c ../src/ntfssup.c -o ntfsads
 * Run:    ./ntfsads <image> <inode> <streamname>
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

static void die(const char *m){ fprintf(stderr,"ERR: %s\n",m); exit(1); }
static void read_at(void *b, ULONG n, ULONGLONG o){ if(pread(g_fd,b,n,(off_t)o)!=(ssize_t)n) die("pread"); }

static int read_runs(PNTFS_RUN runs, ULONG count, ULONGLONG vbo, void *buf, ULONG len)
{
    PUCHAR out=(PUCHAR)buf; ULONG cs=g_geo.ClusterSize;
    while(len){
        ULONGLONG vcn=vbo/cs, lcn, left; ULONG in=(ULONG)(vbo%cs), chunk; BOOLEAN sp;
        if(!NtfsMapVcn(runs,count,vcn,&lcn,&left,&sp)){ if(sp){chunk=cs-in; if(chunk>len)chunk=len; memset(out,0,chunk);} else return 0; }
        else { ULONGLONG bytes=left*cs-in; chunk=(bytes>len)?len:(ULONG)bytes; read_at(out,chunk,lcn*cs+in); }
        out+=chunk; vbo+=chunk; len-=chunk;
    }
    return 1;
}

static int read_mft(ULONGLONG rn, void *buf)
{
    if(!read_runs(g_mftRuns,g_mftRunCount,rn*g_geo.MftRecordSize,buf,g_geo.MftRecordSize)) return 0;
    if(((PNTFS_RECORD_HEADER)buf)->Type!=NTFS_FILE_SIGNATURE) return 0;
    return NtfsApplyFixup(buf,g_geo.MftRecordSize,g_geo.BytesPerSector);
}

int main(int argc, char **argv)
{
    unsigned char boot[512], *rec; PMFT_RECORD m; PATTRIBUTE_RECORD a;
    ULONGLONG inode; WCHAR wname[128]; ULONG wlen=0, i;
    if(argc<4){ fprintf(stderr,"usage: %s <image> <inode> <streamname>\n",argv[0]); return 2; }
    g_fd=open(argv[1],O_RDONLY); if(g_fd<0) die("open");
    inode=(ULONGLONG)strtoull(argv[2],0,0);
    for(i=0;argv[3][i] && i<127;i++) wname[wlen++]=(WCHAR)argv[3][i];

    read_at(boot,512,0);
    if(!NtfsParseBoot(boot,512,&g_geo)) die("boot");
    /* bootstrap $MFT runs from record 0 */
    rec=(unsigned char*)malloc(g_geo.MftRecordSize);
    read_at(rec,g_geo.MftRecordSize,g_geo.MftLcn*g_geo.ClusterSize);
    if(!NtfsApplyFixup(rec,g_geo.MftRecordSize,g_geo.BytesPerSector)) die("mft fixup");
    a=NtfsFindAttribute((PMFT_RECORD)rec,ATTR_DATA,NULL,0);
    if(!a||!NtfsDecodeRuns(a,g_mftRuns,256,&g_mftRunCount)) die("mft runs");

    if(!read_mft(inode,rec)) die("read inode");
    m=(PMFT_RECORD)rec;

    printf("file inode %llu: data streams:\n",(unsigned long long)inode);
    for(a=NtfsFirstAttribute(m);a;a=NtfsNextAttribute(m,a)){
        if(a->Type==ATTR_END) break;
        if(a->Type!=ATTR_DATA) continue;
        if(a->NameLength==0) printf("  <unnamed>  %s  %llu bytes\n",
            a->NonResident?"nonres":"res",
            (unsigned long long)(a->NonResident?a->Form.NonResident.DataSize:NtfsResidentValueLength(a)));
        else { PWCHAR an=NtfsAttributeName(a); ULONG k;
            printf("  :"); for(k=0;k<a->NameLength;k++) putchar((char)an[k]);
            printf("  %s  %llu bytes\n", a->NonResident?"nonres":"res",
              (unsigned long long)(a->NonResident?a->Form.NonResident.DataSize:NtfsResidentValueLength(a))); }
    }

    /* now locate the requested named stream exactly like OpenNamedStream */
    a=NtfsFindAttribute(m,ATTR_DATA,wname,wlen);
    if(!a){ printf("stream '%s' NOT FOUND\n",argv[3]); return 1; }
    printf("stream '%s' found: ",argv[3]);
    if(!a->NonResident){
        ULONG vl=NtfsResidentValueLength(a);
        printf("resident %u bytes: \"",vl);
        fwrite(NtfsResidentValue(a),1,vl>80?80:vl,stdout); printf("\"\n");
    } else {
        NTFS_RUN runs[64]; ULONG rc; ULONG sz=(ULONG)a->Form.NonResident.DataSize;
        unsigned char *b=(unsigned char*)malloc(sz+1);
        if(NtfsDecodeRuns(a,runs,64,&rc) && read_runs(runs,rc,0,b,sz)){
            b[sz>80?80:sz]=0;
            printf("non-resident %u bytes: \"%.*s\"\n",sz,sz>80?80:sz,b);
        } else printf("non-resident %u bytes (read failed)\n",sz);
    }
    return 0;
}
