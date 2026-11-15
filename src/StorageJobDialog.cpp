// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "DeviceWait.h"
#include "StorageJobDialog.h"
#include <shobjidl.h>
#include <cmath>
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"uuid.lib")
#ifdef HFV_TESTING
StorageUiTestState storageUiTest;
#endif
bool StorageOwnerShouldActivate(bool originallyEnabled,HWND owner,HWND dialog,HWND foreground) {
 return originallyEnabled&&owner&&foreground&&(foreground==owner||(dialog&&foreground==dialog));
}
namespace {
class ConflictDialog : public CDialog {
 CString path_;
public:
 ConflictDialog(const std::string& path,CWnd* owner):CDialog(IDD_COPY_CONFLICT,owner),path_(path.c_str()){}
 BOOL OnInitDialog() override {CDialog::OnInitDialog();SetDlgItemText(IDC_CONFLICT_PATH,path_);return TRUE;}
 BOOL OnCommand(WPARAM w,LPARAM l) override {
  if(LOWORD(w)>=IDC_REPLACE && LOWORD(w)<=IDC_COPY_CANCEL){EndDialog(LOWORD(w));return TRUE;}
  return CDialog::OnCommand(w,l);
 }
 void OnCancel() override {EndDialog(IDC_COPY_CANCEL);}
};
class JobDialog : public CDialog {
 std::shared_ptr<Storage::Job> job_;
 CString title_;bool hidden_,waiting_=true,keepWindow_=false,storageOnly_=false,cancellable_=false;
 ITaskbarList3* taskbar_=nullptr;bool uninitialize_=false;TBPFLAG taskbarState_=TBPF_NOPROGRESS;
 HWND owner_;
 std::function<void(HWND)> beforeClose_;
 unsigned spinnerFrame_=0;bool explanationShown_=false;
 void drawSpinner(const DRAWITEMSTRUCT& item) {
  const int width=item.rcItem.right-item.rcItem.left,height=item.rcItem.bottom-item.rcItem.top;
  HDC buffer=CreateCompatibleDC(item.hDC);HBITMAP bitmap=CreateCompatibleBitmap(item.hDC,width,height);
  if(!buffer||!bitmap){if(bitmap)DeleteObject(bitmap);if(buffer)DeleteDC(buffer);return;}
  HGDIOBJ previous=SelectObject(buffer,bitmap);RECT bounds={0,0,width,height};
  COLORREF background=GetSysColor(COLOR_3DFACE),accent=GetSysColor(COLOR_HIGHLIGHT);
  HBRUSH fill=CreateSolidBrush(background);FillRect(buffer,&bounds,fill);DeleteObject(fill);
  int dot=max(2,min(width,height)/8);double radius=min(width,height)*0.33;
  HGDIOBJ pen=SelectObject(buffer,GetStockObject(NULL_PEN));
  for(unsigned n=0;n<8;++n){
   unsigned strength=40+((n+8-spinnerFrame_)%8)*30;
   auto blend=[&](int a,int b){return BYTE((a*strength+b*(255-strength))/255);};
   HBRUSH brush=CreateSolidBrush(RGB(blend(GetRValue(accent),GetRValue(background)),blend(GetGValue(accent),GetGValue(background)),blend(GetBValue(accent),GetBValue(background))));
   HGDIOBJ oldBrush=SelectObject(buffer,brush);double angle=n*3.141592653589793/4;
   int x=int(width/2+radius*cos(angle)),y=int(height/2+radius*sin(angle));
   Ellipse(buffer,x-dot/2,y-dot/2,x+(dot+1)/2,y+(dot+1)/2);
   SelectObject(buffer,oldBrush);DeleteObject(brush);
  }
  SelectObject(buffer,pen);BitBlt(item.hDC,item.rcItem.left,item.rcItem.top,width,height,buffer,0,0,SRCCOPY);
  SelectObject(buffer,previous);DeleteObject(bitmap);DeleteDC(buffer);
 }
 void state(TBPFLAG value) {
  if(taskbar_&&value!=taskbarState_){taskbar_->SetProgressState(owner_,value);taskbarState_=value;}
 }
 void update() {
  auto progress=job_->progress();
  if(storageOnly_){
   DWORD delay=5000;
#ifdef HFV_TESTING
   if(hidden_)delay=storageUiTest.explanationDelayMs;
#endif
   if(!explanationShown_&&GetTickCount()-started_>=delay){
    SetDlgItemText(IDC_SW_EXPLANATION,"The drive is taking longer than usual.");explanationShown_=true;
#ifdef HFV_TESTING
    ++storageUiTest.explanationShows;
#endif
   }
#ifdef HFV_TESTING
   CString text;GetDlgItemText(IDC_CP_PHASE,text);storageUiTest.storageMessage=(LPCSTR)text;
#endif
   state(TBPF_INDETERMINATE);return;
  }
  CString status;status.Format("%u / %u files; %u skipped; %.2f MiB read; %lu seconds",
   progress.files+progress.skipped,progress.totalFiles,progress.skipped,double(progress.bytes)/(1024*1024),(GetTickCount()-started_)/1000);
  if(progress.fileCopy&&!progress.totalFiles)status.Format("Elapsed: %lu seconds",(GetTickCount()-started_)/1000);
  if(!progress.fileCopy)status.Format("Elapsed: %lu seconds",(GetTickCount()-started_)/1000);
  if(progress.byteTransfer&&progress.totalBytes)status.Format("%.2f / %.2f MiB; %u%%; %lu seconds",double(progress.processed)/(1024*1024),double(progress.totalBytes)/(1024*1024),unsigned(min(progress.processed,progress.totalBytes)*100/progress.totalBytes),(GetTickCount()-started_)/1000);
  CString path=progress.path.c_str();
  if(progress.fileTotal){CString percent;percent.Format(" (%u%%)",unsigned(min(progress.fileBytes,progress.fileTotal)*100/progress.fileTotal));path+=percent;}
  CString old;GetDlgItemText(IDC_CP_FILE,old);if(old!=path)SetDlgItemText(IDC_CP_FILE,path);
  GetDlgItemText(IDC_CP_STATUS,old);if(old!=status)SetDlgItemText(IDC_CP_STATUS,status);
  GetDlgItemText(IDC_CP_PHASE,old);if(old!=progress.phase.c_str())SetDlgItemText(IDC_CP_PHASE,progress.phase.c_str());
  bool waiting=progress.scanning || (!progress.totalBytes&&!progress.totalFiles);
  CProgressCtrl* overall=(CProgressCtrl*)GetDlgItem(IDC_CP_BAR);
  if(waiting!=waiting_){
   if(waiting){overall->ModifyStyle(0,PBS_MARQUEE);overall->SendMessage(PBM_SETMARQUEE,TRUE,40);}
   else{overall->SendMessage(PBM_SETMARQUEE,FALSE,0);overall->ModifyStyle(PBS_MARQUEE,0);overall->SetRange32(0,1000);}
   waiting_=waiting;
  }
  if(waiting)state(TBPF_INDETERMINATE);
  else{
   auto total=progress.totalBytes?progress.totalBytes:progress.totalFiles;
   auto value=progress.totalBytes?progress.processed:progress.files+progress.skipped;
   value=min(value,total);int position=int(value*1000/total);if(overall->GetPos()!=position)overall->SetPos(position);state(TBPF_NORMAL);
   if(taskbar_)taskbar_->SetProgressValue(owner_,value,total);
  }
  auto file=(CProgressCtrl*)GetDlgItem(IDC_CP_FILE_BAR);
  int position=progress.fileTotal?int(min(progress.fileBytes,progress.fileTotal)*1000/progress.fileTotal):0;
  if(file&&file->GetPos()!=position)file->SetPos(position);
 }
 DWORD started_=GetTickCount();
public:
 void BeforeClose(std::function<void(HWND)> callback){beforeClose_=std::move(callback);}
 void PrepareToClose(){if(beforeClose_)beforeClose_(GetSafeHwnd());}
 JobDialog(const std::shared_ptr<Storage::Job>& job,const char* title,bool hidden,CWnd* owner,bool keepWindow=false,bool storageOnly=false,bool singleFile=false,bool cancellable=false)
  :CDialog(storageOnly?IDD_STORAGE_WAIT:cancellable?IDD_CD_IMAGE_PROGRESS:singleFile?IDD_COPY_SINGLE_PROGRESS:IDD_COPY_PROGRESS,owner),job_(job),title_(title),hidden_(hidden),keepWindow_(keepWindow),storageOnly_(storageOnly),cancellable_(cancellable),owner_(owner?owner->GetSafeHwnd():nullptr) {
  if(!hidden_){HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);uninitialize_=SUCCEEDED(init);
   if((SUCCEEDED(init)||init==RPC_E_CHANGED_MODE)&&SUCCEEDED(CoCreateInstance(CLSID_TaskbarList,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&taskbar_)))&&FAILED(taskbar_->HrInit())){taskbar_->Release();taskbar_=nullptr;}}
 }
 ~JobDialog(){state(TBPF_NOPROGRESS);if(taskbar_)taskbar_->Release();if(uninitialize_)CoUninitialize();}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();SetWindowText(title_);
