/*
 * NTFS3NT - user-mode validation of directory B-tree LEAF SPLIT (and large-index
 * create) using the exact shared engine the kernel runs: NtfsBuildFileRecord,
 * NtfsIndexInsert and NtfsPromoteSplit. Creates many files in the root of a
 * volume made by our own ntfsfmt, forcing one or more leaf splits, then ntfs-3g
 * verifies every name is listed, content reads back, and the volume is clean.
 *
 * Build: cc -I../src ntfsmany.c ../src/ntfssup.c -o ntfsmany
 * Run:   ./ntfsmany <image> <count>
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
static ULONGLONG now_ft(void){ return 0x01DC000000000000ULL; }

static ULONGLONG rmap(PNTFS_RUN runs,ULONG c,ULONGLONG vbo)
{ ULONG cs=g.ClusterSize; ULONGLONG l,lf; BOOLEAN sp;
  if(!NtfsMapVcn(runs,c,vbo/cs,&l,&lf,&sp)) die("unmapped"); return l*(ULONGLONG)cs+(vbo%cs); }
static void wr_runs(PNTFS_RUN runs,ULONG c,ULONGLONG vbo,const void*buf,ULONG len)
{ const unsigned char*p=buf; ULONG cs=g.ClusterSize;
  while(len){ ULONGLONG l,lf; BOOLEAN sp; ULONG in=vbo%cs,ch;
    if(!NtfsMapVcn(runs,c,vbo/cs,&l,&lf,&sp)) die("wr unmapped");
    ch=(ULONG)(lf*cs-in); if(ch>len)ch=len; wr(p,ch,l*cs+in); p+=ch; vbo+=ch; len-=ch; } }
static void rd_runs(PNTFS_RUN runs,ULONG c,ULONGLONG vbo,void*buf,ULONG len)
{ unsigned char*p=buf; ULONG cs=g.ClusterSize;
  while(len){ ULONGLONG l,lf; BOOLEAN sp; ULONG in=vbo%cs,ch;
    if(!NtfsMapVcn(runs,c,vbo/cs,&l,&lf,&sp)) die("rd unmapped");
    ch=(ULONG)(lf*cs-in); if(ch>len)ch=len; rd(p,ch,l*cs+in); p+=ch; vbo+=ch; len-=ch; } }

static void read_mft(ULONGLONG rn,void*b){ rd(b,g.MftRecordSize,rmap(mftRuns,mftRunCount,rn*g.MftRecordSize));
  if(!NtfsApplyFixup(b,g.MftRecordSize,g.BytesPerSector)) die("fixup"); }
static void write_mft(ULONGLONG rn,void*b){ NtfsSetFixup(b,g.MftRecordSize,g.BytesPerSector);
  wr(b,g.MftRecordSize,rmap(mftRuns,mftRunCount,rn*g.MftRecordSize)); }

/* scan a bitmap attribute (record rn, attr type, name) for a free run; mark + persist */
static ULONGLONG bmp_alloc(ULONGLONG rn,ULONG atype,const WCHAR*nm,ULONG nmlen,ULONG count,ULONGLONG start,ULONGLONG limit)
{
    unsigned char *r=malloc(g.MftRecordSize); PMFT_RECORD m; PATTRIBUTE_RECORD a;
    NTFS_RUN runs[64]; ULONG rc; ULONGLONG bytes,i,found=(ULONGLONG)-1; ULONG run=0; unsigned char*bm;
    read_mft(rn,r); m=(PMFT_RECORD)r;
    a=NtfsFindAttribute(m,atype,nm,nmlen); if(!a||!a->NonResident) die("bmp nonres");
    bytes=a->Form.NonResident.DataSize; NtfsDecodeRuns(a,runs,64,&rc);
    bm=malloc((size_t)bytes); rd_runs(runs,rc,0,bm,(ULONG)bytes);
    if(limit>bytes*8) limit=bytes*8;
    for(i=start;i<limit;i++){ if(!(bm[i>>3]&(1u<<(i&7)))){ if(run==0)found=i; if(++run==count)break; } else {run=0;found=(ULONGLONG)-1;} }
    if(run<count||found==(ULONGLONG)-1) die("bitmap full");
    for(i=found;i<found+count;i++) bm[i>>3]|=(1u<<(i&7));
    { ULONGLONG b0=found>>3,b1=(found+count-1)>>3; wr_runs(runs,rc,b0,bm+b0,(ULONG)(b1-b0+1)); }
    free(bm); free(r); return found;
}
static const WCHAR I30[4]={'$','I','3','0'};
static ULONGLONG alloc_mft(void){ return bmp_alloc(MFT_RN_MFT,ATTR_BITMAP,NULL,0,1,16,1ULL<<40); }
static ULONGLONG alloc_clusters(ULONG n){ return bmp_alloc(MFT_RN_BITMAP,ATTR_DATA,NULL,0,n,0,g.TotalClusters); }

