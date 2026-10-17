/*
 * NTFS3NT - user-mode create/delete check for the write engine. Creates a small
 * file (resident $DATA) in the root directory - allocating an MFT record from
 * $MFT's $BITMAP, building the FILE record, and inserting the $FILE_NAME key
 * into the directory's $INDEX_ALLOCATION leaf - then (optionally) deletes it.
 * ntfs-3g then verifies the result (ntfsls / ntfscat / ntfsfix).
 *
 * Build: cc -I../src ntfscreate.c ../src/ntfssup.c -o ntfscreate
 * Run:   ./ntfscreate <image> create <name> <text>
 *        ./ntfscreate <image> delete <name>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "ntfsusr.h"
#include "ntfssup.h"

static int g_fd;
static NTFS_GEOMETRY g_geo;
static NTFS_RUN g_mftRuns[256];
static ULONG g_mftRunCount;

static void die(const char *m){ fprintf(stderr,"ERROR: %s\n",m); exit(1); }
static void rd(void*b,ULONG n,ULONGLONG o){ if(pread(g_fd,b,n,(off_t)o)!=(ssize_t)n) die("pread"); }
static void wr(const void*b,ULONG n,ULONGLONG o){ if(pwrite(g_fd,b,n,(off_t)o)!=(ssize_t)n) die("pwrite"); }
/* INDX block I/O in the units NTFS uses: VCNs count clusters when an index
   block is at least a cluster, else 512-byte blocks (clusters > 4 KB). */
static int idx_io(int write, NTFS_RUN *iruns, ULONG ic, ULONGLONG vcn, unsigned char *buf);

static ULONGLONG map(PNTFS_RUN r,ULONG c,ULONGLONG vbo){
    ULONG cs=g_geo.ClusterSize; ULONGLONG l,left; BOOLEAN sp;
    if(!NtfsMapVcn(r,c,vbo/cs,&l,&left,&sp)) die("unmapped");
    return l*(ULONGLONG)cs + (vbo%cs);
}
static ULONGLONG mftoff(ULONGLONG rn){ return map(g_mftRuns,g_mftRunCount,rn*(ULONGLONG)g_geo.MftRecordSize); }

static void read_rec(ULONGLONG rn, void*buf){
    rd(buf,g_geo.MftRecordSize,mftoff(rn));
    if(!NtfsApplyFixup(buf,g_geo.MftRecordSize,g_geo.BytesPerSector)) die("rec fixup");
}
static void write_rec(ULONGLONG rn, void*buf){
    NtfsSetFixup(buf,g_geo.MftRecordSize,g_geo.BytesPerSector);
    wr(buf,g_geo.MftRecordSize,mftoff(rn));
}

static ULONGLONG now_filetime(void){
    /* seconds since 1970 -> 100ns since 1601 */
    return ((ULONGLONG)time(NULL) + 11644473600ULL) * 10000000ULL;
}

/* Max MFT records that currently exist (from $MFT's $DATA size). */
static ULONG mft_record_count(void){
    static unsigned char r0[4096]; PATTRIBUTE_RECORD d;
    read_rec(MFT_RN_MFT,r0);
    d=NtfsFindAttribute((PMFT_RECORD)r0,ATTR_DATA,NULL,0);
    if(!d||!d->NonResident) die("$MFT $DATA");
    return (ULONG)(d->Form.NonResident.DataSize / g_geo.MftRecordSize);
}

/*
 * Allocate a free MFT record from $MFT's $BITMAP (record 0), within the records
 * that already exist (16..count) - record 16 onward, leaving 0-15 to the system.
 * (Growing the MFT when no slot is free is a later step.)
 */
