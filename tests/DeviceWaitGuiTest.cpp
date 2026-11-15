// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "DeviceWait.h"
#include "StorageJobDialog.h"
#include "RefreshStorage.h"
#include "FakeFloppy.h"
#include <atomic>
#include <stdio.h>
#include <stdexcept>
class SyntheticOwner : public CWnd {
public:
 unsigned paints=0;
 LRESULT WindowProc(UINT message,WPARAM w,LPARAM l) override {
  if(message==WM_PAINT){++paints;PAINTSTRUCT paint;::BeginPaint(GetSafeHwnd(),&paint);::EndPaint(GetSafeHwnd(),&paint);return 0;}
  return CWnd::WindowProc(message,w,l);
 }
};
class SlowMedia : public FakeFloppy {
public:
 SlowMedia():FakeFloppy(std::vector<BYTE>(1474560,0x69)){}
 bool Read(HANDLE h,ULONGLONG offset,DWORD count,void* buffer,DWORD& got) override {
  Sleep(250);return FakeFloppy::Read(h,offset,count,buffer,got);
 }
};
int RunDeviceWaitGuiTest(const char* report) {
 FILE* out=nullptr;if(fopen_s(&out,report,"w") || !out)return 2;
 try {
  SyntheticOwner owner;CString name=AfxRegisterWndClass(0);
  if(!owner.CreateEx(0,name,"Synthetic storage owner",WS_OVERLAPPED,0,0,100,100,nullptr,0))throw std::runtime_error("Create hidden owner");
  AfxGetApp()->m_pMainWnd=&owner;InitializeStorageUi();DWORD ui=GetCurrentThreadId();HWND ownerWindow=owner.GetSafeHwnd();
  auto check=[](bool ok,const char* text){if(!ok)throw std::runtime_error(text);};
  HWND fakeDialog=reinterpret_cast<HWND>(UINT_PTR(123)),otherWindow=reinterpret_cast<HWND>(UINT_PTR(456));
  check(StorageOwnerShouldActivate(true,ownerWindow,fakeDialog,fakeDialog)&&StorageOwnerShouldActivate(true,ownerWindow,fakeDialog,ownerWindow),"Active storage dialog must return activation to its owner");
  check(!StorageOwnerShouldActivate(true,ownerWindow,fakeDialog,otherWindow)&&!StorageOwnerShouldActivate(false,ownerWindow,fakeDialog,fakeDialog)&&!StorageOwnerShouldActivate(true,ownerWindow,fakeDialog,nullptr),"Completion must not steal activation from another application or activate disabled owners");
  for(int failure=0;failure<3;++failure) {
   storageUiTest={};unsigned beforePaint=owner.paints;
   auto result=ShowStorageJob([&](Storage::Job& job)->Storage::Result {
    check(GetCurrentThreadId()!=ui,"Work stayed on UI thread");PostMessage(ownerWindow,WM_PAINT,0,0);Sleep(350);
    if(failure==2)throw std::runtime_error("Injected error");
    if(failure==1)return {1,ERROR_NOT_READY,"Injected read error"};
    Storage::Progress progress;progress.phase="Complete";job.progress(progress);return {};
   },"Synthetic slow storage",true);
   check(owner.paints>beforePaint&&storageUiTest.timerTicks>=2 && storageUiTest.disabledDuring,"Normal modal timers did not run during slow read");
   check(storageUiTest.enabledAfter && storageUiTest.dialogClosed && storageUiTest.ownerRestoredBeforeClose && IsWindowEnabled(owner.GetSafeHwnd())&&!StorageUi::busy(),"Owner must be restored before modal dialog closes");
   check((failure==0)==(result.code==0),"Error not delivered");
  }
  storageUiTest={};storageUiTest.hiddenNative=true;storageUiTest.explanationDelayMs=150;
  {
   StorageAction action;
   check(HfvDeviceWait::invoke([]{Sleep(100);return TRUE;})!=FALSE,"First session read failed");
   check(!owner.IsWindowEnabled(),"Session restored owner before action finished");
   check(storageUiTest.simpleStorageLayout,"Storage wait must contain spinner, fixed status and explanation without copy fields or bar");
   check(storageUiTest.explanationShows==0&&storageUiTest.storageMessage=="Waiting for storage...","Short wait must retain fixed status without explanation");
   {
    StorageAction nested;
    BOOL read=HfvDeviceWait::invoke([]{Sleep(240);SetLastError(ERROR_NOT_READY);return FALSE;});
    DWORD error=GetLastError();
    check(!read&&error==ERROR_NOT_READY,"Session read error changed");
   }
   check(HfvDeviceWait::invoke([]{return TRUE;})!=FALSE,"Ready session read failed");
   check(storageUiTest.explanationShows==1&&storageUiTest.spinnerTicks>=2&&storageUiTest.storageMessage=="Waiting for storage...","Long reused wait must animate and show explanation once without changing status");
   check(storageUiTest.dialogCreations==1&&!owner.IsWindowEnabled(),"Repeated reads opened multiple wait windows");
  }
  check(storageUiTest.dialogCreations==1&&owner.IsWindowEnabled()&&storageUiTest.dialogClosed&&storageUiTest.ownerRestoredBeforeClose&&!StorageUi::state().sessionOpen&&!StorageUi::busy(),"Single wait window must restore owner before closing at action completion");
  {
   StorageAction action;
   check(HfvDeviceWait::invoke([]{Sleep(80);return TRUE;})!=FALSE,"Next action read failed");
  }
  check(storageUiTest.dialogCreations==2&&owner.IsWindowEnabled()&&storageUiTest.explanationShows==1,"Next action must get fresh window and restart explanation delay");
  owner.EnableWindow(FALSE);ShowStorageJob([](Storage::Job&)->Storage::Result {Sleep(100);return {};},"Disabled owner",true);
  check(!owner.IsWindowEnabled(),"Previously disabled owner was enabled");owner.EnableWindow(TRUE);
  std::atomic<int> running(0),maximum(0);DWORD firstThread=0,secondThread=0;
  auto first=Storage::worker().submit([&](Storage::Job&)->Storage::Result {firstThread=GetCurrentThreadId();maximum=++running;Sleep(100);--running;return {};});
  auto second=Storage::worker().submit([&](Storage::Job&)->Storage::Result {secondThread=GetCurrentThreadId();int n=++running;if(n>maximum)maximum=n;--running;return {};});
  first->wait();second->wait();check(maximum==1&&firstThread==secondThread,"Jobs did not serialize on persistent worker");
  auto snapshots=std::make_shared<std::vector<PhysicalVolume>>();
  auto scan=Storage::worker().submit(MakeMediaScanJob(true,true,snapshots,[](int drive,bool floppy){Sleep(1);return floppy?drive==0:drive==3;}));scan->wait();
  check(scan->result().code==0&&snapshots->size()==2,"Scan snapshot failed");
  storageUiTest={};storageUiTest.explanationDelayMs=50;
  auto startupSnapshots=std::make_shared<std::vector<PhysicalVolume>>();
  auto startup=ShowStorageWaitJob(MakeMediaScanJob(true,false,startupSnapshots,[](int drive,bool){Sleep(5);return drive==0;}),true);
  check(startup.code==0&&startupSnapshots->size()==1&&storageUiTest.simpleStorageLayout&&storageUiTest.copyBarCount==0&&storageUiTest.dialogCreations==1,"Startup/media scans must use compact spinner layout instead of copy dialog");
  check(storageUiTest.explanationShows==1&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"Startup scan must preserve delayed explanation and restoration");
  {
   Storage::Worker stopping;std::atomic<int> completed(0);
   auto queued=stopping.submit([&](Storage::Job&)->Storage::Result {Sleep(30);++completed;return {};});
   stopping.shutdown();check(queued->done()&&completed==1,"Shutdown did not drain the queue");
   bool rejected=false;try{stopping.submit([](Storage::Job&)->Storage::Result{return {};});}catch(const std::exception&){rejected=true;}
   check(rejected,"Shut down worker accepted more work");
  }
  owner.DestroyWindow();AfxGetApp()->m_pMainWnd=nullptr;
  fprintf(out,"PASS: persistent serialized worker, slow reads with normal UI timers, read errors and exceptions, completed dialog closure, main-window restoration, preservation of disabled owner, media scan snapshots, one reused wait window per action (including nested reads and errors), spinner-only layout, fixed status, delayed explanation once per action and delay reset; synthetic hidden windows only.\n");
 } catch(const std::exception& error){fprintf(out,"FAIL: %s\n",error.what());fclose(out);return 1;}
 fclose(out);return 0;
}
#endif
