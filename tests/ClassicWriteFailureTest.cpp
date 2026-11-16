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
ftype_sync originalSync;
unsigned syncCalls, syncFailures, syncFailAt=1;
int failingSync(int fd) {
 ++syncCalls;
 if(syncFailures&&syncCalls>=syncFailAt){--syncFailures;errno=EIO;return -1;}
 return originalSync(fd);
}
ftype_read originalRead;
int failingRead(int,void*,unsigned){errno=EIO;return -1;}

unsigned calls,failAt;int mode;bool fired;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int failingWrite(int fd,const void* data,unsigned count) {
 unsigned at=calls++;
 if(mode==2&&fired){errno=EIO;return -1;}
 if(!fired&&at==failAt) {
  fired=true;errno=EIO;
  if(mode==1||mode==2)return originalWrite(fd,data,count/2);
  return -1;
 }
 return originalWrite(fd,data,count);
}
std::vector<BYTE> load(const CString& path){CFile f;require(f.Open(path,CFile::modeRead|CFile::shareDenyNone)!=FALSE,"open rollback image");std::vector<BYTE> b(size_t(f.GetLength()));require(f.Read(b.data(),UINT(b.size()))==b.size(),"read rollback image");return b;}
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
 if(operation==6){properties.fdflags^=HFS_FNDR_ISINVISIBLE;return hfs_setattr(volume,file,&properties);}
 hfsfile* opened=hfs_open(volume,file);if(!opened)return -1;
 int result=hfs_setfork(opened,operation==8?1:0);
 if(!result)result=hfs_truncate(opened,1);
 int closed=hfs_close(opened);return result?result:closed;
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
 char blockerName[]="Rollback:Blocker";require(hfs_create(volume,blockerName,type,creator)==0,"create fragmentation blocker");
 hfsfile* file=hfs_open(volume,name);hfsfile* blocker=hfs_open(volume,blockerName);
 require(file&&blocker,"open fragmented fork fixture");file->clump=blocker->clump=volume->mdb.drAlBlkSiz;
 for(unsigned fork=0;fork<2;++fork) {
  require(hfs_setfork(file,fork)==0,"select fragmented fork");
  unsigned length=fork?3001:4097;
  for(unsigned offset=0;offset<length;) {
   unsigned count=(std::min)(512u,length-offset);std::vector<BYTE> part(count,fork?0x72:0x31),block(512,0x55);
   require(hfs_write(file,part.data(),count)==count&&hfs_write(blocker,block.data(),512)==512,"write fragmented fork extent");offset+=count;
  }
 }
 require(hfs_close(file)==0&&hfs_close(blocker)==0&&hfs_umount(volume)==0,"finish fragmented rollback fixture");
 auto seed=load(seedPath);originalWrite=fhook_write;
 for(int cache=0;cache<2;++cache)for(unsigned operation=0;operation<9;++operation) {
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
 // Commit flush failures must restore sectors before discarding undo state.
 originalSync=fhook_sync;
 for(int cache=0;cache<2;++cache) {
  save(path,seed);volume=hfs_mount(path.GetBuffer(),0,O_RDWR);path.ReleaseBuffer();require(volume!=nullptr,"mount commit-flush fixture");
  if(!cache)require(b_finish(volume)==0,"disable commit-flush cache");
  syncCalls=0;syncFailures=1;fhook_sync=failingSync;
  int synced=hfs_mkdir(volume,unrelated);fhook_sync=originalSync;
  require(synced<0&&syncCalls==2&&!(volume->flags&HFS_WRITE_FAILED),"commit flush failure did not roll back and flush restoration");
  verify(volume);require(hfs_umount(volume)==0&&load(path)==seed,"commit flush failure left changed image");
 }
 fprintf(report,"PASS: commit flush failures restore and flush original sectors in both cache modes.\n");
 // Final clean-unmount marker must also roll back a failed storage flush.
 for(int cache=0;cache<2;++cache) {
  save(path,seed);volume=hfs_mount(path.GetBuffer(),0,O_RDWR);path.ReleaseBuffer();require(volume!=nullptr,"mount clean-marker fixture");
  if(!cache)require(b_finish(volume)==0,"disable clean-marker cache");
  require(hfs_mkdir(volume,unrelated)==0,"dirty clean-marker fixture");
  auto dirty=load(path);syncCalls=0;syncFailures=1;syncFailAt=1;fhook_sync=failingSync;
  int unmounted=hfs_umount(volume);fhook_sync=originalSync;
  require(unmounted<0&&syncCalls==2&&load(path)==dirty,"failed clean-marker flush was published or retried without undo");
  volume=hfs_mount(path.GetBuffer(),0,O_RDONLY);path.ReleaseBuffer();require(volume!=nullptr,"reopen marker rollback");
  verify(volume);require(hfs_umount(volume)==0,"close marker verification");
 }
 fprintf(report,"PASS: final clean-unmount marker flush failures restore exact pre-unmount image bytes.\n");
 // Failed close preserves its pending file view for explicit retry or unmount.
 for(int cache=0;cache<2;++cache)for(int retry=0;retry<2;++retry) {
  save(path,seed);volume=hfs_mount(path.GetBuffer(),0,O_RDWR);path.ReleaseBuffer();require(volume!=nullptr,"mount close-retry fixture");
  if(!cache)require(b_finish(volume)==0,"disable close-retry cache");
  file=hfs_open(volume,name);require(file&&hfs_lseek(file,0,SEEK_END)>=0&&hfs_write(file,const_cast<char*>("APPEND"),6)==6,"prepare close-retry update");
  auto pos=file->pos;calls=0;failAt=0;mode=retry;fired=false;fhook_write=failingWrite;
  int closed=hfs_close(file);fhook_write=originalWrite;
  require(closed<0&&fired&&volume->files==file&&file->pos==pos&&!(volume->flags&HFS_WRITE_FAILED),"failed close discarded pending file view");
  if(retry)require(hfs_close(file)==0,"explicit close retry");
  require(hfs_umount(volume)==0,"unmount retry of pending close");
  volume=hfs_mount(path.GetBuffer(),0,O_RDONLY);path.ReleaseBuffer();require(volume!=nullptr,"reopen close-retry image");
  file=hfs_open(volume,name);require(file&&hfs_lseek(file,4097,SEEK_SET)==4097&&hfs_read(file,appended,6)==6&&!memcmp(appended,"APPEND",6),"failed close lost pending append");
  require(hfs_close(file)==0&&hfs_umount(volume)==0,"close retry verification");
 }
 fprintf(report,"PASS: failed/short close writes retain file views; explicit close and unmount retries preserve pending data.\n");
 // Persistent preimage read errors must not loop forever while closing.
 save(path,seed);volume=hfs_mount(path.GetBuffer(),0,O_RDWR);path.ReleaseBuffer();require(volume!=nullptr,"mount persistent-read fixture");
 require(b_finish(volume)==0,"disable persistent-read cache");file=hfs_open(volume,name);
 require(file&&hfs_lseek(file,0,SEEK_END)>=0&&hfs_write(file,const_cast<char*>("APPEND"),6)==6,"prepare persistent-read update");
 originalRead=fhook_read;fhook_read=failingRead;calls=0;failAt=~0u;mode=0;fired=false;fhook_write=failingWrite;
 require(hfs_umount(volume)<0&&calls==0,"persistent close read failure hung or wrote without undo");
 fhook_read=originalRead;fhook_write=originalWrite;
 fprintf(report,"PASS: persistent before-image read failures stop unmount after bounded retries without device writes.\n");
 // A device that also rejects undo writes must be fenced, not retried as clean.
 save(path,seed);volume=hfs_mount(path.GetBuffer(),0,O_RDWR);path.ReleaseBuffer();require(volume!=nullptr,"mount unrecoverable-write fixture");
 hfsfile* held=hfs_open(volume,blockerName);require(held!=nullptr,"hold unrelated file during rollback failure");
 calls=0;failAt=0;mode=2;fired=false;fhook_write=failingWrite;
 int permanent=mutate(volume,0);require(permanent<0&&(volume->flags&HFS_WRITE_FAILED)&&(volume->flags&HFS_READONLY),"failed rollback left volume writable");
 std::string message=hfs_error?hfs_error:"";size_t first=message.find('"'),last=message.find('"',first+1);
 require(first!=std::string::npos&&last!=std::string::npos,"failed rollback did not retain original sector evidence");
 CString undoPath(message.substr(first+1,last-first-1).c_str());auto undo=load(undoPath);
 auto little32=[](const BYTE* p){return DWORD(p[0])|(DWORD(p[1])<<8)|(DWORD(p[2])<<16)|(DWORD(p[3])<<24);};
 require(undo.size()>=512&&!memcmp(undo.data(),"HFSUNDO1",8),"undo evidence header");unsigned records=little32(undo.data()+32);
 require(records&&undo.size()==512+records*528ULL,"undo evidence truncated");
 for(unsigned i=0;i<records;++i){auto record=undo.data()+512+i*528;ULONGLONG offset=little32(record)|(ULONGLONG(little32(record+4))<<32);require(offset<=seed.size()-512&&little32(record+12)==512&&!memcmp(record+16,seed.data()+size_t(offset),512),"undo evidence lost original image sectors");}
 auto writesAfterFailure=calls;
 require(hfs_write(held,const_cast<char*>("NO"),2)<0&&hfs_flush(volume)<0&&hfs_mkdir(volume,unrelated)<0,"poisoned volume accepted further writes");
 hfs_close(held);require(hfs_umount(volume)<0&&calls==writesAfterFailure,"poisoned cleanup attempted more device writes or reported clean close");
 fhook_write=originalWrite;
 require(GetFileAttributesA(undoPath)!=INVALID_FILE_ATTRIBUTES&&hfs_error&&strstr(hfs_error,"Undo records saved"),"close discarded rollback recovery details");
 fprintf(report,"PASS: rollback-write failures disable all further writes, preserve exact sector undo evidence and report an unclean close. Undo fixture: %s\n",(LPCSTR)undoPath);
 fprintf(report,"PASS: classic HFS file/folder creation, deletion, rename, attributes and fragmented-fork truncation roll back every injected write error and short write, restoring exact image bytes and both forks.\n");
}
#endif