static ULONGLONG alloc_mft_record(void){
    static unsigned char r0[4096];
    PMFT_RECORD m; PATTRIBUTE_RECORD b;
    ULONG maxrec=mft_record_count(), i;
    read_rec(MFT_RN_MFT, r0);
    m=(PMFT_RECORD)r0;
    b=NtfsFindAttribute(m,ATTR_BITMAP,NULL,0);
    if(!b) die("$MFT $BITMAP not found");
    if(!b->NonResident){
        unsigned char *bm=NtfsResidentValue(b);
        for(i=16;i<maxrec;i++){ if(!(bm[i/8]&(1<<(i&7)))){
            bm[i/8]|=(1<<(i&7)); write_rec(MFT_RN_MFT,r0); return i; } }
        die("no free MFT record (growing the MFT not yet implemented)");
    } else {
        NTFS_RUN runs[32]; ULONG rc; static unsigned char clu[65536];
        ULONG cs=g_geo.ClusterSize; ULONGLONG lcn,left; BOOLEAN sp;
        if(!NtfsDecodeRuns(b,runs,32,&rc)) die("bitmap runs");
        if(!NtfsMapVcn(runs,rc,0,&lcn,&left,&sp)) die("bitmap vcn0");
        rd(clu,cs,lcn*(ULONGLONG)cs);
        for(i=16;i<maxrec;i++){ if(!(clu[i/8]&(1<<(i&7)))){
            clu[i/8]|=(1<<(i&7)); wr(clu,cs,lcn*(ULONGLONG)cs); return i; } }
        die("no free MFT record (growing the MFT not yet implemented)");
    }
    return 0;
}

static void free_mft_bit(ULONGLONG rn){
    static unsigned char r0[4096];
    PMFT_RECORD m; PATTRIBUTE_RECORD b;
    read_rec(MFT_RN_MFT,r0); m=(PMFT_RECORD)r0;
    b=NtfsFindAttribute(m,ATTR_BITMAP,NULL,0);
    if(b && !b->NonResident){ unsigned char*bm=NtfsResidentValue(b);
        bm[rn/8]&=~(1<<(rn&7)); write_rec(MFT_RN_MFT,r0); return; }
    if(b){ NTFS_RUN runs[32]; ULONG rc; static unsigned char clu[65536];
        ULONG cs=g_geo.ClusterSize; ULONGLONG lcn,left; BOOLEAN sp;
        NtfsDecodeRuns(b,runs,32,&rc); NtfsMapVcn(runs,rc,0,&lcn,&left,&sp);
        rd(clu,cs,lcn*(ULONGLONG)cs); clu[rn/8]&=~(1<<(rn&7)); wr(clu,cs,lcn*(ULONGLONG)cs); }
}

/* add u16 le name into buf, return chars */
static int ascii_to_utf16(const char*s, WCHAR*out){ int i=0; while(s[i]){ out[i]=(unsigned char)s[i]; i++; } return i; }

