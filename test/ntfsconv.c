/*
 * NTFS3NT - user-mode validation of resident -> non-resident $DATA conversion
 * (NtfsConvertDataToNonResident), the engine the kernel runs when a write grows
 * a small resident file past the MFT record. Takes a resident file, converts it,
 * writes the old bytes + appended bytes into fresh clusters, and ntfs-3g then
 * reads the whole file back and reports the volume clean.
 *
 * Build: cc -I../src ntfsconv.c ../src/ntfssup.c -o ntfsconv
 * Run:   ./ntfsconv <image> <mft-record> <append-bytes>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "ntfsusr.h"
#include "ntfssup.h"

static int fd; static NTFS_GEOMETRY g; static NTFS_RUN mftRuns[256]; static ULONG mftRunCount;
static void die(const char*m){ fprintf(stderr,"ERR: %s\n",m); exit(1); }
static void rd(void*b,ULONG n,ULONGLONG o){ if(pread(fd,b,n,(off_t)o)!=(ssize_t)n) die("pread"); }
static void wr(const void*b,ULONG n,ULONGLONG o){ if(pwrite(fd,b,n,(off_t)o)!=(ssize_t)n) die("pwrite"); }
static ULONGLONG rmap(PNTFS_RUN r,ULONG c,ULONGLONG vbo){ ULONG cs=g.ClusterSize; ULONGLONG l,lf; BOOLEAN sp;
  if(!NtfsMapVcn(r,c,vbo/cs,&l,&lf,&sp)) die("unmapped"); return l*(ULONGLONG)cs+(vbo%cs); }
static void wr_runs(PNTFS_RUN r,ULONG c,ULONGLONG vbo,const void*buf,ULONG len){ const unsigned char*p=buf; ULONG cs=g.ClusterSize;
  while(len){ ULONGLONG l,lf; BOOLEAN sp; ULONG in=vbo%cs,ch; if(!NtfsMapVcn(r,c,vbo/cs,&l,&lf,&sp)) die("wr unmapped");
    ch=(ULONG)(lf*cs-in); if(ch>len)ch=len; wr(p,ch,l*cs+in); p+=ch; vbo+=ch; len-=ch; } }
static void rd_runs(PNTFS_RUN r,ULONG c,ULONGLONG vbo,void*buf,ULONG len){ unsigned char*p=buf; ULONG cs=g.ClusterSize;
  while(len){ ULONGLONG l,lf; BOOLEAN sp; ULONG in=vbo%cs,ch; if(!NtfsMapVcn(r,c,vbo/cs,&l,&lf,&sp)) die("rd unmapped");
    ch=(ULONG)(lf*cs-in); if(ch>len)ch=len; rd(p,ch,l*cs+in); p+=ch; vbo+=ch; len-=ch; } }
static void read_mft(ULONGLONG rn,void*b){ rd(b,g.MftRecordSize,rmap(mftRuns,mftRunCount,rn*g.MftRecordSize));
  if(!NtfsApplyFixup(b,g.MftRecordSize,g.BytesPerSector)) die("fixup"); }
static void write_mft(ULONGLONG rn,void*b){ NtfsSetFixup(b,g.MftRecordSize,g.BytesPerSector);
  wr(b,g.MftRecordSize,rmap(mftRuns,mftRunCount,rn*g.MftRecordSize)); }

static ULONGLONG alloc_clusters(ULONG n){
    unsigned char *r=malloc(g.MftRecordSize); PMFT_RECORD m; PATTRIBUTE_RECORD a; NTFS_RUN runs[64]; ULONG rc;
    ULONGLONG bytes,i,found=(ULONGLONG)-1; ULONG run=0; unsigned char*bm;
    read_mft(MFT_RN_BITMAP,r); m=(PMFT_RECORD)r; a=NtfsFindAttribute(m,ATTR_DATA,NULL,0);
    bytes=a->Form.NonResident.DataSize; NtfsDecodeRuns(a,runs,64,&rc); bm=malloc((size_t)bytes); rd_runs(runs,rc,0,bm,(ULONG)bytes);
    for(i=0;i<g.TotalClusters;i++){ if(!(bm[i>>3]&(1u<<(i&7)))){ if(run==0)found=i; if(++run==n)break; } else {run=0;found=(ULONGLONG)-1;} }
    if(run<n) die("disk full");
    for(i=found;i<found+n;i++) bm[i>>3]|=(1u<<(i&7));
    { ULONGLONG b0=found>>3,b1=(found+n-1)>>3; wr_runs(runs,rc,b0,bm+b0,(ULONG)(b1-b0+1)); }
    free(bm); free(r); return found;
}

int main(int argc,char**argv){
    unsigned char boot[512],r0[4096],*rec,*data; PMFT_RECORD m; PATTRIBUTE_RECORD a;
    ULONGLONG rn,lcn,oldEof,newEof; ULONG add,cs,clusters,i; NTFS_RUN run;
    if(argc<4){ fprintf(stderr,"usage: %s <img> <rec> <append>\n",argv[0]); return 2; }
    fd=open(argv[1],O_RDWR); if(fd<0) die("open"); rn=strtoull(argv[2],0,0); add=(ULONG)strtoul(argv[3],0,0);
    rd(boot,512,0); if(!NtfsParseBoot(boot,512,&g)) die("boot"); cs=g.ClusterSize;
    rd(r0,g.MftRecordSize,g.MftLcn*(ULONGLONG)cs); NtfsApplyFixup(r0,g.MftRecordSize,g.BytesPerSector);
    a=NtfsFindAttribute((PMFT_RECORD)r0,ATTR_DATA,NULL,0); if(!NtfsDecodeRuns(a,mftRuns,256,&mftRunCount)) die("mft runs");

    rec=malloc(g.MftRecordSize); read_mft(rn,rec); m=(PMFT_RECORD)rec;
    a=NtfsFindAttribute(m,ATTR_DATA,NULL,0); if(!a) die("no $DATA"); if(a->NonResident) die("already non-resident");
    oldEof=NtfsResidentValueLength(a); newEof=oldEof+add;
    printf("before: resident %llu bytes\n",(unsigned long long)oldEof);

    /* snapshot old resident bytes */
    data=malloc((size_t)newEof); memcpy(data,NtfsResidentValue(a),(size_t)oldEof);
    for(i=0;i<add;i++) data[oldEof+i]=(unsigned char)('a'+((oldEof+i)%26));

    clusters=(ULONG)((newEof+cs-1)/cs); lcn=alloc_clusters(clusters);
    if(!NtfsConvertDataToNonResident(m,g.MftRecordSize,lcn,clusters,(ULONGLONG)clusters*cs,newEof)) die("convert");
    write_mft(rn,rec);
    run.Vcn=0; run.Lcn=lcn; run.Length=clusters; run.Sparse=FALSE;
    wr_runs(&run,1,0,data,(ULONG)newEof);
    fsync(fd); close(fd);
    printf("after:  non-resident %llu bytes in %u cluster(s)\n",(unsigned long long)newEof,clusters);
    return 0;
}