static int ascii_w(const char*s,WCHAR*w){ int n=0; while(s[n]){ w[n]=(WCHAR)s[n]; n++; } return n; }

static ULONGLONG find_leaf(PMFT_RECORD root,const WCHAR*wn,int n){
    PATTRIBUTE_RECORD ir=NtfsFindAttribute(root,ATTR_INDEX_ROOT,I30,4); PINDEX_ROOT r; PINDEX_ENTRY e;
    if(!ir) die("no $INDEX_ROOT"); r=(PINDEX_ROOT)NtfsResidentValue(ir);
    if(!(r->Header.Flags&1)) return (ULONGLONG)-1;
    for(e=NtfsFirstIndexEntry(&r->Header);;e=NtfsNextIndexEntry(e)){
        if(e->Flags&INDEX_ENTRY_END) return NtfsIndexSubnodeVcn(e);
        { PFILE_NAME_ATTR fn=(PFILE_NAME_ATTR)((PUCHAR)e+sizeof(INDEX_ENTRY));
          WCHAR*en=(WCHAR*)((PUCHAR)fn+sizeof(FILE_NAME_ATTR));
          if(NtfsCollateName(wn,n,en,fn->NameLength)<=0)
              return (e->Flags&INDEX_ENTRY_HAS_SUBNODE)?NtfsIndexSubnodeVcn(e):(ULONGLONG)-1; }
    }
}