/* Build a FILE record for a regular file with resident data. */
static void build_file_record(void*buf, ULONGLONG rn, USHORT seq,
                              ULONGLONG parentRef, const char*name,
                              const void*data, ULONG dataLen)
{
    unsigned char *p=(unsigned char*)buf;
    PMFT_RECORD m=(PMFT_RECORD)buf;
    WCHAR wname[256]; int wn=ascii_to_utf16(name,wname);
    ULONG off, usaCount=g_geo.MftRecordSize/g_geo.BytesPerSector + 1;
    ULONG usaOff=0x30;
    PATTRIBUTE_RECORD a;
    ULONGLONG t=now_filetime();

    memset(p,0,g_geo.MftRecordSize);
    m->Rhdr.Type=NTFS_FILE_SIGNATURE;
    m->Rhdr.UsaOffset=(USHORT)usaOff;
    m->Rhdr.UsaCount=(USHORT)usaCount;
    m->SequenceNumber=seq;
    m->HardLinkCount=1;
    m->FirstAttributeOffset=(USHORT)((usaOff + usaCount*2 + 7)&~7);
    m->Flags=MFT_RECORD_IN_USE;
    m->BytesAllocated=g_geo.MftRecordSize;
    m->NextAttributeId=0;
    m->MftRecordNumber=(ULONG)rn;
    off=m->FirstAttributeOffset;

    /* $STANDARD_INFORMATION (resident) */
    a=(PATTRIBUTE_RECORD)(p+off);
    a->Type=ATTR_STANDARD_INFORMATION;
    a->NonResident=0; a->NameLength=0; a->NameOffset=0x18;
    a->AttributeId=m->NextAttributeId++;
    a->Form.Resident.ValueLength=0x48; a->Form.Resident.ValueOffset=0x18;
    a->Length=(0x18+0x48+7)&~7;
    { PSTANDARD_INFORMATION si=(PSTANDARD_INFORMATION)((unsigned char*)a+0x18);
      si->CreationTime=si->ChangeTime=si->MftChangeTime=si->LastAccessTime=t;
      si->FileAttributes=0x80; /* FILE_ATTRIBUTE_NORMAL */ }
    off+=a->Length;

    /* $FILE_NAME (resident, indexed) */
    a=(PATTRIBUTE_RECORD)(p+off);
    a->Type=ATTR_FILE_NAME;
    a->NonResident=0; a->NameLength=0; a->NameOffset=0x18;
    a->AttributeId=m->NextAttributeId++;
    a->Form.Resident.IndexedFlag=1;
    { ULONG vlen=sizeof(FILE_NAME_ATTR)+wn*2;
      a->Form.Resident.ValueLength=vlen; a->Form.Resident.ValueOffset=0x18;
      a->Length=(0x18+vlen+7)&~7;
      PFILE_NAME_ATTR fn=(PFILE_NAME_ATTR)((unsigned char*)a+0x18);
      memset(fn,0,sizeof(*fn));
      fn->ParentDirectory=parentRef;
      fn->CreationTime=fn->ChangeTime=fn->MftChangeTime=fn->LastAccessTime=t;
      fn->AllocatedSize=(dataLen+7)&~7; fn->DataSize=dataLen;
      fn->FileAttributes=0x80;
      fn->NameLength=(UCHAR)wn; fn->NameType=NTFS_FNAME_WIN32DOS;
      memcpy((unsigned char*)fn+sizeof(FILE_NAME_ATTR),wname,wn*2);
    }
    off+=a->Length;

    /* $DATA (resident) */
    a=(PATTRIBUTE_RECORD)(p+off);
    a->Type=ATTR_DATA;
    a->NonResident=0; a->NameLength=0; a->NameOffset=0x18;
    a->AttributeId=m->NextAttributeId++;
    a->Form.Resident.ValueLength=dataLen; a->Form.Resident.ValueOffset=0x18;
    a->Length=(0x18+dataLen+7)&~7;
    memcpy((unsigned char*)a+0x18,data,dataLen);
    off+=a->Length;

    /* end marker */
    *(ULONG*)(p+off)=ATTR_END;
    off+=8;
    m->BytesInUse=off;
}

/* Descend INDEX_ROOT -> leaf INDX block that should hold `name`; returns the
 * block VCN, or (ULONGLONG)-1 if the index is entirely resident in the root. */
static ULONGLONG find_leaf_vcn(PMFT_RECORD root, const WCHAR*wname, int wn){
    PATTRIBUTE_RECORD ir=NtfsFindAttribute(root,ATTR_INDEX_ROOT,(const WCHAR[]){'$','I','3','0'},4);
    PINDEX_ROOT r; PINDEX_ENTRY e;
    if(!ir) die("no $INDEX_ROOT");
    r=(PINDEX_ROOT)NtfsResidentValue(ir);
    if(!(r->Header.Flags & 1)) return (ULONGLONG)-1;   /* small index, resident */
    for(e=NtfsFirstIndexEntry(&r->Header); ; e=(PINDEX_ENTRY)((PUCHAR)e+e->Length)){
        if(e->Flags & INDEX_ENTRY_END) return NtfsIndexSubnodeVcn(e);
        { PFILE_NAME_ATTR fn=(PFILE_NAME_ATTR)((PUCHAR)e+sizeof(INDEX_ENTRY));
          WCHAR*en=(WCHAR*)((PUCHAR)fn+sizeof(FILE_NAME_ATTR));
          if(NtfsCollateName(wname,wn,en,fn->NameLength)<=0)
              return (e->Flags&INDEX_ENTRY_HAS_SUBNODE)?NtfsIndexSubnodeVcn(e):(ULONGLONG)-1;
        }
        if(e->Length==0) die("bad root index");
    }
}

