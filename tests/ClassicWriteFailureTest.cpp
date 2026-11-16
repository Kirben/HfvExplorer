// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include <cstdio>
#include <vector>
#include <stdexcept>
extern "C" {
#undef ERROR
#include "hfs/libhfs/internal.h"
#include "hfs/libhfs/block.h"
#define _FHOOK_INTERNAL_
#include "Filehook.h"
}
namespace {
ftype_write originalWrite;
unsigned calls,failAt;int mode;bool fired;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int failingWrite(int fd,const void* data,unsigned count) {
 unsigned at=calls++;
 if(!fired&&at==failAt) {
  fired=true;errno=EIO;
  if(mode==1)return originalWrite(fd,data,count/2);
  return -1;
 }
 return originalWrite(fd,data,count);
}
std::vector<BYTE> load(const CString& path){CFile f;require(f.Open(path,CFile::modeRead)!=FALSE,"open rollback image");std::vector<BYTE> b(size_t(f.GetLength()));require(f.Read(b.data(),UINT(b.size()))==b.size(),"read rollback image");return b;}
void save(const CString& path,const std::vector<BYTE>& b){CFile f;require(f.Open(path,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create rollback image");f.Write(b.data(),UINT(b.size()));}
int mutate(hfsvol* volume,unsigned operation) {
 char file[]="Rollback:Folder:File",renamed[]="Rollback:Folder:Renamed",folder[]="Rollback:Folder",moved[]="Rollback:Destination:Moved";
 if(operation==0)return hfs_delete(volume,file);
 if(operation==1)return hfs_rename(volume,file,renamed);
 if(operation==2)return hfs_rename(volume,folder,moved);
 char created[]="Rollback:Created",empty[]="Rollback:Empty",type[]="TEXT",creator[]="TEST";
 if(operation==3)return hfs_mkdir(volume,created);
 if(operation==4)return hfs_create(volume,created,type,creator);
 if(operation==5)return hfs_rmdir(volume,empty);
 hfsdirent properties;if(hfs_stat(volume,file,&properties)<0)return -1;
 properties.fdflags^=HFS_FNDR_ISINVISIBLE;return hfs_setattr(volume,file,&properties);
}
void verify(hfsvol* volume) {
 char name[]="Rollback:Folder:File";hfsfile* file=hfs_open(volume,name);require(file!=nullptr,"rollback lost file catalog record");
 std::vector<BYTE> bytes(4097);require(hfs_read(file,bytes.data(),4097)==4097&&bytes==std::vector<BYTE>(4097,0x31),"rollback changed data fork");
 bytes.resize(3001);require(hfs_setfork(file,1)==0&&hfs_read(file,bytes.data(),3001)==3001&&bytes==std::vector<BYTE>(3001,0x72),"rollback changed resource fork");require(hfs_close(file)==0,"close rollback verification");
}
}
void RunClassicWriteFailureTests(const char* directory,FILE* report) {
 CString root=directory,seedPath=root+"\\undo-seed.dsk",path=root+"\\undo-case.dsk";
 std::vector<BYTE> blank(4*1024*1024);save(seedPath,blank);
 char label[]="Rollback",folder[]="Rollback:Folder",destination[]="Rollback:Destination",name[]="Rollback:Folder:File",type[]="TEXT",creator[]="TEST";
 require(hfs_format(seedPath.GetBuffer(),0,label)==0,"format rollback seed");seedPath.ReleaseBuffer();
 hfsvol* volume=hfs_mount(seedPath.GetBuffer(),0,O_RDWR);seedPath.ReleaseBuffer();require(volume!=nullptr,"mount rollback seed");
 char empty[]="Rollback:Empty";
 require(hfs_mkdir(volume,folder)==0&&hfs_mkdir(volume,destination)==0&&hfs_mkdir(volume,empty)==0&&hfs_create(volume,name,type,creator)==0,"create rollback fixture tree");
 hfsfile* file=hfs_open(volume,name);std::vector<BYTE> data(4097,0x31),resource(3001,0x72);
 require(file&&hfs_write(file,data.data(),ULONG(data.size()))==data.size()&&hfs_setfork(file,1)==0&&hfs_write(file,resource.data(),ULONG(resource.size()))==resource.size()&&hfs_close(file)==0&&hfs_umount(volume)==0,"finish rollback fixture");
 auto seed=load(seedPath);originalWrite=fhook_write;
 for(int cache=0;cache<2;++cache)for(unsigned operation=0;operation<7;++operation) {
  save(path,seed);volume=hfs_mount(path.GetBuffer(),0,O_RDWR);path.ReleaseBuffer();require(volume!=nullptr,"mount measured operation");if(!cache)require(b_finish(volume)==0,"disable measured block cache");
  calls=0;failAt=~0u;fired=false;fhook_write=failingWrite;
  int result=mutate(volume,operation);if(!result)result=hfs_flush(volume);
  unsigned writes=calls;fhook_write=originalWrite;require(result==0&&writes>0&&hfs_umount(volume)==0,"measure successful metadata writes");
  for(int failure=0;failure<2;++failure)for(unsigned at=0;at<writes;++at) {
   save(path,seed);volume=hfs_mount(path.GetBuffer(),0,O_RDWR);path.ReleaseBuffer();require(volume!=nullptr,"mount injected metadata failure");if(!cache)require(b_finish(volume)==0,"disable injected block cache");
   calls=0;failAt=at;mode=failure;fired=false;fhook_write=failingWrite;
   result=mutate(volume,operation);if(!result)result=hfs_flush(volume);
   fhook_write=originalWrite;require(fired&&result<0,"metadata write failure was hidden");
   verify(volume);
   require(hfs_umount(volume)==0,"rollback left invalid pending metadata");
   require(load(path)==seed,"failed metadata operation left partial image writes");
   volume=hfs_mount(path.GetBuffer(),0,O_RDONLY);path.ReleaseBuffer();require(volume!=nullptr,"reopen rolled-back image");verify(volume);require(hfs_umount(volume)==0,"close recovered image");
  }
 }
 // A failed pre-operation flush must preserve older dirty open-file views.
 save(path,seed);volume=hfs_mount(path.GetBuffer(),0,O_RDWR);path.ReleaseBuffer();require(volume!=nullptr,"mount pending-write fixture");
 file=hfs_open(volume,name);require(file&&hfs_lseek(file,0,SEEK_END)>=0&&hfs_write(file,const_cast<char*>("APPEND"),6)==6,"prepare pending file update");
 auto pendingPosition=file->pos;calls=0;failAt=0;mode=1;fired=false;fhook_write=failingWrite;
 char unrelated[]="Rollback:Pending directory";int pendingResult=hfs_mkdir(volume,unrelated);fhook_write=originalWrite;
 require(pendingResult<0&&fired&&!(volume->flags&HFS_WRITE_FAILED)&&file->pos==pendingPosition,"failed pending flush poisoned/lost an open-file view");
 require(hfs_close(file)==0&&hfs_umount(volume)==0,"retry pending file metadata after rollback");
 volume=hfs_mount(path.GetBuffer(),0,O_RDONLY);path.ReleaseBuffer();require(volume!=nullptr,"reopen pending-update image");
 file=hfs_open(volume,name);char appended[6];require(file&&hfs_lseek(file,4097,SEEK_SET)==4097&&hfs_read(file,appended,6)==6&&!memcmp(appended,"APPEND",6),"rollback lost earlier pending data");
 require(hfs_close(file)==0&&hfs_umount(volume)==0,"close pending-update verification");
 fprintf(report,"PASS: rollback preserves earlier pending file data and open-file positions across pre-operation flush failures.\n");
 fprintf(report,"PASS: classic HFS file/folder creation, deletion, rename and attribute changes roll back every injected write error and short write, restoring exact image bytes and both forks.\n");
}
#endif
