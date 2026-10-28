/*
 * NTFS3NT - host validator for the COMPRESS direction. Takes a plain file
 * already in an NTFS image and rewrites its $DATA as LZNT1-compressed using the
 * EXACT layout NtfsCompressFile produces (NtfsLznt1CompressUnit +
 * NtfsSetBaseDataRuns with CompressionUnit=4, real+sparse runs per unit). It
 * reuses the file's own already-allocated clusters (no $Bitmap juggling - any
 * freed tail just leaks, which is fine for a test image). Then ntfs-3g reading
 * the file back byte-for-byte proves the on-disk compressed attribute and run
 * layout are what a real NTFS reader expects.
 *
 * Build: cc -I../src ntfsmkcomp.c ../src/ntfssup.c -o ntfsmkcomp
 * Run:   ./ntfsmkcomp <image> <inode>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "ntfsusr.h"
#include "ntfssup.h"

static int fd;
static NTFS_GEOMETRY g;
static void ra(void*b,ULONG l,ULONGLONG o){ if(pread(fd,b,l,o)!=(ssize_t)l){perror("pread");exit(1);} }
static void wa(const void*b,ULONG l,ULONGLONG o){ if(pwrite(fd,b,l,o)!=(ssize_t)l){perror("pwrite");exit(1);} }

#define CU 16u

int main(int c,char**v){
    unsigned char boot[512];
    if(c<3){fprintf(stderr,"usage: %s <image> <inode>\n",v[0]);return 2;}
    fd=open(v[1],O_RDWR); if(fd<0){perror("open");return 1;}
    ra(boot,512,0); NtfsParseBoot(boot,512,&g);
    ULONG cs=g.ClusterSize, ub=CU*cs, rs=g.MftRecordSize;
    ULONGLONG recoff=g.MftLcn*(ULONGLONG)g.ClusterSize + (ULONGLONG)atoll(v[2])*rs;
    unsigned char *rec=malloc(rs); ra(rec,rs,recoff);
    NtfsApplyFixup(rec,rs,g.BytesPerSector);
    PMFT_RECORD m=(PMFT_RECORD)rec;
    PATTRIBUTE_RECORD a=NtfsFindAttribute(m,ATTR_DATA,NULL,0);
    if(!a||!a->NonResident){fprintf(stderr,"not a non-resident file\n");return 1;}
    ULONGLONG dsize=a->Form.NonResident.DataSize;
    NTFS_RUN runs[512]; ULONG rc=0; NtfsDecodeRuns(a,runs,512,&rc);
    /* gather the file's existing LCNs in VCN order */
    ULONGLONG lcns[4096]; ULONG nlcn=0, i,j;
    for(i=0;i<rc;i++) for(j=0;j<runs[i].Length && nlcn<4096;j++) lcns[nlcn++]=runs[i].Lcn+j;
    /* read the plain data */
    ULONGLONG numUnits=(dsize+ub-1)/ub; if(!numUnits)numUnits=1;
    unsigned char *buf=calloc(numUnits,ub);
    for(i=0;i<(dsize+cs-1)/cs;i++) ra(buf+i*cs,cs, lcns[i]*(ULONGLONG)cs);
    /* save original for later compare */
    FILE*of=fopen("/tmp/mkcomp_orig.bin","wb"); fwrite(buf,1,dsize,of); fclose(of);

    /* compress unit by unit into the file's own clusters; build new runs */
    NTFS_RUN nr[1024]; ULONG nrc=0; ULONGLONG totalReal=0, vcn=0;
    unsigned char *comp=malloc(ub+4096), *body=malloc(4096);
    ULONG lcnIdx=0, u;
    for(u=0;u<numUnits;u++){
        ULONG op=NtfsLznt1CompressUnit(buf+u*ub, ub, comp, ub, body);
        ULONG cc=(op==0xFFFFFFFFu)?CU:((op+cs-1)/cs);
        if(cc<CU){
            memset(comp+op,0,cc*cs-op);
            for(j=0;j<cc;j++) wa(comp+j*cs,cs,lcns[lcnIdx+j]*(ULONGLONG)cs);
            /* real run(s): emit one per contiguous span */
            ULONG s=0;
            while(s<cc){ ULONG e=s+1; while(e<cc && lcns[lcnIdx+e]==lcns[lcnIdx+e-1]+1) e++;
                nr[nrc].Vcn=vcn+s; nr[nrc].Lcn=lcns[lcnIdx+s]; nr[nrc].Length=e-s; nr[nrc].Sparse=FALSE; nrc++; s=e; }
            nr[nrc].Vcn=vcn+cc; nr[nrc].Lcn=0; nr[nrc].Length=CU-cc; nr[nrc].Sparse=TRUE; nrc++;
            lcnIdx+=cc; totalReal+=cc;
        } else {
            for(j=0;j<CU;j++) wa(buf+u*ub+j*cs,cs,lcns[lcnIdx+j]*(ULONGLONG)cs);
            ULONG s=0;
            while(s<CU){ ULONG e=s+1; while(e<CU && lcns[lcnIdx+e]==lcns[lcnIdx+e-1]+1) e++;
                nr[nrc].Vcn=vcn+s; nr[nrc].Lcn=lcns[lcnIdx+s]; nr[nrc].Length=e-s; nr[nrc].Sparse=FALSE; nrc++; s=e; }
            lcnIdx+=CU; totalReal+=CU;
        }
        vcn+=CU;
    }
    if(totalReal>=numUnits*CU){ printf("incompressible; left plain\n"); return 0; }
    if(!NtfsSetBaseDataRuns(m,rs,nr,nrc,numUnits*CU*(ULONGLONG)cs,dsize,4,totalReal*cs)){ fprintf(stderr,"mapping too big\n"); return 1; }
    /* mark FILE_ATTRIBUTE_COMPRESSED in $STD_INFO */
    { PATTRIBUTE_RECORD si=NtfsFindAttribute(m,ATTR_STANDARD_INFORMATION,NULL,0);
      if(si&&!si->NonResident){ PSTANDARD_INFORMATION s=(PSTANDARD_INFORMATION)NtfsResidentValue(si); s->FileAttributes|=0x800; } }
    NtfsSetFixup(rec,rs,g.BytesPerSector);
    wa(rec,rs,recoff);
    fsync(fd);
    printf("wrote compressed: dsize=%llu units=%llu realClu=%llu/%llu nrc=%u\n",
           (unsigned long long)dsize,(unsigned long long)numUnits,(unsigned long long)totalReal,(unsigned long long)(numUnits*CU),nrc);
    return 0;
}
