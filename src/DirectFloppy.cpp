// SPDX-License-Identifier: GPL-2.0-or-later
#include "DirectFloppy.h"
#include <string.h>
namespace {
const DWORD kSize=1474560,kBuffer=65536,kSector=512;
thread_local DirectFloppy* current=nullptr;
class NativeDirectApi : public FloppyWriteApi {
 bool ctl(HANDLE h,DWORD code){DWORD n=0;return DeviceIoControl(h,code,nullptr,0,nullptr,0,&n,nullptr)!=FALSE;}
public:
 UINT DriveType(int d) override{return NativeFloppyReadApi().DriveType(d);}
 HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f) override{return NativeFloppyReadApi().Open(p,a,s,f);}
 bool Geometry(HANDLE h,DISK_GEOMETRY& g) override{return NativeFloppyReadApi().Geometry(h,g);}
 bool Verify(HANDLE h,DWORD& n) override{return NativeFloppyReadApi().Verify(h,n);}
 bool Read(HANDLE h,ULONGLONG o,DWORD n,void* p,DWORD& got) override{return NativeFloppyReadApi().Read(h,o,n,p,got);}
 void Close(HANDLE h) override{CloseHandle(h);}
 bool Lock(HANDLE h) override{return ctl(h,FSCTL_LOCK_VOLUME);}
 bool Dismount(HANDLE h) override{return ctl(h,FSCTL_DISMOUNT_VOLUME);}
 bool Unlock(HANDLE h) override{return ctl(h,FSCTL_UNLOCK_VOLUME);}
 bool FormatTracks1440(HANDLE) override{SetLastError(ERROR_NOT_SUPPORTED);return false;}
 bool Write(HANDLE h,ULONGLONG o,DWORD n,const void* p,DWORD& got) override{LARGE_INTEGER at;at.QuadPart=o;got=0;return SetFilePointerEx(h,at,nullptr,FILE_BEGIN)&&WriteFile(h,p,n,&got,nullptr);}
 bool Flush(HANDLE h) override{return FlushFileBuffers(h)!=FALSE;}
} native;
}
DirectFloppy::DirectFloppy(FloppyWriteApi* api):api_(api?api:&native){cache_init(&cache_);}
DirectFloppy::~DirectFloppy(){
 if(current==this)current=nullptr;
 if(handle_!=INVALID_HANDLE_VALUE){if(locked_)api_->Unlock(handle_);api_->Close(handle_);}
 if(scratch_)VirtualFree(scratch_,0,MEM_RELEASE);if(pending_)VirtualFree(pending_,0,MEM_RELEASE);cache_final(&cache_);
}
DirectFloppy* DirectFloppy::Current(int drive){return current&&current->drive_==drive?current:nullptr;}
bool DirectFloppy::fail(DWORD error){
 if(!error_)error_=error?error:ERROR_IO_DEVICE;cache_clear(&cache_);
 if(current!=this){
  if(handle_!=INVALID_HANDLE_VALUE){if(locked_)api_->Unlock(handle_);api_->Close(handle_);handle_=INVALID_HANDLE_VALUE;locked_=false;}
  lease_.Release();
 }
 SetLastError(error_);return false;
}
bool DirectFloppy::verify(){DWORD n=0;if(error_)return fail(error_);if(!api_->Verify(handle_,n))return fail(GetLastError());return n==generation_?true:fail(ERROR_MEDIA_CHANGED);}
bool DirectFloppy::Open(int drive,bool verifyWrites){
 verifyWrites_=verifyWrites;if(verifyWrites_){expected_.assign(kSize,0);dirty_.assign(kSize/kSector,0);}
#ifdef HFV_FLOPPY_TESTING
 if(api_==&native)return fail(ERROR_NOT_SUPPORTED);
#endif
 if(current||drive<0||drive>25)return fail(ERROR_INVALID_PARAMETER);
 if(!lease_.Acquire(drive))return fail(GetLastError());
 ReadOnlyFloppy probe(api_);if(!probe.OpenDrive(drive)||!probe.FindHfs())return fail(probe.Error());
 if(probe.MediaSize()!=kSize)return fail(ERROR_NOT_SUPPORTED);
 if(!api_->Verify(probe.Handle(),generation_))return fail(GetLastError());probe.Close();
 char path[]="\\\\.\\A:";path[4]+=char(drive);
 handle_=api_->Open(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH);
 if(handle_==INVALID_HANDLE_VALUE)return fail(GetLastError());
 if(!api_->Lock(handle_))return fail(GetLastError());locked_=true;
 if(!verify()||!api_->Dismount(handle_)||!verify())return fail(GetLastError());
 DISK_GEOMETRY g={};if(!api_->Geometry(handle_,g))return fail(GetLastError());
 if(g.MediaType!=F3_1Pt44_512||g.Cylinders.QuadPart!=80||g.TracksPerCylinder!=2||g.SectorsPerTrack!=18||g.BytesPerSector!=512)return fail(ERROR_NOT_SUPPORTED);
 scratch_=(BYTE*)VirtualAlloc(nullptr,kBuffer,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);pending_=(BYTE*)VirtualAlloc(nullptr,kBuffer,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
 if(!scratch_||!pending_)return fail(ERROR_NOT_ENOUGH_MEMORY);
 drive_=drive;current=this;return true;
}
bool DirectFloppy::flushWrites(){
 if(!pendingCount_)return !error_;
 if(!verify())return false;
 DWORD got=0;writes_=true;
 if(!api_->Write(handle_,pendingOffset_,pendingCount_,pending_,got))return fail(GetLastError());
 if(got!=pendingCount_)return fail(ERROR_WRITE_FAULT);
 pendingCount_=0;return verify();
}

bool DirectFloppy::Read(ULONGLONG offset,DWORD count,void* buffer){
 if(current!=this||handle_==INVALID_HANDLE_VALUE)return fail(ERROR_INVALID_HANDLE);
 if(error_)return fail(error_);if(offset>kSize||count>kSize-offset||(!buffer&&count))return fail(ERROR_INVALID_PARAMETER);
 if(!count)return true;if(!verify())return false;
 BYTE* out=(BYTE*)buffer;BYTE sector[kSector];
 while(count){
  DWORD block=DWORD(offset/kSector),skip=DWORD(offset%kSector);
  if(cache_get(&cache_,block,(char*)sector)){DWORD n=min(count,kSector-skip);memcpy(out,sector+skip,n);out+=n;offset+=n;count-=n;continue;}
  if(!flushWrites())return false;
  DWORD n=min(count,kBuffer-skip),transfer=(skip+n+511)&~511u,got=0;
  if(!api_->Read(handle_,offset-skip,transfer,scratch_,got))return fail(GetLastError());
  if(got!=transfer)return fail(ERROR_READ_FAULT);if(!verify())return false;
  for(DWORD i=0;i<transfer/kSector;++i)cache_put(&cache_,block+i,(char*)scratch_+i*kSector,kSector);
  memcpy(out,scratch_+skip,n);out+=n;offset+=n;count-=n;
 }
 return true;
}
bool DirectFloppy::Write(ULONGLONG offset,DWORD count,const void* buffer){
 if(current!=this||handle_==INVALID_HANDLE_VALUE)return fail(ERROR_INVALID_HANDLE);
 if(error_)return fail(error_);
 if(offset>kSize||count>kSize-offset||offset%kSector||count%kSector||(!buffer&&count))return fail(ERROR_INVALID_PARAMETER);
 auto source=(const BYTE*)buffer;
 while(count){
  if(pendingCount_&&(pendingOffset_+pendingCount_!=offset||pendingCount_==kBuffer))if(!flushWrites())return false;
  if(!pendingCount_)pendingOffset_=DWORD(offset);
  DWORD n=min(count,kBuffer-pendingCount_);memcpy(pending_+pendingCount_,source,n);pendingCount_+=n;
  if(verifyWrites_){memcpy(expected_.data()+size_t(offset),source,n);memset(dirty_.data()+size_t(offset/kSector),1,n/kSector);}
  for(DWORD i=0;i<n/kSector;++i)cache_put(&cache_,int(offset/kSector+i),(char*)source+i*kSector,kSector);
  source+=n;offset+=n;count-=n;
 }
 return true;
}
bool DirectFloppy::Finish(){
 if(current!=this||handle_==INVALID_HANDLE_VALUE)return fail(ERROR_INVALID_HANDLE);
 if(!flushWrites()||!verify())return false;
 if(!api_->Flush(handle_)){DWORD e=GetLastError();if(e!=ERROR_INVALID_FUNCTION&&e!=ERROR_NOT_SUPPORTED)return fail(e);}
 if(!verify())return false;
 if(verifyWrites_){
  for(DWORD sector=0;sector<kSize/kSector;){
   if(!dirty_[sector]){++sector;continue;}
   DWORD end=sector;while(end<kSize/kSector&&dirty_[end]&&end-sector<kBuffer/kSector)++end;
   DWORD count=(end-sector)*kSector,got=0;if(!verify())return false;
   // Bypass the write cache: only device reads can confirm persisted bytes.
   if(!api_->Read(handle_,ULONGLONG(sector)*kSector,count,scratch_,got))return fail(GetLastError());
   if(got!=count)return fail(ERROR_READ_FAULT);if(!verify())return false;
   if(memcmp(scratch_,expected_.data()+size_t(sector)*kSector,count))return fail(ERROR_CRC);
   memset(dirty_.data()+sector,0,end-sector);sector=end;
  }
 }
 return verify();
}
