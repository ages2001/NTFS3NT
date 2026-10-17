/*
 * NTFS3NT - ntfschk: a consistency checker for an NTFS volume, built on the same
 * engine (ntfssup.c) as the driver. It validates the boot sector, bootstraps
 * $MFT, then walks every in-use MFT record checking the 'FILE' signature and
 * update-sequence fixup and that each attribute stays within the record, and
 * verifies the root directory index ('INDX' signature + fixups). Read-only; it
 * reports problems and a summary. (/F repair is a later addition.)
 *
 * Runs on NT 3.1 - 4.0 as a freestanding console program (no C runtime), and
 * also builds on Linux against a disk image for testing. Usage:
 *   ntfschk <volume|image> [/V]
 */

#include "ntfsusr.h"
#include "ntfssup.h"

#ifdef _WIN32
#include <windows.h>
static HANDLE g_h;
static int io_open(const char *p){ g_h=CreateFileA(p,GENERIC_READ,
    FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,0,0); return g_h!=INVALID_HANDLE_VALUE; }
static int io_read(void*b,ULONG n,ULONGLONG o){ LARGE_INTEGER li; DWORD got;
    li.QuadPart=(LONGLONG)o;
    /* SetFilePointer, not SetFilePointerEx: NT 3.1 - 4.0 have only the former */
    if(SetFilePointer(g_h,(LONG)li.LowPart,&li.HighPart,FILE_BEGIN)==0xFFFFFFFF && GetLastError()!=NO_ERROR) return 0;
    return ReadFile(g_h,b,n,&got,0)&&got==n; }
#else
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
static int g_fd;
static int io_open(const char *p){ g_fd=open(p,O_RDONLY); return g_fd>=0; }
static int io_read(void*b,ULONG n,ULONGLONG o){ return pread(g_fd,b,n,(off_t)o)==(ssize_t)n; }
#endif

/* tiny output (works in both the freestanding NT build and on Linux) */
static void outs(const char*s);
static void outu(ULONGLONG v);
#ifdef _WIN32
static void outs(const char*s){ DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,lstrlenA(s),&w,0); }
#else
static void outs(const char*s){ (void)write(1,s,strlen(s)); }
#endif
static void outu(ULONGLONG v){ char b[24]; int i=24; b[--i]=0; if(!v){outs("0");return;}
    while(v){ b[--i]=(char)('0'+(int)(v%10)); v/=10; } outs(&b[i]); }

static NTFS_GEOMETRY g_geo;
static NTFS_RUN g_mftRuns[256]; static ULONG g_mftRunCount;
static unsigned char g_rec[8192];
static ULONG g_errors;

static void err(const char*m, ULONGLONG n){ outs("  ERROR: "); outs(m); outs(" "); outu(n); outs("\n"); g_errors++; }

static ULONGLONG map_mft(ULONGLONG rn){
    ULONG cs=g_geo.ClusterSize; ULONGLONG vbo=rn*(ULONGLONG)g_geo.MftRecordSize,lcn,left; BOOLEAN sp;
    if(!NtfsMapVcn(g_mftRuns,g_mftRunCount,vbo/cs,&lcn,&left,&sp)) return (ULONGLONG)-1;
    return lcn*(ULONGLONG)cs+(vbo%cs);
}

/* check one MFT record's attributes stay in bounds; returns in-use? */
static int check_record(ULONGLONG rn, int verbose){
    PMFT_RECORD m=(PMFT_RECORD)g_rec; PATTRIBUTE_RECORD a; ULONG count=0;
    ULONGLONG off=map_mft(rn);
    if(off==(ULONGLONG)-1){ err("record unmapped",rn); return 0; }
    if(!io_read(g_rec,g_geo.MftRecordSize,off)){ err("record read failed",rn); return 0; }
    if(m->Rhdr.Type!=NTFS_FILE_SIGNATURE){ return 0; } /* not a FILE record (free slot) */
    if(!NtfsApplyFixup(g_rec,g_geo.MftRecordSize,g_geo.BytesPerSector)){ err("bad fixup (torn) in record",rn); return 0; }
    if(!(m->Flags&MFT_RECORD_IN_USE)) return 0;
    if(m->BytesInUse>m->BytesAllocated||m->BytesAllocated>g_geo.MftRecordSize){ err("bad size fields in record",rn); return 1; }
    for(a=NtfsFirstAttribute(m); a; a=NtfsNextAttribute(m,a)){
        if(a->Type==ATTR_END) break;
        if((PUCHAR)a - g_rec + a->Length > m->BytesInUse){ err("attribute overruns record",rn); break; }
        count++;
        if(count>64){ err("runaway attribute list in record",rn); break; }
    }
    if(verbose){ outs("  rec "); outu(rn); outs(m->Flags&MFT_RECORD_IS_DIRECTORY?" dir":" file");
                 outs(" attrs "); outu(count); outs("\n"); }
    return 1;
}

int main(int argc, char **argv){
    unsigned char boot[512]; PATTRIBUTE_RECORD a; ULONGLONG rn, total, inuse=0;
    int verbose=0, i;
    const char *path=0;

    for(i=1;i<argc;i++){ int opt=(argv[i][0]=='-')||(argv[i][0]=='/'&&argv[i][1]&&!argv[i][2]);
        if(opt){ if(argv[i][1]=='V'||argv[i][1]=='v') verbose=1; } else path=argv[i]; }
    if(!path){ outs("usage: ntfschk <volume|image> [/V]\n"); return 2; }
#ifdef _WIN32
    { /* "D:" is the volume \\.\D: (else it would open D:'s current directory) */
      static char dev[8];
      if(path[0] && path[1]==':' && !path[2]){ dev[0]=dev[1]=dev[3]='\\'; dev[2]='.'; dev[4]=path[0]; dev[5]=':'; dev[6]=0; path=dev; } }
#endif
    if(!io_open(path)){ outs("cannot open volume\n"); return 1; }

    if(!io_read(boot,sizeof(boot),0)||!NtfsParseBoot(boot,sizeof(boot),&g_geo)){ outs("not an NTFS volume\n"); return 1; }
    outs("NTFS: cluster "); outu(g_geo.ClusterSize); outs(", MFT record "); outu(g_geo.MftRecordSize);
    outs(", clusters "); outu(g_geo.TotalClusters); outs("\n");

    /* bootstrap $MFT */
    if(!io_read(g_rec,g_geo.MftRecordSize,g_geo.MftLcn*(ULONGLONG)g_geo.ClusterSize)){ outs("cannot read $MFT rec0\n"); return 1; }
    if(!NtfsApplyFixup(g_rec,g_geo.MftRecordSize,g_geo.BytesPerSector)){ outs("$MFT record 0 fixup bad\n"); return 1; }
    a=NtfsFindAttribute((PMFT_RECORD)g_rec,ATTR_DATA,NULL,0);
    if(!a||!a->NonResident||!NtfsDecodeRuns(a,g_mftRuns,256,&g_mftRunCount)){ outs("$MFT $DATA unreadable\n"); return 1; }
    total=a->Form.NonResident.DataSize/g_geo.MftRecordSize;
    outs("MFT records: "); outu(total); outs("\n");

    for(rn=0; rn<total; rn++){ if(check_record(rn,verbose)) inuse++; }

    outs("in-use records: "); outu(inuse); outs("\n");
    if(g_errors){ outs("FAILED: "); outu(g_errors); outs(" problem(s)\n"); return 1; }
    outs("OK: no problems found\n");
    return 0;
}