int main(int argc,char**argv){
    unsigned char boot[512]; static unsigned char r0[4096];
    unsigned char *root,*indx; PMFT_RECORD m; PATTRIBUTE_RECORD a;
    const char *op,*name;
    WCHAR wname[256]; int wn;

    if(argc<4){ fprintf(stderr,"usage: %s <img> create <name> <text> | %s <img> delete <name>\n",argv[0],argv[0]); return 2; }
    g_fd=open(argv[1],O_RDWR); if(g_fd<0) die("open");
    op=argv[2]; name=argv[3];
    wn=ascii_to_utf16(name,wname);

    rd(boot,sizeof(boot),0);
    if(!NtfsParseBoot(boot,sizeof(boot),&g_geo)) die("not NTFS");
    rd(r0,g_geo.MftRecordSize,g_geo.MftLcn*(ULONGLONG)g_geo.ClusterSize);
    if(!NtfsApplyFixup(r0,g_geo.MftRecordSize,g_geo.BytesPerSector)) die("rec0 fixup");
    a=NtfsFindAttribute((PMFT_RECORD)r0,ATTR_DATA,NULL,0);
    if(!a||!a->NonResident) die("$MFT data");
    if(!NtfsDecodeRuns(a,g_mftRuns,256,&g_mftRunCount)) die("mft runs");

    root=malloc(g_geo.MftRecordSize);
    indx=malloc(g_geo.IndexBlockSize);
    read_rec(MFT_RN_ROOT,root); m=(PMFT_RECORD)root;

    if(strcmp(op,"create")==0){
        const char *text=(argc>4)?argv[4]:"";
        ULONG dlen=(ULONG)strlen(text);
        ULONGLONG rn=alloc_mft_record();
        USHORT seq=1;
        ULONGLONG parentRef=((ULONGLONG)m->SequenceNumber<<48)|MFT_RN_ROOT;
        ULONGLONG fileRef;
        static unsigned char frec[4096];
        unsigned char entbuf[512]; PINDEX_ENTRY ie=(PINDEX_ENTRY)entbuf;
        ULONG entLen; PFILE_NAME_ATTR efn;
        ULONGLONG leafVcn;

        /* use the SHARED builder the kernel create path uses */
        NtfsBuildFileRecord(frec,g_geo.MftRecordSize,g_geo.BytesPerSector,
                            rn,seq,parentRef,wname,wn,now_filetime(),text,dlen);
        write_rec(rn,frec);
        fileRef=((ULONGLONG)seq<<48)|rn;

        /* build the index entry: header + FILE_NAME key */
        memset(entbuf,0,sizeof(entbuf));
        efn=(PFILE_NAME_ATTR)(entbuf+sizeof(INDEX_ENTRY));
        efn->ParentDirectory=parentRef;
        efn->CreationTime=efn->ChangeTime=efn->MftChangeTime=efn->LastAccessTime=now_filetime();
        efn->AllocatedSize=(dlen+7)&~7; efn->DataSize=dlen; efn->FileAttributes=0x80;
        efn->NameLength=(UCHAR)wn; efn->NameType=NTFS_FNAME_WIN32DOS;
        memcpy((unsigned char*)efn+sizeof(FILE_NAME_ATTR),wname,wn*2);
        { ULONG keyLen=sizeof(FILE_NAME_ATTR)+wn*2;
          ie->FileReference=fileRef; ie->KeyLength=(USHORT)keyLen;
          ie->Flags=0; entLen=(sizeof(INDEX_ENTRY)+keyLen+7)&~7; ie->Length=(USHORT)entLen; }

        leafVcn=find_leaf_vcn(m,wname,wn);
        if(leafVcn==(ULONGLONG)-1) die("resident-root insert not supported in this test (need large index)");

        /* read the leaf INDX block, insert, write back */
        a=NtfsFindAttribute(m,ATTR_INDEX_ALLOCATION,(const WCHAR[]){'$','I','3','0'},4);
        { NTFS_RUN iruns[64]; ULONG ic; PINDEX_BLOCK ib;
          if(!NtfsDecodeRuns(a,iruns,64,&ic)) die("ia runs");
          if(!idx_io(0,iruns,ic,leafVcn,indx)) die("read leaf");
          ib=(PINDEX_BLOCK)indx;
          if(ib->Rhdr.Type!=NTFS_INDX_SIGNATURE) die("not INDX");
          if(!NtfsApplyFixup(indx,g_geo.IndexBlockSize,g_geo.BytesPerSector)) die("indx fixup");
          { ULONG cap=g_geo.IndexBlockSize - (ULONG)((PUCHAR)&ib->Header-(PUCHAR)ib);
            if(!NtfsIndexInsert(&ib->Header,cap,entbuf,entLen)) die("leaf full (node split not yet implemented)"); }
          NtfsSetFixup(indx,g_geo.IndexBlockSize,g_geo.BytesPerSector);
          if(!idx_io(1,iruns,ic,leafVcn,indx)) die("write leaf");
        }
        fsync(g_fd);
        printf("created '%s' as MFT record %llu\n",name,(unsigned long long)rn);
    }
    else if(strcmp(op,"delete")==0){
        ULONGLONG leafVcn=find_leaf_vcn(m,wname,wn);
        NTFS_RUN iruns[64]; ULONG ic; PINDEX_BLOCK ib; ULONGLONG fileRef=0;
        if(leafVcn==(ULONGLONG)-1) die("resident-root delete not supported in this test");
        a=NtfsFindAttribute(m,ATTR_INDEX_ALLOCATION,(const WCHAR[]){'$','I','3','0'},4);
        if(!NtfsDecodeRuns(a,iruns,64,&ic)) die("ia runs");
        if(!idx_io(0,iruns,ic,leafVcn,indx)) die("read leaf");
        ib=(PINDEX_BLOCK)indx;
        if(!NtfsApplyFixup(indx,g_geo.IndexBlockSize,g_geo.BytesPerSector)) die("indx fixup");
        { PINDEX_ENTRY e;
          for(e=NtfsFirstIndexEntry(&ib->Header); e && !(e->Flags&INDEX_ENTRY_END); e=NtfsNextIndexEntry(e)){
            PFILE_NAME_ATTR fn=(PFILE_NAME_ATTR)((PUCHAR)e+sizeof(INDEX_ENTRY));
            WCHAR*en=(WCHAR*)((PUCHAR)fn+sizeof(FILE_NAME_ATTR));
            if(fn->NameLength==(UCHAR)wn && NtfsCollateName(wname,wn,en,fn->NameLength)==0){ fileRef=e->FileReference; break; } } }
        if(!NtfsIndexRemove(&ib->Header,wname,wn)) die("name not in index");
        NtfsSetFixup(indx,g_geo.IndexBlockSize,g_geo.BytesPerSector);
        if(!idx_io(1,iruns,ic,leafVcn,indx)) die("write leaf");
        /* free the MFT record: clear IN_USE, bump sequence, clear bitmap bit */
        if(fileRef){ ULONGLONG rn=NTFS_MREF_RECORD(fileRef); static unsigned char fr[4096];
          read_rec(rn,fr); ((PMFT_RECORD)fr)->Flags&=~MFT_RECORD_IN_USE;
          ((PMFT_RECORD)fr)->SequenceNumber++; write_rec(rn,fr); free_mft_bit(rn); }
        fsync(g_fd);
        printf("deleted '%s'\n",name);
    }
    else die("op must be create or delete");

    close(g_fd);
    return 0;
}

static int idx_io(int write, NTFS_RUN *iruns, ULONG ic, ULONGLONG vcn, unsigned char *buf)
{
    ULONG cs=g_geo.ClusterSize, ibs=g_geo.IndexBlockSize, done=0;
    ULONGLONG vbo = vcn * (ULONGLONG)((ibs >= cs) ? cs : 512);
    while (done < ibs) {
        ULONGLONG lcn, left; BOOLEAN sp;
        ULONGLONG at = vbo + done; ULONG in = (ULONG)(at % cs), n = cs - in;
        if (n > ibs - done) n = ibs - done;
        if (!NtfsMapVcn(iruns, ic, at / cs, &lcn, &left, &sp) || sp) return 0;
        if (write) wr(buf + done, n, lcn * (ULONGLONG)cs + in); else rd(buf + done, n, lcn * (ULONGLONG)cs + in);
        done += n;
    }
    return 1;
}
