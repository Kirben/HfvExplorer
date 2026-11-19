// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "MediaControl.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "BackgroundCopies.h"
#include "BrowserFolderReader.h"
#include <stdexcept>
#include <vector>
#include "FakeFloppy.h"
#include "FloppyWriter.h"
#include "ReadOnlyCdBackend.h"
#define _FHOOK_INTERNAL_
#include "Filehook.h"
#include <fcntl.h>
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
class FakeControl : public MediaControlApi {
public:
 UINT type=DRIVE_CDROM;DWORD failCode=0,error=ERROR_NOT_SUPPORTED,openError=0,access=0;unsigned opens=0,closes=0,pauses=0,lockFailures=0,delay=0;bool closeOK=true;
 std::vector<DWORD> commands;std::function<void()> onOpen;
 UINT DriveType(int) override{return type;}
 HANDLE Open(int drive,DWORD flags) override{check(drive==3,"wrong selected drive");++opens;access=flags;if(onOpen)onOpen();if(openError){SetLastError(openError);return INVALID_HANDLE_VALUE;}return (HANDLE)uintptr_t(123);}
 bool Control(HANDLE,DWORD code) override{commands.push_back(code);if(delay&&(code==IOCTL_STORAGE_LOAD_MEDIA||code==IOCTL_STORAGE_EJECT_MEDIA))Sleep(delay);if(code==FSCTL_LOCK_VOLUME&&lockFailures){--lockFailures;SetLastError(ERROR_SHARING_VIOLATION);return false;}if(code==failCode){SetLastError(error);return false;}return true;}
 bool Close(HANDLE) override{++closes;if(!closeOK)SetLastError(ERROR_INVALID_HANDLE);return closeOK;}
 void Pause() override{++pauses;}
};
Storage::Result run(FakeControl& api,bool reload=false,std::function<void()> release={}){auto job=Storage::worker().submit(MakeMediaControlJob({3,api.type},reload,&api,release));job->wait();return job->result();}
}
int RunMediaControlTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;FILE* report=nullptr;fopen_s(&report,(std::string(directory)+"\\media-control-report.txt").c_str(),"w");if(!report)return 2;
 try{
  check(CanControlMedia({3,DRIVE_CDROM})&&CanControlMedia({0,DRIVE_REMOVABLE})&&!CanControlMedia({3,DRIVE_FIXED})&&!CanControlMedia({-1,DRIVE_CDROM}),"command eligibility");
  check(MacMediaControlTarget("D:\\NP~1_2~3.DSK").drive==3&&MacMediaControlTarget("A:\\NO~1_2~3.DSK").type==DRIVE_REMOVABLE&&!CanControlMedia(MacMediaControlTarget("D:\\game.dsk")),"physical Mac path detection excludes image files");
  FakeControl cd;bool released=false;cd.onOpen=[&]{check(released,"own readers must close before opening control handle");};
  check(run(cd,false,[&]{released=true;}).code==0&&cd.access==GENERIC_READ&&cd.commands==std::vector<DWORD>{FSCTL_LOCK_VOLUME,FSCTL_DISMOUNT_VOLUME,IOCTL_STORAGE_EJECT_MEDIA,FSCTL_UNLOCK_VOLUME}&&cd.closes==1,"CD eject sequence");
  FakeControl removable;removable.type=DRIVE_REMOVABLE;check(run(removable).code==0&&removable.access==(GENERIC_READ|GENERIC_WRITE),"removable volume access");
  FakeControl reload;check(run(reload,true).code==0&&reload.commands==std::vector<DWORD>{IOCTL_STORAGE_LOAD_MEDIA}&&reload.closes==1,"reload empty drive without lock or dismount");
  FakeControl empty;empty.failCode=FSCTL_LOCK_VOLUME;empty.error=ERROR_NO_MEDIA_IN_DRIVE;check(run(empty).code==0&&empty.commands==std::vector<DWORD>{FSCTL_LOCK_VOLUME,IOCTL_STORAGE_EJECT_MEDIA},"empty optical tray eject");
  FakeControl busy;busy.lockFailures=20;check(run(busy).code!=0&&busy.pauses==19&&busy.commands.size()==20&&busy.closes==1,"busy drive never forced or ejected");
  for(DWORD stage:{DWORD(FSCTL_DISMOUNT_VOLUME),DWORD(IOCTL_STORAGE_EJECT_MEDIA),DWORD(IOCTL_STORAGE_LOAD_MEDIA)}){
   FakeControl failure;failure.failCode=stage;auto result=run(failure,stage==IOCTL_STORAGE_LOAD_MEDIA);check(result.error==ERROR_NOT_SUPPORTED&&result.message.find("does not support")!=std::string::npos&&failure.closes==1,"unsupported control reports failure and closes");
   if(stage!=IOCTL_STORAGE_LOAD_MEDIA)check(failure.commands.back()==FSCTL_UNLOCK_VOLUME,"locked failure unlocks");
  }
  FakeControl denied;denied.openError=ERROR_ACCESS_DENIED;check(run(denied).error==ERROR_ACCESS_DENIED&&denied.commands.empty()&&denied.closes==0,"open errors");
  FakeControl changed;check(run(changed,false,[&]{changed.type=DRIVE_FIXED;}).error==ERROR_BAD_DEVICE&&changed.opens==0,"drive reclassification before open");
  FakeControl fixed;fixed.type=DRIVE_FIXED;check(run(fixed).error==ERROR_BAD_DEVICE&&fixed.opens==0,"fixed drive rejected");
  FakeControl close;close.closeOK=false;check(run(close,true).code!=0&&close.closes==1,"close failure after request reports completed stage without double close");
  std::vector<BYTE> image;std::string why;check(PrepareFloppyHfs("EjectTest",image,why),"prepare media suspend fixture");FakeFloppy disk(image);
  readonly_cd_backend_init();check(readonly_floppy_attach_api(25,&disk),"attach media suspend fixture");
  const char* path="Z:\\NO~1_2~3.DSK";int descriptor=fhook_open(path,O_RDONLY);BYTE bytes[512];
  check(descriptor>=0&&fhook_read(descriptor,bytes,sizeof(bytes))==sizeof(bytes),"cache media before ejection");native_media_suspend(25);
  check(fhook_read(descriptor,bytes,1)<0&&disk.opens==disk.closes,"media suspension invalidates cached descriptors and closes own handles");
  descriptor=fhook_open(path,O_RDONLY);check(descriptor>=0&&fhook_read(descriptor,bytes,1)==1,"drive can reopen after failed eject or reload");fhook_close(descriptor);readonly_cd_backend_final();check(readonly_floppy_attach_api(25,nullptr),"detach media suspend fixture");
  CWnd owner;check(owner.CreateEx(0,AfxRegisterWndClass(0),"Hidden media control owner",WS_OVERLAPPED,0,0,100,100,nullptr,0)!=FALSE,"hidden owner");AfxGetApp()->m_pMainWnd=&owner;InitializeStorageUi();storageUiTest={};
  FakeControl slow;slow.delay=250;auto result=ShowStorageWaitJob(MakeMediaControlJob({3,DRIVE_CDROM},true,&slow),true);
  check(result.code==0&&storageUiTest.timerTicks>=2&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"responsive media control and owner restoration");  CWnd other;
  check(other.CreateEx(0,AfxRegisterWndClass(0),"Other hidden browser",WS_OVERLAPPED,0,0,100,100,nullptr,0)!=FALSE,"second hidden browser");
  FakeControl background;
  background.delay=450;
  const auto device=VolumeCoord::DeviceKey(3);
  const std::vector<VolumeCoord::Access> access{{device,VolumeCoord::Mode::Write}};
  auto reservation=std::make_shared<VolumeCoord::Lease>(access);
  bool completed=false,success=false;
  auto work=MakeMediaControlJob({3,DRIVE_CDROM},true,&background,{},std::move(reservation));
  check(BackgroundCopies::StartStorage(std::move(work),&owner,"Reloading media","Reload",
      [&](bool ok){completed=true;success=ok;}),"start background media command");
  check(!VolumeCoord::Available(access)&&owner.IsWindowEnabled()&&other.IsWindowEnabled()&&!StorageUi::busy(),
        "background media command disables only its device");
  BrowserFolderReader reader({});
  BrowserDirectory::Snapshot listing;
  const auto parent=VolumeCoord::Wide(directory).substr(0,VolumeCoord::Wide(directory).find_last_of(L"\\/"));
  auto browsed=ShowStorageWaitJob([&](Storage::Job& job){
   listing=reader.read_windows_browser_directory(0,parent,true,job);
   return Storage::Result{};
  },true,&other);
  check(!browsed.code&&!listing.rows.empty()&&!completed,"second browser can list an unrelated folder during reload");
  BackgroundCopies::TransferOwner(owner.GetSafeHwnd(),other.GetSafeHwnd());
  owner.DestroyWindow();
  const auto started=GetTickCount();
  while(!completed&&GetTickCount()-started<5000){
   MSG message{};
   while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessage(&message);}
   Sleep(1);
  }
  check(completed&&success&&VolumeCoord::Available(access)&&!BackgroundCopies::Active()&&other.IsWindowEnabled(),
        "completion releases the device and survives closing the original owner");
  other.DestroyWindow();AfxGetApp()->m_pMainWnd=nullptr;
  fprintf(report,"PASS: physical-only selection, own-reader release, CD/removable access, lock/dismount/eject order, empty-tray reload/eject, busy/unsupported/access/drive-change/close errors, balanced cleanup, responsive wait/owner restoration, and modeless reload with a same-device reservation, concurrent folder browsing and independent owner closure. All media controls simulated.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());fclose(report);return 1;}
 fclose(report);return 0;
}
#endif
