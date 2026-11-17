// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MediaControl.h"
#include "ReadOnlyCdBackend.h"
#include <ctype.h>
namespace {
class NativeMediaControl : public MediaControlApi {
public:
 UINT DriveType(int drive) override{char root[]={char('A'+drive),':','\\',0};return GetDriveTypeA(root);}
 HANDLE Open(int drive,DWORD access) override{char path[]="\\\\.\\A:";path[4]+=char(drive);return CreateFileA(path,access,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);}
 bool Control(HANDLE handle,DWORD code) override{DWORD returned=0;return DeviceIoControl(handle,code,nullptr,0,nullptr,0,&returned,nullptr)!=FALSE;}
 bool Close(HANDLE handle) override{return CloseHandle(handle)!=FALSE;}
} native;
Storage::Result failure(DWORD error,const char* stage,bool reload){
 if(!error)error=ERROR_IO_DEVICE;char system[512]={};FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,error,0,system,sizeof(system),nullptr);
 std::string message=std::string(reload?"Reload":"Eject")+" failed at "+stage+" (Windows error "+std::to_string(error)+"). "+system;
 if(error==ERROR_INVALID_FUNCTION||error==ERROR_NOT_SUPPORTED)message+="\r\nThe drive does not support this operation. A manually operated drive needs manual media removal/insertion.";
 if(error==ERROR_ACCESS_DENIED||error==ERROR_SHARING_VIOLATION||error==ERROR_LOCK_VIOLATION||error==ERROR_BUSY)message+="\r\nClose other programs using this drive and try again. Windows may require administrator access.";
 return {1,error,message};
}
struct Volume {
 MediaControlApi* api;HANDLE handle=INVALID_HANDLE_VALUE;bool locked=false;
 ~Volume(){if(handle!=INVALID_HANDLE_VALUE){if(locked)api->Control(handle,FSCTL_UNLOCK_VOLUME);api->Close(handle);}}
};
}
bool CanControlMedia(MediaControlTarget target){return target.drive>=0&&target.drive<26&&(target.type==DRIVE_CDROM||target.type==DRIVE_REMOVABLE);}
MediaControlTarget MacMediaControlTarget(const char* path){
 if(!readonly_media_path(path))return {};
 int floppy=physical_floppy_path(path);return {floppy>=0?floppy:toupper((unsigned char)path[0])-'A',UINT(floppy>=0?DRIVE_REMOVABLE:DRIVE_CDROM)};
}
std::function<Storage::Result(Storage::Job&)> MakeMediaControlJob(MediaControlTarget target,bool reload,MediaControlApi* api,std::function<void()> release,CommandStorage::Reservation held){
 auto work=[=](Storage::Job& job)->Storage::Result {
  if(!CanControlMedia(target))return failure(ERROR_BAD_DEVICE,"drive selection",reload);
#ifdef HFV_TESTING
  if(!api)return failure(ERROR_NOT_SUPPORTED,"missing simulated device",reload);
#endif
  auto os=api?api:&native;
  if(os->DriveType(target.drive)!=target.type)return failure(ERROR_BAD_DEVICE,"drive type changed",reload);
  Storage::Progress p;p.phase=reload?"Reloading media...":"Ejecting media...";p.path=std::string(1,char('A'+target.drive))+":";job.progress(p);
  if(job.cancelled())throw Storage::Cancelled();
  if(release)release();
  if(os->DriveType(target.drive)!=target.type)return failure(ERROR_BAD_DEVICE,"drive type changed",reload);
  Volume volume{os};volume.handle=os->Open(target.drive,target.type==DRIVE_CDROM?GENERIC_READ:GENERIC_READ|GENERIC_WRITE);
  if(volume.handle==INVALID_HANDLE_VALUE)return failure(GetLastError(),"opening the drive",reload);
  if(!reload){
   DWORD error=0;
   for(int attempt=0;attempt<20;++attempt){
    if(os->Control(volume.handle,FSCTL_LOCK_VOLUME)){volume.locked=true;break;}
    error=GetLastError();if(error!=ERROR_ACCESS_DENIED&&error!=ERROR_SHARING_VIOLATION&&error!=ERROR_LOCK_VIOLATION)break;
    if(attempt<19)os->Pause();
   }
   // An empty optical drive has no mounted filesystem to lock or dismount.
   bool emptyCd=target.type==DRIVE_CDROM&&(error==ERROR_NO_MEDIA_IN_DRIVE||error==ERROR_NOT_READY);
   if(!volume.locked&&!emptyCd)return failure(error,"locking the volume",reload);
   if(volume.locked&&!os->Control(volume.handle,FSCTL_DISMOUNT_VOLUME))return failure(GetLastError(),"dismounting the volume",reload);
  }
  // Do not clear other applications' media-removal locks or force dismount.
  if(!os->Control(volume.handle,reload?IOCTL_STORAGE_LOAD_MEDIA:IOCTL_STORAGE_EJECT_MEDIA))return failure(GetLastError(),reload?"loading media":"ejecting media",reload);
  if(volume.locked){os->Control(volume.handle,FSCTL_UNLOCK_VOLUME);volume.locked=false;}
  HANDLE handle=volume.handle;volume.handle=INVALID_HANDLE_VALUE;
  if(!os->Close(handle))return failure(GetLastError(),"closing the drive after the request completed",reload);
  return {};
 };
 if(!CanControlMedia(target))return work;auto key=VolumeCoord::DeviceKey(target.drive);return CommandStorage::Guard({{key,VolumeCoord::Mode::Write}},work,{key},held);
}