#ifdef HFV_TESTING
  ++storageUiTest.dialogCreations;
  if(!storageOnly_)storageUiTest.copyBarCount=(GetDlgItem(IDC_CP_BAR)?1:0)+(GetDlgItem(IDC_CP_FILE_BAR)?1:0);
  if(storageOnly_){
   unsigned children=0;for(HWND child=::GetWindow(m_hWnd,GW_CHILD);child;child=::GetWindow(child,GW_HWNDNEXT))++children;
   storageUiTest.simpleStorageLayout=children==3&&GetDlgItem(IDC_CP_PHASE)&&GetDlgItem(IDC_SW_SPINNER)&&GetDlgItem(IDC_SW_EXPLANATION)&&!GetDlgItem(IDC_CP_BAR)&&!GetDlgItem(IDC_CP_FILE)&&!GetDlgItem(IDC_CP_FILE_BAR)&&!GetDlgItem(IDC_CP_STATUS);
  }
#endif
  auto menu=GetSystemMenu(FALSE);if(menu)menu->EnableMenuItem(SC_CLOSE,MF_BYCOMMAND|MF_GRAYED);
  if(!storageOnly_)GetDlgItem(IDC_CP_BAR)->SendMessage(PBM_SETMARQUEE,TRUE,40);
  if(GetDlgItem(IDC_CP_FILE_BAR))((CProgressCtrl*)GetDlgItem(IDC_CP_FILE_BAR))->SetRange32(0,1000);update();SetTimer(1,50,nullptr);return TRUE;
 }
 int WaitForJob(const std::shared_ptr<Storage::Job>& job) {
  job_=job;update();SetTimer(1,50,nullptr);
  if(job_->done())PostMessage(WM_APP+94,0,0);
  return RunModalLoop(MLF_NOKICKIDLE);
 }
 void OnCancel() override {if(cancellable_){job_->cancel();GetDlgItem(IDCANCEL)->EnableWindow(FALSE);SetDlgItemText(IDCANCEL,"Cancelling...");}}
 void OnOK() override {}
 LRESULT WindowProc(UINT message,WPARAM w,LPARAM l) override {
  if(message==WM_DESTROY)PrepareToClose();
  if(message==WM_DRAWITEM&&w==IDC_SW_SPINNER){drawSpinner(*reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;}
  if(message==WM_CLOSE)return 0;
  if(message==WM_APP+94){
   if(keepWindow_&&job_->done()){KillTimer(1);EndModalLoop(IDOK);}
   return 0;
  }
#ifdef HFV_TESTING
  if(hidden_&&message==WM_WINDOWPOSCHANGING)reinterpret_cast<WINDOWPOS*>(l)->flags&=~SWP_SHOWWINDOW;
#endif
  if(message==WM_TIMER&&w==1){
#ifdef HFV_TESTING
   if(hidden_&&cancellable_&&storageUiTest.cancelOnTimer&&!job_->cancelled())OnCancel();
#endif
   if(storageOnly_&&spinnerFrame_!=((GetTickCount()-started_)/120)%8){spinnerFrame_=((GetTickCount()-started_)/120)%8;GetDlgItem(IDC_SW_SPINNER)->Invalidate(FALSE);
#ifdef HFV_TESTING
    ++storageUiTest.spinnerTicks;
#endif
   }
#ifdef HFV_TESTING
   ++storageUiTest.timerTicks;
   if(owner_&&!::IsWindowEnabled(owner_))storageUiTest.disabledDuring=true;
#endif
   update();
   auto conflict=job_->conflict();
   if(!conflict.empty()){
    KillTimer(1);
#ifdef HFV_TESTING
    if(hidden_)job_->resolve(storageUiTest.answer==Storage::ConflictChoice::None?Storage::ConflictChoice::Cancel:storageUiTest.answer);
    else
#endif
    {
     ConflictDialog prompt(conflict,this);INT_PTR choice=prompt.DoModal();
     job_->resolve(choice==IDC_REPLACE?Storage::ConflictChoice::Replace:choice==IDC_REPLACE_ALL?Storage::ConflictChoice::ReplaceAll:choice==IDC_SKIP?Storage::ConflictChoice::Skip:Storage::ConflictChoice::Cancel);
    }
    SetTimer(1,50,nullptr);
   }
   if(job_->done()){KillTimer(1);if(keepWindow_)EndModalLoop(IDOK);else{state(TBPF_NOPROGRESS);PrepareToClose();EndDialog(IDOK);}}
   return 0;
  }
  return CDialog::WindowProc(message,w,l);
 }
};
class ActiveUiJob {
public:
 ActiveUiJob(){++StorageUi::state().active;}
 ~ActiveUiJob(){--StorageUi::state().active;}
};
class OwnerRecovery {
 HWND window_,focus_;bool enabled_,restored_=false;
public:
 explicit OwnerRecovery(CWnd* owner):window_(owner?owner->GetSafeHwnd():nullptr),focus_(GetFocus()),enabled_(window_&&IsWindowEnabled(window_)){}
 void disable(){if(enabled_&&IsWindow(window_))EnableWindow(window_,FALSE);}
 void restore(HWND dialog=nullptr){
  if(restored_)return;restored_=true;
  if(window_&&IsWindow(window_)){
   bool activate=StorageOwnerShouldActivate(enabled_,window_,dialog,GetForegroundWindow());
   if((IsWindowEnabled(window_)!=FALSE)!=enabled_)EnableWindow(window_,enabled_?TRUE:FALSE);
   if(activate){SetForegroundWindow(window_);SetActiveWindow(window_);}
   if(enabled_&&GetForegroundWindow()==window_&&focus_&&IsWindow(focus_))SetFocus(focus_);
  }
#ifdef HFV_TESTING
  storageUiTest.ownerRestoredBeforeClose=dialog&&IsWindow(dialog)&&window_&&((IsWindowEnabled(window_)!=FALSE)==enabled_);
#endif
 }
 ~OwnerRecovery(){
  restore();
  if(window_&&IsWindow(window_)){
   // MFC can adjust the owner again while tearing down a modal dialog.
   if((IsWindowEnabled(window_)!=FALSE)!=enabled_)EnableWindow(window_,enabled_?TRUE:FALSE);
   RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME);
   PostMessage(window_,WM_APP+91,0,0);
  }
#ifdef HFV_TESTING
  storageUiTest.enabledAfter=window_&&IsWindowEnabled(window_);
  storageUiTest.dialogClosed=true;
#endif
 }
};
class NativeSession {
 OwnerRecovery owner_;
 std::unique_ptr<JobDialog> dialog_;
public:
 NativeSession(const std::shared_ptr<Storage::Job>& job,const char* title,bool hidden,CWnd* owner):owner_(owner){
  owner_.disable();
  dialog_.reset(new JobDialog(job,title,hidden,owner,true,true));
  dialog_->BeforeClose([this](HWND dialog){owner_.restore(dialog);});
  if(!dialog_->Create(IDD_STORAGE_WAIT,owner))throw std::runtime_error("Could not create storage wait window");
  dialog_->ShowWindow(SW_SHOW);StorageUi::state().sessionOpen=true;
 }
 ~NativeSession(){
  if(dialog_&&dialog_->GetSafeHwnd()){dialog_->KillTimer(1);dialog_->PrepareToClose();dialog_->DestroyWindow();}
  StorageUi::state().sessionOpen=false;
 }
 void wait(const std::shared_ptr<Storage::Job>& job){
  ActiveUiJob active;
  if(dialog_->WaitForJob(job)!=IDOK)job->cancel();
  job->wait();
 }
};
unsigned actionDepth=0;
std::unique_ptr<NativeSession> nativeSession;
void wait(const std::shared_ptr<Storage::Job>& job,const char* title,bool hidden,bool storageOnly=false,bool singleFile=false,bool cancellable=false) {
 INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
 ActiveUiJob active;
 CWnd* owner=AfxGetMainWnd();
 if(actionDepth&&storageOnly){
  if(!nativeSession)nativeSession.reset(new NativeSession(job,title,hidden,owner));
  nativeSession->wait(job);return;
 }
 if(!storageOnly)nativeSession.reset();
 OwnerRecovery restore(owner);
 struct Lifetime{
  std::shared_ptr<Storage::Job> job;
  ~Lifetime(){if(!job->done()){job->cancel();job->wait();}}
 } lifetime={job};
 JobDialog dialog(job,title,hidden,owner,false,storageOnly,singleFile,cancellable);
 dialog.BeforeClose([&restore](HWND window){restore.restore(window);});
 if(dialog.DoModal()!=IDOK){job->cancel();job->wait();}
 job->wait();
}
void waitNative(const std::shared_ptr<Storage::Job>& job){
 bool hidden=false;
#ifdef HFV_TESTING
 hidden=storageUiTest.hiddenNative;
#endif
 wait(job,"Waiting for storage",hidden,true);
}
}
StorageAction::StorageAction():active_(GetCurrentThreadId()==StorageUi::state().thread&&!StorageUi::busy()){
 if(active_)++actionDepth;
}
StorageAction::~StorageAction(){
 if(active_&&--actionDepth==0){ActiveUiJob closing;nativeSession.reset();}
}
void InitializeStorageUi(){StorageUi::initialize(waitNative);}
Storage::Result ShowStorageJob(std::function<Storage::Result(Storage::Job&)> work,const char* title,bool hidden,bool singleFile,bool cancellable) {
 if(StorageUi::busy())return {1,ERROR_BUSY,"Another storage operation is active"};
 auto job=Storage::worker().submit(std::move(work));wait(job,title,hidden,false,singleFile,cancellable);return job->result();
}
Storage::Result ShowStorageWaitJob(std::function<Storage::Result(Storage::Job&)> work,bool hidden) {
 if(StorageUi::busy())return {1,ERROR_BUSY,"Another storage operation is active"};
 auto job=Storage::worker().submit(std::move(work));wait(job,"Waiting for storage",hidden,true);return job->result();
}
void ShutdownStorageUi(){
 nativeSession.reset();StorageUi::state().wait=nullptr;StorageUi::state().thread=0;
 Storage::worker().shutdown();
}
