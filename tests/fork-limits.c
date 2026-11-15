// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <errno.h>
#include <sys/stat.h>
#undef ERROR
#include "hfs/libhfs/internal.h"
#include "hfs/libhfs/file.h"
#include "hfs/copyin.h"
#include "hfs/binhex.h"

static void must(int ok,const char* action) {if(!ok){fprintf(stderr,"FAIL: fork limits: %s (%s; %s)\n",action,hfs_error?hfs_error:"",cpi_error);exit(1);}}
int main(int argc,char** argv) {
 char image[1024],hex[1024],target[64],label[]="Limits",name[]="Limits:Empty",type[]="TEXT",creator[]="ttxt";
 HANDLE host;LARGE_INTEGER length;hfsvol* volume;hfsfile* file;unsigned fork;unsigned char byte=0;
 must(argc==2 && strlen(argv[1])<900,"new test directory argument");must(CreateDirectoryA(argv[1],NULL),"exclusive directory");
 sprintf(image,"%s\\limits.dsk",argv[1]);host=CreateFileA(image,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);must(host!=INVALID_HANDLE_VALUE,"new image");
 length.QuadPart=16*1024*1024;must(SetFilePointerEx(host,length,NULL,FILE_BEGIN)&&SetEndOfFile(host),"size limits fixture");CloseHandle(host);
 must(hfs_format(image,0,label)==0,"format limits fixture");volume=hfs_mount(image,0,O_RDWR);must(volume!=NULL,"mount limits fixture");
 must(hfs_create(volume,name,type,creator)==0,"create empty fork fixture");file=hfs_open(volume,name);must(file!=NULL,"open fork fixture");
 {
  hfsfile fake=*file;unsigned short before=volume->mdb.drFreeBks;
  fake.cat.u.fil.filPyLen=(0xffffffffUL/volume->mdb.drAlBlkSiz)*volume->mdb.drAlBlkSiz;
  must(f_alloc(&fake)==-1 && errno==EFBIG && volume->mdb.drFreeBks==before,"physical allocation rollover guard");
  fake.cat.u.fil.filLgLen=0xffffffffUL;
  must(f_trunc(&fake)==-1 && errno==EFBIG && volume->mdb.drFreeBks==before,"rounded physical EOF rollover guard");
  fake.cat.u.fil.filLgLen=0;fake.pos=1;
  must(hfs_read(&fake,&byte,1)==0,"read position above EOF does not unsigned-wrap remaining length");
  fake.pos=0;must(hfs_write(&fake,&byte,0x80000000UL)==-1 && errno==EINVAL,"oversized single transfer rejected before buffer access");
 }
 must(hfs_close(file)==0,"close limit fixture");
 // Real BinHex headers with unsigned large lengths, deliberately missing their
 // payload: these must reach payload validation, not reject the high size bit.
 for(fork=0;fork<2;++fork) {
  unsigned char header[24]={};int fd;
  sprintf(hex,"%s\\truncated-%u.hqx",argv[1],fork);sprintf(target,"Limits:Truncated%u",fork);
  header[0]=1;header[1]='X';memcpy(header+3,"TEXTttxt",8);header[13+4*fork]=0x80;
  fd=_open(hex,_O_CREAT|_O_EXCL|_O_WRONLY|_O_BINARY,_S_IREAD|_S_IWRITE);must(fd>=0,"new BinHex fixture");
  must(bh_start(fd)==0 && bh_insert(header,21)==0 && bh_insertcrc()==0,"encode large unsigned BinHex header");
  if(fork) must(bh_insertcrc()==0,"encode empty data fork CRC");
  must(bh_end()==0 && _close(fd)==0,"close BinHex fixture");
  must(cpi_binh(hex,volume,target)==-1 && strstr(cpi_error,"bad file length")==NULL,"large BinHex length accepted, truncated payload rejected");
 }
 must(hfs_umount(volume)==0,"close limits volume");
 puts("PASS: fork limits: 4 GiB allocation/rounding guards, unsigned EOF arithmetic, signed per-call count guard, unsigned BinHex headers with truncated payload rejection");return 0;
}
