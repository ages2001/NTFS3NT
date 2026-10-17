/*
 * NTFS3NT - user-mode test for NTFS transparent compression (LZNT1).
 * Reads a COMPRESSED file's $DATA from a real NTFS image through the shared
 * engine's run-list + NtfsLznt1Unit decompressor and writes the plaintext to
 * /tmp/lz_out.bin for comparison with a reference (e.g. `ntfscat`).
 *
 * Build: cc -I../src ntfscomp.c ../src/ntfssup.c -o ntfscomp
 * Run:   ./ntfscomp <image> <inode-of-compressed-file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "ntfsusr.h"
#include "ntfssup.h"
static int fd; static NTFS_GEOMETRY g; static NTFS_RUN mr[256]; static ULONG mrc;
static void ra(void*b,ULONG l,ULONGLONG o){ if(pread(fd,b,l,o)!=(ssize_t)l){perror("pread");exit(1);} }
static int rdruns(PNTFS_RUN runs,ULONG rc,ULONGLONG vbo,void*buf,ULONG len){
  PUCHAR out=buf; ULONG cs=g.ClusterSize;
  while(len){ ULONGLONG vcn=vbo/cs; ULONG inclu=vbo%cs; ULONGLONG lcn,left;BOOLEAN sp; ULONG ch;
    if(!NtfsMapVcn(runs,rc,vcn,&lcn,&left,&sp)){ if(sp){ch=cs-inclu;if(ch>len)ch=len;memset(out,0,ch);out+=ch;vbo+=ch;len-=ch;continue;} return 0;}
    ch=cs-inclu; if(ch>len)ch=len; ra(out,ch,lcn*(ULONGLONG)cs+inclu); out+=ch;vbo+=ch;len-=ch;}
  return 1;}
int main(int c,char**v){ unsigned char boot[512],*rec; PATTRIBUTE_RECORD a; PMFT_RECORD m;
 fd=open(v[1],O_RDONLY); ra(boot,512,0); NtfsParseBoot(boot,512,&g);
 { unsigned char r0[4096]; ra(r0,g.MftRecordSize,g.MftLcn*(ULONGLONG)g.ClusterSize);
   NtfsApplyFixup(r0,g.MftRecordSize,g.BytesPerSector); m=(PMFT_RECORD)r0;
   a=NtfsFindAttribute(m,ATTR_DATA,NULL,0); NtfsDecodeRuns(a,mr,256,&mrc); }
 rec=malloc(g.MftRecordSize);
 ULONGLONG rn = (c>2)?strtoull(v[2],0,0):66ULL; if(!rdruns(mr,mrc,rn*g.MftRecordSize,rec,g.MftRecordSize)){printf("rec read fail\n");return 1;}
 NtfsApplyFixup(rec,g.MftRecordSize,g.BytesPerSector); m=(PMFT_RECORD)rec;
 a=NtfsFindAttribute(m,ATTR_DATA,NULL,0);
 ULONG cu_cl = 1u << ((PUCHAR)a)[0x22];     /* clusters per unit */
 ULONG cu_sz = cu_cl*g.ClusterSize;
 ULONGLONG dsize=a->Form.NonResident.DataSize;
 NTFS_RUN runs[256]; ULONG rc; NtfsDecodeRuns(a,runs,256,&rc);
 printf("compressed file: dsize=%llu cu_clusters=%u cu_size=%u runs=%u\n",(unsigned long long)dsize,cu_cl,cu_sz,rc);
 unsigned char *out=malloc(((dsize+cu_sz-1)/cu_sz)*cu_sz);
 unsigned char *comp=malloc(cu_sz); unsigned char *unit=malloc(cu_sz);
 ULONGLONG off=0; ULONG total=0;
 while(off<dsize){
   ULONGLONG uvcn=(off/cu_sz)*cu_cl; ULONG mapped=0; ULONG i;
   for(i=0;i<cu_cl;i++){ULONGLONG lcn,left;BOOLEAN sp; if(NtfsMapVcn(runs,rc,uvcn+i,&lcn,&left,&sp))mapped++; }
   if(mapped==cu_cl){ rdruns(runs,rc,(off/cu_sz)*cu_sz,unit,cu_sz); }         /* uncompressed unit */
   else if(mapped==0){ memset(unit,0,cu_sz); }                                /* sparse */
   else { memset(comp,0,cu_sz); rdruns(runs,rc,(off/cu_sz)*cu_sz,comp,mapped*g.ClusterSize);
          NtfsLznt1Unit(comp,mapped*g.ClusterSize,unit,cu_sz); }              /* compressed */
   memcpy(out+total,unit,cu_sz); total+=cu_sz; off+=cu_sz;
 }
 FILE*fo=fopen("/tmp/lz_out.bin","wb"); fwrite(out,1,(size_t)dsize,fo); fclose(fo);
 printf("wrote %llu bytes to /tmp/lz_out.bin\n",(unsigned long long)dsize);
 return 0;}
