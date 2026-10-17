/*
 * NTFS3NT - validate SMALL (resident) $INDEX_ROOT insert/remove
 * (NtfsIndexRootInsert / NtfsIndexRootRemove) by creating and deleting a file in
 * $Extend (record 11), a small resident-root directory on an ntfsfmt volume, then
 * checking the volume stays clean with ntfs-3g.
 *
 * Build: cc -I../src ntfssmall.c ../src/ntfssup.c -o ntfssmall
 * Run:   ./ntfssmall <image>
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
static ULONGLONG alloc_mft(void){
    unsigned char *r=malloc(g.MftRecordSize); PMFT_RECORD m; PATTRIBUTE_RECORD a; NTFS_RUN runs[64]; ULONG rc;
    ULONGLONG bytes,i,found=(ULONGLONG)-1; unsigned char*bm;
    read_mft(MFT_RN_MFT,r); m=(PMFT_RECORD)r; a=NtfsFindAttribute(m,ATTR_BITMAP,NULL,0);
    bytes=a->Form.NonResident.DataSize; NtfsDecodeRuns(a,runs,64,&rc); bm=malloc((size_t)bytes); rd_runs(runs,rc,0,bm,(ULONG)bytes);
    for(i=16;i<bytes*8;i++) if(!(bm[i>>3]&(1u<<(i&7)))){ found=i; break; }
    if(found==(ULONGLONG)-1) die("mft full"); bm[found>>3]|=(1u<<(found&7)); wr_runs(runs,rc,found>>3,bm+(found>>3),1);
    free(bm); free(r); return found;
}

int main(int argc,char**argv){
    unsigned char boot[512],r0[4096],*ext,*frec; PMFT_RECORD em; PATTRIBUTE_RECORD a;
    WCHAR wn[8]; int n; ULONGLONG rn,parentRef,fileRef; unsigned char ent[512];
    PINDEX_ENTRY ie=(PINDEX_ENTRY)ent; PFILE_NAME_ATTR efn; ULONG keyLen,entLen; const char*nm="small.txt";
    if(argc<2){ fprintf(stderr,"usage: %s <img>\n",argv[0]); return 2; }
    fd=open(argv[1],O_RDWR); if(fd<0) die("open");
    rd(boot,512,0); if(!NtfsParseBoot(boot,512,&g)) die("boot");
    rd(r0,g.MftRecordSize,g.MftLcn*(ULONGLONG)g.ClusterSize); NtfsApplyFixup(r0,g.MftRecordSize,g.BytesPerSector);
    a=NtfsFindAttribute((PMFT_RECORD)r0,ATTR_DATA,NULL,0); if(!NtfsDecodeRuns(a,mftRuns,256,&mftRunCount)) die("mft runs");
    n=0; while(nm[n]){ wn[n]=(WCHAR)nm[n]; n++; }

    ext=malloc(g.MftRecordSize); frec=malloc(g.MftRecordSize);
    read_mft(MFT_RN_EXTEND,ext); em=(PMFT_RECORD)ext;
    if(!(em->Flags&MFT_RECORD_IS_DIRECTORY)) die("$Extend not a dir");
    parentRef=((ULONGLONG)em->SequenceNumber<<48)|MFT_RN_EXTEND;
    rn=alloc_mft();
    NtfsBuildFileRecord(frec,g.MftRecordSize,g.BytesPerSector,rn,1,parentRef,wn,n,0x01DC000000000000ULL,"hi",2);
    write_mft(rn,frec); fileRef=((ULONGLONG)1<<48)|rn;

    memset(ent,0,sizeof(ent)); efn=(PFILE_NAME_ATTR)(ent+sizeof(INDEX_ENTRY));
    efn->ParentDirectory=parentRef; efn->DataSize=2; efn->AllocatedSize=8; efn->FileAttributes=0x80;
    efn->NameLength=(UCHAR)n; efn->NameType=NTFS_FNAME_WIN32DOS; memcpy((PUCHAR)efn+sizeof(FILE_NAME_ATTR),wn,n*2);
    keyLen=sizeof(FILE_NAME_ATTR)+n*2; ie->FileReference=fileRef; ie->KeyLength=(USHORT)keyLen; ie->Flags=0;
    entLen=(sizeof(INDEX_ENTRY)+keyLen+7)&~7u; ie->Length=(USHORT)entLen;

    if(!NtfsIndexRootInsert(em,g.MftRecordSize,ent,entLen)) die("root insert");
    write_mft(MFT_RN_EXTEND,ext);
    printf("inserted '%s' into $Extend (small index) as record %llu\n",nm,(unsigned long long)rn);

    /* now remove it again */
    read_mft(MFT_RN_EXTEND,ext); em=(PMFT_RECORD)ext;
    if(!NtfsIndexRootRemove(em,g.MftRecordSize,wn,n)) die("root remove");
    write_mft(MFT_RN_EXTEND,ext);
    printf("removed '%s' from $Extend\n",nm);
    close(fd);
    return 0;
}
