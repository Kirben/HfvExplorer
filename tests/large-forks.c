// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#undef ERROR
#include "hfs/ClassicHfsEngine.h"
#include "hfs/ClassicHfsForkState.h"
#include "hfs/copyin.h"
#include "hfs/copyout.h"
#include "LargeForkTestData.h"

// Transfers link the production translation modules. These services belong to
// the GUI and are unused by the raw/MacBinary paths exercised in this target.
HFV_THREAD_LOCAL int crlf_translation=0;
void mac_to_pc_charset(unsigned char* s) {(void)s;}
void pc_to_mac_charset(unsigned char* s) {(void)s;}
int get_apple_double_type_creator(char* n,char* t,char* c,unsigned long* o,unsigned long* l) {
 (void)n;(void)t;(void)c;(void)o;(void)l;return 0;
}

static void must(int ok,const char* operation) {
 if(!ok) {fprintf(stderr,"FAIL: large forks: %s (HFS=%s; import=%s; export=%s)\n",operation,hfs_error?hfs_error:"",cpi_error,cpo_error);exit(1);}
}
static void payload(unsigned char* bytes,unsigned length,uint64_t offset,unsigned seed) {
 unsigned i;for(i=0;i<length;++i) bytes[i]=(unsigned char)((offset+i)*37+seed);
}
static void verifyFork(hfsfile* file,unsigned fork,unsigned long length,unsigned char* bytes,unsigned capacity) {
 uint64_t offset=0;unsigned char* expected=malloc(capacity);must(expected!=NULL,"verification buffer");
 must(hfs_setfork(file,fork)==0,"select persisted large fork");
 while(offset<length) {
  unsigned n=length-offset<capacity?(unsigned)(length-offset):capacity;
  must(hfs_read(file,bytes,n)==n,"read full persisted fork past 2 GiB");payload(expected,n,offset,17+fork*101);
  must(!memcmp(bytes,expected,n),"full large-fork byte fidelity");offset+=n;
  if(offset%(512ULL*1024*1024)==0) {printf("Verified fork %u: %llu MiB\n",fork,offset/(1024*1024));fflush(stdout);}
 }
 { unsigned char guard=0x5a;must(hfs_read(file,&guard,1)==0 && guard==0x5a,"large fork EOF"); }
 must(hfs_lseek(file,INT64_MAX,SEEK_CUR)==length,"overflow-safe relative seek clamp");
 must(hfs_lseek(file,INT64_MIN,SEEK_END)==0,"negative extreme seek clamp");
 must(hfs_lseek(file,-19,SEEK_END)==length-19,"negative seek from large fork EOF");
 must(hfs_read(file,bytes,100)==19,"read clips at large EOF");payload(expected,19,length-19,17+fork*101);
 must(!memcmp(bytes,expected,19),"EOF tail contents");
 free(expected);
}
void CreateLargeForkFixture(const char* directory) {
 const unsigned capacity=4*1024*1024;const unsigned long length=0x7fffffffUL;
 LargeForkManifest m={0};char source[1100],binary[1100],meta[1100],name[]="LargeForks:BigForks",label[]="LargeForks";
 unsigned char* bytes=malloc(capacity);uint64_t offset;HANDLE host;LARGE_INTEGER size;DWORD ignored,wrote;
 hfsvol* volume;hfsfile* file;hfsdirent entry;unsigned fork,i;
 must(bytes!=NULL,"allocate streaming buffer");
 sprintf(m.image,"%s\\large-forks.dsk",directory);sprintf(source,"%s\\large-fork.raw",directory);
 sprintf(binary,"%s\\large-forks.bin",directory);sprintf(meta,"%s\\large-forks.meta",directory);m.length=length;
 host=CreateFileA(m.image,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);must(host!=INVALID_HANDLE_VALUE,"create large-fork image");
 must(DeviceIoControl(host,FSCTL_SET_SPARSE,NULL,0,NULL,0,&ignored,NULL),"sparse large-fork image");
 size.QuadPart=6ULL*1024*1024*1024;must(SetFilePointerEx(host,size,NULL,FILE_BEGIN)&&SetEndOfFile(host),"size large-fork image");CloseHandle(host);
 must(hfs_format(m.image,0,label)==0,"format large-fork volume");
 host=CreateFileA(source,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);must(host!=INVALID_HANDLE_VALUE,"create full raw source");
 for(offset=0;offset<length;) {
  unsigned n=length-offset<capacity?(unsigned)(length-offset):capacity;payload(bytes,n,offset,17);
  must(WriteFile(host,bytes,n,&wrote,NULL)&&wrote==n,"write full at Apple maximum raw source");offset+=n;
 }
 CloseHandle(host);printf("Importing %lu-byte raw data fork...\n",length);fflush(stdout);
 volume=hfs_mount(m.image,0,O_RDWR);must(volume!=NULL,"mount fork test volume");
 must(cpi_raw(source,volume,name)==0,"raw import must not silently stop at LONG_MAX");
 must(hfs_stat(volume,name,&entry)==0 && entry.u.file.dsize==length,"raw-imported fork length equals Apple maximum");
 file=hfs_open(volume,name);must(file!=NULL,"open large data fork");must(hfs_setfork(file,1)==0,"select resource fork");
 for(offset=0;offset<length;) {
  unsigned n=length-offset<capacity?(unsigned)(length-offset):capacity;payload(bytes,n,offset,118);
  must(hfs_write(file,bytes,n)==n,"stream complete at Apple maximum resource fork");offset+=n;
  if(offset%(512ULL*1024*1024)==0) {printf("Wrote resource fork: %llu MiB\n",offset/(1024*1024));fflush(stdout);}
 }
 // Overwrites spanning the signed boundary must not alias to low fork offsets.
 for(fork=0;fork<2;++fork) {
  must(hfs_setfork(file,fork)==0 && hfs_lseek(file,length-128,SEEK_SET)==length-128,"seek to signed boundary");
  payload(bytes,128,length-128,17+fork*101);must(hfs_write(file,bytes,128)==128,"overwrite at maximum EOF");
 }
 // Reject oversized single-call write counts before touching a tiny buffer.
 must(hfs_setfork(file,0)==0,"select limit-test fork");
 { unsigned long before=file->pos;must(hfs_write(file,bytes,0x80000000UL)==-1 && errno==EINVAL && file->pos==before,"signed per-call transfer result guard"); }
 // Test the physical-EOF rollover without allocating another four GiB.
 { hfsfile fake=*file;unsigned short freeBefore=volume->mdb.drFreeBks;
   fake.cat.u.fil.filPyLen=(0xffffffffUL/volume->mdb.drAlBlkSiz)*volume->mdb.drAlBlkSiz;
   must(ClassicHfsExtendForkAllocation(&fake)==-1 && errno==EFBIG && volume->mdb.drFreeBks==freeBefore,"4 GiB physical allocation rollover rejected"); }
 must(hfs_close(file)==0 && hfs_umount(volume)==0,"persist both at Apple maximum forks");
 volume=hfs_mount(m.image,0,O_RDONLY);must(volume!=NULL,"reopen both large forks");file=hfs_open(volume,name);must(file!=NULL,"reopen large file");
 for(fork=0;fork<2;++fork) verifyFork(file,fork,length,bytes,capacity);
 must(hfs_close(file)==0,"close full verification");
 printf("Exporting both large forks as MacBinary II...\n");fflush(stdout);
 must(cpo_macb(volume,name,binary)==0,"export at Apple maximum fork sizes as MacBinary II");must(hfs_umount(volume)==0,"close export volume");
 // Free the original before reimport so the round trip fits the 6 GiB image.
 volume=hfs_mount(m.image,0,O_RDWR);must(volume!=NULL,"mount round-trip destination");must(hfs_delete(volume,name)==0,"remove disposable original before reimport");
 printf("Reimporting MacBinary II with both length fields at LONG_MAX...\n");fflush(stdout);
 must(cpi_macb(binary,volume,name)==0,"MacBinary II unsigned fork length import");must(hfs_umount(volume)==0,"persist MacBinary round trip");
 volume=hfs_mount(m.image,0,O_RDWR);must(volume!=NULL,"reopen MacBinary round trip");file=hfs_open(volume,name);must(file!=NULL,"open round-trip file");
 must(hfs_fstat(file,&entry)==0 && entry.u.file.dsize==length && entry.u.file.rsize==length,"both lengths survive full transfer");
 for(fork=0;fork<2;++fork) verifyFork(file,fork,length,bytes,capacity);
 // Truncate above the signed boundary and append back to the original size.
 for(fork=0;fork<2;++fork) {
  must(hfs_setfork(file,fork)==0 && hfs_truncate(file,length-37)==0,"truncate at Apple maximum fork");
  must(hfs_lseek(file,0,SEEK_END)==length-37,"large truncated EOF");payload(bytes,37,length-37,17+fork*101);
  must(hfs_write(file,bytes,37)==37,"append to at Apple maximum fork");
 }
 must(hfs_close(file)==0 && hfs_umount(volume)==0,"persist truncate/append");
 volume=hfs_mount(m.image,0,O_RDONLY);must(volume!=NULL,"reopen truncate/append");file=hfs_open(volume,name);must(file!=NULL,"open final fixture");
 m.fileID=file->cat.u.fil.filFlNum;
 for(i=0;i<3;++i) {
  m.data[i].start=file->cat.u.fil.filExtRec[i].xdrStABN;m.data[i].count=file->cat.u.fil.filExtRec[i].xdrNumABlks;
  m.resource[i].start=file->cat.u.fil.filRExtRec[i].xdrStABN;m.resource[i].count=file->cat.u.fil.filRExtRec[i].xdrNumABlks;
 }
 must(hfs_close(file)==0 && hfs_umount(volume)==0,"close final large-fork fixture");free(bytes);
 { FILE* out=fopen(meta,"wb");must(out!=NULL && fwrite(&m,sizeof(m),1,out)==1,"write GUI large-fork manifest");fclose(out); }
 puts("PASS: individual forks at Apple maximum: full raw import, resource write, full readback, boundary overwrite, MacBinary export/import, reopen, truncate/append, count and allocation limit guards");fflush(stdout);
}