static void create_file(const char*name,const char*text){
    static unsigned char root[4096],frec[4096],leaf[4096],low[4096],high[4096];
    WCHAR wn[128]; int n=ascii_w(name,wn); ULONG dlen=(ULONG)strlen(text);
    PMFT_RECORD m; PATTRIBUTE_RECORD ia; ULONGLONG rn,parentRef,fileRef,leafVcn;
    unsigned char ent[600]; PINDEX_ENTRY ie=(PINDEX_ENTRY)ent; PFILE_NAME_ATTR efn;
    ULONG keyLen,entLen,ibs=g.IndexBlockSize; NTFS_RUN iruns[64]; ULONG ic;

    read_mft(MFT_RN_ROOT,root); m=(PMFT_RECORD)root;
    parentRef=((ULONGLONG)m->SequenceNumber<<48)|MFT_RN_ROOT;
    rn=alloc_mft();
    NtfsBuildFileRecord(frec,g.MftRecordSize,g.BytesPerSector,rn,1,parentRef,wn,n,now_ft(),text,dlen);
    write_mft(rn,frec);
    fileRef=((ULONGLONG)1<<48)|rn;

    memset(ent,0,sizeof(ent)); efn=(PFILE_NAME_ATTR)(ent+sizeof(INDEX_ENTRY));
    efn->ParentDirectory=parentRef; efn->AllocatedSize=(dlen+7)&~7u; efn->DataSize=dlen;
    efn->FileAttributes=0x80; efn->NameLength=(UCHAR)n; efn->NameType=NTFS_FNAME_WIN32DOS;
    memcpy((PUCHAR)efn+sizeof(FILE_NAME_ATTR),wn,n*2);
    keyLen=sizeof(FILE_NAME_ATTR)+n*2; ie->FileReference=fileRef; ie->KeyLength=(USHORT)keyLen;
    ie->Flags=0; entLen=(sizeof(INDEX_ENTRY)+keyLen+7)&~7u; ie->Length=(USHORT)entLen;

    leafVcn=find_leaf(m,wn,n);
    if(leafVcn==(ULONGLONG)-1){
        if(!NtfsIndexRootInsert(m,g.MftRecordSize,ent,entLen)) die("root insert");
        write_mft(MFT_RN_ROOT,root); return;
    }
    ia=NtfsFindAttribute(m,ATTR_INDEX_ALLOCATION,I30,4); if(!ia) die("no $IA");
    NtfsDecodeRuns(ia,iruns,64,&ic);
    { ULONGLONG off=leafVcn*ibs; rd_runs(iruns,ic,off,leaf,ibs);
      if(!NtfsApplyFixup(leaf,ibs,g.BytesPerSector)) die("indx fixup"); }
    { PINDEX_BLOCK ib=(PINDEX_BLOCK)leaf; ULONG cap=ibs-0x18;
      if(NtfsIndexInsert(&ib->Header,cap,ent,entLen)){
          NtfsSetFixup(leaf,ibs,g.BytesPerSector); wr_runs(iruns,ic,leafVcn*ibs,leaf,ibs); return; }
    }
    /* leaf full -> split (mirror kernel NtfsSplitLeaf) */
    { ULONG cs=g.ClusterSize, blk=(ibs+cs-1)/cs; ULONGLONG newVcn,lcn,oldAlloc;
      PATTRIBUTE_RECORD bmp;
      oldAlloc=ia->Form.NonResident.AllocatedSize; newVcn=oldAlloc/ibs;
      lcn=alloc_clusters(blk);
      if(!iruns[ic-1].Sparse && iruns[ic-1].Lcn+iruns[ic-1].Length==lcn) iruns[ic-1].Length+=blk;
      else { iruns[ic].Vcn=iruns[ic-1].Vcn+iruns[ic-1].Length; iruns[ic].Lcn=lcn; iruns[ic].Length=blk; iruns[ic].Sparse=FALSE; ic++; }
      if(!NtfsRewriteAttrRuns(m,g.MftRecordSize,ia,iruns,ic,oldAlloc+(ULONGLONG)blk*cs,oldAlloc+(ULONGLONG)blk*cs)) die("rewrite IA");
      ia=NtfsFindAttribute(m,ATTR_INDEX_ALLOCATION,I30,4); NtfsDecodeRuns(ia,iruns,64,&ic);
      bmp=NtfsFindAttribute(m,ATTR_BITMAP,I30,4);
      if(bmp && !bmp->NonResident){ PUCHAR bits=(PUCHAR)NtfsResidentValue(bmp);
        if((newVcn>>3)<NtfsResidentValueLength(bmp)) bits[newVcn>>3]|=(UCHAR)(1u<<(newVcn&7)); }
      if(!NtfsPromoteSplit(m,g.MftRecordSize,leaf,low,high,ibs,g.BytesPerSector,leafVcn,newVcn,ent,entLen)) die("promote split (root overflow)");
      NtfsSetFixup(low,ibs,g.BytesPerSector);  wr_runs(iruns,ic,leafVcn*ibs,low,ibs);
      NtfsSetFixup(high,ibs,g.BytesPerSector); wr_runs(iruns,ic,newVcn*ibs,high,ibs);
      write_mft(MFT_RN_ROOT,root);
    }
}

int main(int argc,char**argv){
    unsigned char boot[512],r0[4096]; PATTRIBUTE_RECORD a; int i,count;
    char name[64],text[64];
    if(argc<3){ fprintf(stderr,"usage: %s <image> <count>\n",argv[0]); return 2; }
    fd=open(argv[1],O_RDWR); if(fd<0) die("open"); count=atoi(argv[2]);
    rd(boot,512,0); if(!NtfsParseBoot(boot,512,&g)) die("boot");
    rd(r0,g.MftRecordSize,g.MftLcn*(ULONGLONG)g.ClusterSize); NtfsApplyFixup(r0,g.MftRecordSize,g.BytesPerSector);
    a=NtfsFindAttribute((PMFT_RECORD)r0,ATTR_DATA,NULL,0);
    if(!NtfsDecodeRuns(a,mftRuns,256,&mftRunCount)) die("mft runs");
    for(i=0;i<count;i++){ sprintf(name,"file%04d.txt",i); sprintf(text,"content-of-file-%04d",i); create_file(name,text); }
    printf("created %d files\n",count);
    return 0;
}
