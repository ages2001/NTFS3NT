/*
 * NTFS3NT - user-mode validation of the write ENGINE the kernel uses to grow a
 * file: scan $Bitmap for free clusters, mark them, extend the $DATA run list,
 * re-encode the mapping pairs into the record (NtfsRewriteDataRuns), write the
 * record back with a fresh fixup, and write the new data into the fresh
 * clusters. ntfs-3g then verifies the result. This exercises NtfsEncodeRuns,
 * NtfsRewriteDataRuns and the allocation logic that ntfsalloc.c/ntfswrite.c run.
 *
 * Build: cc -I../src ntfsgrow.c ../src/ntfssup.c -o ntfsgrow
 * Run:   ./ntfsgrow <image> <mft-record> <extra-bytes>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "ntfsusr.h"
#include "ntfssup.h"

static int fd;
static NTFS_GEOMETRY g;
static NTFS_RUN mftRuns[256];
static ULONG mftRunCount;

static void die(const char *m){ fprintf(stderr,"ERR: %s\n",m); exit(1); }
static void rd(void*b,ULONG n,ULONGLONG o){ if(pread(fd,b,n,(off_t)o)!=(ssize_t)n) die("pread"); }
static void wr(const void*b,ULONG n,ULONGLONG o){ if(pwrite(fd,b,n,(off_t)o)!=(ssize_t)n) die("pwrite"); }

static ULONGLONG map(PNTFS_RUN runs, ULONG c, ULONGLONG vbo)
{ ULONG cs=g.ClusterSize; ULONGLONG l,lf; BOOLEAN sp;
  if(!NtfsMapVcn(runs,c,vbo/cs,&l,&lf,&sp)) die("unmapped"); return l*(ULONGLONG)cs+(vbo%cs); }

/* write len bytes at vbo through a run list, splitting at cluster boundaries */
static void wr_runs(PNTFS_RUN runs, ULONG c, ULONGLONG vbo, const void*buf, ULONG len)
{ const unsigned char*p=buf; ULONG cs=g.ClusterSize;
  while(len){ ULONGLONG l,lf; BOOLEAN sp; ULONG in=vbo%cs,ch;
    if(!NtfsMapVcn(runs,c,vbo/cs,&l,&lf,&sp)) die("wr unmapped");
    ch=(ULONG)(lf*cs-in); if(ch>len)ch=len; wr(p,ch,l*cs+in); p+=ch; vbo+=ch; len-=ch; } }

static void read_mft(ULONGLONG rn, void*buf)
{ ULONGLONG off=map(mftRuns,mftRunCount,rn*g.MftRecordSize);
  rd(buf,g.MftRecordSize,off); if(!NtfsApplyFixup(buf,g.MftRecordSize,g.BytesPerSector)) die("fixup"); }
static void write_mft(ULONGLONG rn, void*buf)
{ ULONGLONG off=map(mftRuns,mftRunCount,rn*g.MftRecordSize);
  NtfsSetFixup(buf,g.MftRecordSize,g.BytesPerSector); wr(buf,g.MftRecordSize,off); }

/* allocate Count contiguous free clusters from $Bitmap (record 6); mark+persist */
static ULONGLONG alloc_clusters(ULONG count)
{
    unsigned char *r6=malloc(g.MftRecordSize); PMFT_RECORD m; PATTRIBUTE_RECORD a;
    NTFS_RUN runs[64]; ULONG rc; ULONGLONG bytes; unsigned char *bm;
    ULONGLONG i, found=(ULONGLONG)-1; ULONG run=0;
    read_mft(6,r6); m=(PMFT_RECORD)r6;
    a=NtfsFindAttribute(m,ATTR_DATA,NULL,0); if(!a||!a->NonResident) die("$Bitmap");
    bytes=a->Form.NonResident.DataSize; NtfsDecodeRuns(a,runs,64,&rc);
    bm=malloc((size_t)bytes);
    { const unsigned char*p; ULONG cs=g.ClusterSize; ULONGLONG vbo=0; ULONG left=(ULONG)bytes;
      unsigned char*o=bm; (void)p;
      while(left){ ULONGLONG l,lf; BOOLEAN sp; ULONG in=vbo%cs,ch;
        if(!NtfsMapVcn(runs,rc,vbo/cs,&l,&lf,&sp)) die("bm map");
        ch=(ULONG)(lf*cs-in); if(ch>left)ch=left; rd(o,ch,l*cs+in); o+=ch; vbo+=ch; left-=ch; } }
    for(i=0;i<g.TotalClusters;i++){
        if(!(bm[i>>3]&(1u<<(i&7)))){ if(run==0)found=i; if(++run==count)break; }
        else { run=0; found=(ULONGLONG)-1; } }
    if(run<count||found==(ULONGLONG)-1) die("disk full");
    for(i=found;i<found+count;i++) bm[i>>3]|=(1u<<(i&7));
    { ULONGLONG b0=found>>3,b1=(found+count-1)>>3; wr_runs(runs,rc,b0,bm+b0,(ULONG)(b1-b0+1)); }
    free(bm); free(r6);
    return found;
}

int main(int argc,char**argv)
{
    unsigned char boot[512],*rec,*data; PMFT_RECORD m; PATTRIBUTE_RECORD a;
    ULONGLONG rn, oldData, oldAlloc, newData, newAlloc, lcn; ULONG extra, cs, addClu, i;
    NTFS_RUN runs[66]; ULONG rc;
    if(argc<4){ fprintf(stderr,"usage: %s <image> <rec> <extra-bytes>\n",argv[0]); return 2; }
    fd=open(argv[1],O_RDWR); if(fd<0) die("open");
    rn=strtoull(argv[2],0,0); extra=(ULONG)strtoul(argv[3],0,0);

    rd(boot,512,0); if(!NtfsParseBoot(boot,512,&g)) die("boot"); cs=g.ClusterSize;
    { unsigned char r0[4096]; rd(r0,g.MftRecordSize,g.MftLcn*(ULONGLONG)cs);
      NtfsApplyFixup(r0,g.MftRecordSize,g.BytesPerSector);
      a=NtfsFindAttribute((PMFT_RECORD)r0,ATTR_DATA,NULL,0);
      if(!NtfsDecodeRuns(a,mftRuns,256,&mftRunCount)) die("mft runs"); }

    rec=malloc(g.MftRecordSize); read_mft(rn,rec); m=(PMFT_RECORD)rec;
    a=NtfsFindAttribute(m,ATTR_DATA,NULL,0); if(!a) die("no $DATA");
    if(!a->NonResident) die("resident (use a non-resident target)");
    oldData=a->Form.NonResident.DataSize; oldAlloc=a->Form.NonResident.AllocatedSize;
    if(!NtfsDecodeRuns(a,runs,66,&rc)) die("decode");
    printf("before: data=%llu alloc=%llu runs=%u\n",
        (unsigned long long)oldData,(unsigned long long)oldAlloc,rc);

    newData=oldData+extra;
    addClu=(ULONG)(((newData+cs-1)/cs) - (oldAlloc/cs));
    newAlloc=oldAlloc;
    if(addClu>0){
        lcn=alloc_clusters(addClu);
        /* contiguous with last run? extend : append */
        if(!runs[rc-1].Sparse && runs[rc-1].Lcn+runs[rc-1].Length==lcn){
            runs[rc-1].Length+=addClu;
        } else {
            runs[rc].Vcn=runs[rc-1].Vcn+runs[rc-1].Length; runs[rc].Lcn=lcn;
            runs[rc].Length=addClu; runs[rc].Sparse=FALSE; rc++;
        }
        newAlloc=oldAlloc+(ULONGLONG)addClu*cs;
        if(!NtfsRewriteDataRuns(m,g.MftRecordSize,runs,rc,newAlloc,newData)) die("rewrite runs");
    } else {
        NtfsSetDataSize(m,newData);
    }
    write_mft(rn,rec);

    /* write recognizable data into the grown region */
    data=malloc(extra?extra:1);
    for(i=0;i<extra;i++) data[i]=(unsigned char)('A'+((oldData+i)%26));
    if(extra) wr_runs(runs,rc,oldData,data,extra);
    fsync(fd); close(fd);
    printf("after:  data=%llu alloc=%llu runs=%u (+%u clusters)\n",
        (unsigned long long)newData,(unsigned long long)newAlloc,rc,addClu);
    return 0;
}
