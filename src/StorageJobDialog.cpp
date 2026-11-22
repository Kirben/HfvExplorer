// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DialogDpi.h"
#include "MetadataDialogs.h"
#include "HFVExplorer.h"
#include "resource.h"
#include "DeviceWait.h"
#include "StorageJobDialog.h"
#include "FileOperations.h"
#include "BackgroundCopies.h"
#include "BackgroundJobController.h"
#include "BrowserLifetime.h"
#include "DialogClose.h"
#include "DynCopyModeSelect.h"
#include <atomic>
#include <map>
#include "TaskbarProgress.h"
#include "VolumeCoordinator.h"
#include "BrowserPresentation.h"
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
 Storage::PendingCopyPrompt prompt_;
 bool utf8_;
 std::shared_ptr<Storage::Job> job_;
public:
 ConflictDialog(const Storage::PendingCopyPrompt& prompt,CWnd* owner,bool utf8,std::shared_ptr<Storage::Job> job)
  :CDialog(IDD_COPY_CONFLICT,owner),prompt_(prompt),utf8_(utf8),job_(std::move(job)){}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();
  if(job_)SetTimer(1,50,nullptr);
  const bool uncompressed=prompt_.kind==Storage::CopyPrompt::Uncompressed;
  const bool folders=prompt_.kind==Storage::CopyPrompt::IndependentFolders;
  if(uncompressed||folders||utf8_){
   BrowserPresentation::UnicodeControl(m_hWnd,IDC_CONFLICT_PATH,L"STATIC");
   SetDlgItemTextW(m_hWnd,IDC_CONFLICT_PATH,BrowserPresentation::FromUtf8(prompt_.path).c_str());
  }else SetDlgItemText(IDC_CONFLICT_PATH,prompt_.path.c_str());
  if(uncompressed||folders){
   SetWindowText(folders?"Copy linked folders":"Copy without compression");
   SetDlgItemText(IDC_CONFLICT_FILE_LABEL,folders?"First linked folder":"File");
   SetDlgItemText(IDC_CONFLICT_QUESTION,folders?"APFS cannot share folders this way. Copy them as independent folders?":
                                              "Copy without compression to preserve both forks?");
   SetDlgItemText(IDC_REPLACE,folders?"&Copy folders":"Copy &uncompressed");
   GetDlgItem(IDC_REPLACE_ALL)->ShowWindow(SW_HIDE);
   GetDlgItem(IDC_SKIP)->ShowWindow(SW_HIDE);
   CRect button(174,72,278,90);
   MapDialogRect(&button);
   GetDlgItem(IDC_REPLACE)->MoveWindow(&button);
  }
  return TRUE;
 }
 BOOL OnCommand(WPARAM w,LPARAM l) override {
  if(LOWORD(w)>=IDC_REPLACE && LOWORD(w)<=IDC_COPY_CANCEL){EndDialog(LOWORD(w));return TRUE;}
  return CDialog::OnCommand(w,l);
 }
 void OnCancel() override {EndDialog(IDC_COPY_CANCEL);}
 LRESULT WindowProc(UINT m,WPARAM w,LPARAM l)override{if(m==WM_TIMER&&w==1&&job_&&(job_->cancelled()||job_->done())){EndDialog(IDC_COPY_CANCEL);return 0;}return CDialog::WindowProc(m,w,l);}
};
class StorageSpinner : public CStatic {
 unsigned frame_=0;
 void draw(HDC target) {
  CRect client;GetClientRect(&client);const int width=client.Width(),height=client.Height();
  HDC buffer=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,width,height);
  if(!buffer||!bitmap){if(bitmap)DeleteObject(bitmap);if(buffer)DeleteDC(buffer);return;}
  HGDIOBJ previous=SelectObject(buffer,bitmap);RECT bounds={0,0,width,height};
  COLORREF background=GetSysColor(COLOR_3DFACE),accent=GetSysColor(COLOR_HIGHLIGHT);
  HBRUSH fill=CreateSolidBrush(background);FillRect(buffer,&bounds,fill);DeleteObject(fill);
  // Three-pixel null-pen ellipses can rasterize to no visible fill.
  int dot=max(NativeDpi::Scale(4,NativeDpi::Window(m_hWnd)),min(width,height)/7);double radius=min(width,height)*0.33;
  HGDIOBJ pen=SelectObject(buffer,GetStockObject(NULL_PEN));
  for(unsigned n=0;n<8;++n){
   unsigned strength=40+((n+8-frame_)%8)*30;
   auto blend=[&](int a,int b){return BYTE((a*strength+b*(255-strength))/255);};
   COLORREF color=RGB(blend(GetRValue(accent),GetRValue(background)),blend(GetGValue(accent),GetGValue(background)),blend(GetBValue(accent),GetBValue(background)));
   HBRUSH brush=CreateSolidBrush(color);
   HGDIOBJ oldBrush=SelectObject(buffer,brush);double angle=n*3.141592653589793/4;
   int x=int(width/2+radius*cos(angle)),y=int(height/2+radius*sin(angle));
   ::Ellipse(buffer,x-dot/2,y-dot/2,x+(dot+1)/2,y+(dot+1)/2);
   SelectObject(buffer,oldBrush);DeleteObject(brush);
  }
  SelectObject(buffer,pen);
  BitBlt(target,0,0,width,height,buffer,0,0,SRCCOPY);
  SelectObject(buffer,previous);DeleteObject(bitmap);DeleteDC(buffer);
 }
public:
 void Start(){SetTimer(1,120,nullptr);Invalidate(FALSE);}
 LRESULT WindowProc(UINT message,WPARAM w,LPARAM l) override {
  if(message==WM_ERASEBKGND)return TRUE;
  if(message==WM_PAINT){CPaintDC dc(this);draw(dc.GetSafeHdc());return 0;}
  if(message==WM_PRINTCLIENT){if(w)draw(reinterpret_cast<HDC>(w));return 0;}
  if(message==WM_TIMER&&w==1){frame_=(frame_+1)%8;Invalidate(FALSE);
#ifdef HFV_TESTING
   ++storageUiTest.spinnerTicks;
#endif
   return 0;
  }
  return CStatic::WindowProc(message,w,l);
 }
};
struct TaskbarEntry{HWND owner;TaskbarProgress::Value progress;};
std::map<const void*,TaskbarEntry> taskbarEntries; // UI thread only.
class JobDialog : public CDialog {
 std::shared_ptr<Storage::Job> job_;
 bool modeless_=false,completed_=false,mutation_=false,handlingTimer_=false;
 bool dismissOnCompletion_=false;unsigned dispatchDepth_=0;
 std::string operation_="Copy",successMessage_;
 std::function<bool()> advance_;
 std::function<Storage::Result()> finished_;
 std::function<void()> cancelled_;
 std::function<void(uint64_t,Storage::ConflictChoice)> resolve_;
 std::function<void(const char*)> failed_;
 std::wstring imageDestination_;
 CString title_;bool hidden_,waiting_=true,keepWindow_=false,storageOnly_=false,cancellable_=false;
 ITaskbarList3* taskbar_=nullptr;bool uninitialize_=false;uint64_t taskbarDone_=0,taskbarTotal_=0;
 HWND owner_;
 std::function<void(HWND)> beforeClose_;
 StorageSpinner spinner_;bool explanationShown_=false;
 void state(TBPFLAG value) {
  if(value==TBPF_NOPROGRESS)taskbarEntries.erase(this);else taskbarEntries[this]={owner_,{value==TBPF_INDETERMINATE,taskbarDone_,taskbarTotal_}};
  std::vector<TaskbarProgress::Value> values;for(auto& item:taskbarEntries)if(item.second.owner==owner_)values.push_back(item.second.progress);
  auto summary=TaskbarProgress::Combine(values);
  if(taskbar_&&owner_&&IsWindow(owner_)){taskbar_->SetProgressState(owner_,!summary.active?TBPF_NOPROGRESS:summary.waiting?TBPF_INDETERMINATE:TBPF_NORMAL);if(summary.active&&!summary.waiting)taskbar_->SetProgressValue(owner_,summary.done,summary.total);}
 }
 void update() {
  auto progress=job_->progress();
  if(modeless_&&progress.phase.empty()){CString text;GetDlgItemText(IDC_CP_PHASE,text);if(text!="Waiting for another transfer...")SetDlgItemText(IDC_CP_PHASE,"Waiting for another transfer...");return;}
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
  if(!progress.detail.empty()){status=progress.detail.c_str();CString elapsed;elapsed.Format("; %lu seconds",(GetTickCount()-started_)/1000);status+=elapsed;}
  if(progress.byteTransfer&&progress.totalBytes)status.Format("%.2f / %.2f MiB; %lu seconds",double(progress.processed)/(1024*1024),double(progress.totalBytes)/(1024*1024),(GetTickCount()-started_)/1000);
  if(!imageDestination_.empty()&&progress.fileTotal&&!progress.path.empty())status=progress.path.c_str()+CString(" - ")+status;
  CString old;
  if(imageDestination_.empty()){
   if(progress.utf8){BrowserPresentation::UnicodeControl(m_hWnd,IDC_CP_FILE,L"STATIC");auto path=BrowserPresentation::FromUtf8(progress.path);if(BrowserPresentation::Text(::GetDlgItem(m_hWnd,IDC_CP_FILE))!=path)SetDlgItemTextW(m_hWnd,IDC_CP_FILE,path.c_str());}
   else{GetDlgItemText(IDC_CP_FILE,old);if(old!=progress.path.c_str())SetDlgItemText(IDC_CP_FILE,progress.path.c_str());}
  }
  GetDlgItemText(IDC_CP_STATUS,old);if(old!=status)SetDlgItemText(IDC_CP_STATUS,status);
  GetDlgItemText(IDC_CP_PHASE,old);if(old!=progress.phase.c_str())SetDlgItemText(IDC_CP_PHASE,progress.phase.c_str());
  bool waiting=(mutation_&&imageDestination_.empty())||progress.scanning || (!progress.totalBytes&&!progress.totalFiles);
  CProgressCtrl* overall=(CProgressCtrl*)GetDlgItem(IDC_CP_BAR);
  if(waiting!=waiting_){
   if(waiting){overall->ModifyStyle(0,PBS_MARQUEE);overall->SendMessage(PBM_SETMARQUEE,TRUE,40);}
   else{overall->SendMessage(PBM_SETMARQUEE,FALSE,0);overall->ModifyStyle(PBS_MARQUEE,0);overall->SetRange32(0,1000);}
   waiting_=waiting;
  }
  if(waiting){SetPercent(IDC_CP_PERCENT,-1);state(TBPF_INDETERMINATE);}
  else{
   auto total=progress.totalBytes?progress.totalBytes:progress.totalFiles;
   auto value=progress.totalBytes?progress.processed:progress.files+progress.skipped;
   value=min(value,total);int position=ProgressPosition(value,total);if(overall->GetPos()!=position)overall->SetPos(position);SetPercent(IDC_CP_PERCENT,position/10);taskbarDone_=value;taskbarTotal_=total;state(TBPF_NORMAL);
  }
  auto file=(CProgressCtrl*)GetDlgItem(IDC_CP_FILE_BAR);
  if(file){
   const int position=ProgressPosition(progress.fileBytes,progress.fileTotal);
   if(file->GetPos()!=position)file->SetPos(position);
   SetPercent(IDC_CP_FILE_PERCENT,progress.fileTotal?position/10:-1);
  }
 }
 static int ProgressPosition(uint64_t done,uint64_t total) {
  if(!total)return 0;
  if(done>=total)return 1000;
  return min(999,int(double(done)*1000/total));
 }
 void SetPercent(int id,int value) {
  CString text,old;
  if(value>=0)text.Format("%d%%",value);
  GetDlgItemText(id,old);
  if(old!=text)SetDlgItemText(id,text);
#ifdef HFV_TESTING
  if(hidden_){
   if(id==IDC_CP_PERCENT)storageUiTest.overallPercent=value;
   if(id==IDC_CP_FILE_PERCENT)storageUiTest.filePercent=value;
  }
#endif
 }
 void ResultDetails(const std::string& detail) {
  if(GetDlgItem(IDC_CP_PATH_LABEL))SetDlgItemText(IDC_CP_PATH_LABEL,"Details");
  BrowserPresentation::UnicodeControl(m_hWnd,IDC_CP_FILE,L"STATIC");
  SetDlgItemTextW(m_hWnd,IDC_CP_FILE,BrowserPresentation::FromUtf8(detail).c_str());
 }
 DWORD started_=GetTickCount();
public:
 void ImageDestination(const std::wstring& path){imageDestination_=path;}
 void SuccessMessage(const char* message){if(message)successMessage_=message;}
 void Operation(const char* operation){operation_=operation;mutation_=operation_!="Copy"&&operation_!="Resource import";}
 void DismissOnCompletion(){dismissOnCompletion_=true;}
 void Planning(){job_=std::make_shared<Storage::Job>();Storage::Progress p;p.phase="Choose a copy mode";job_->progress(p);job_->finish({});}
 HWND ProgressOwner()const{return owner_;}
 void ProgressOwner(HWND window){
  if(owner_==window)return;
  auto found=taskbarEntries.find(this);bool tracked=found!=taskbarEntries.end();bool waiting=tracked&&found->second.progress.waiting;
  if(tracked)state(TBPF_NOPROGRESS); // Clear only this job's old taskbar contribution.
  owner_=window;if(tracked)state(waiting?TBPF_INDETERMINATE:TBPF_NORMAL);
 }
 void Background(std::function<bool()> next, std::function<Storage::Result()> finish,
     std::function<void()> cancel, std::function<void(uint64_t,Storage::ConflictChoice)> resolve,
     std::function<void(const char*)> fail) {
  modeless_=true;advance_=std::move(next);finished_=std::move(finish);cancelled_=std::move(cancel);
  resolve_=std::move(resolve);failed_=std::move(fail);
 }
 void Next(const std::shared_ptr<Storage::Job>& job){job_=job;started_=GetTickCount();waiting_=true;GetDlgItem(IDC_CP_BAR)->SendMessage(PBM_SETMARQUEE,TRUE,40);update();}
 void BeforeClose(std::function<void(HWND)> callback){beforeClose_=std::move(callback);}
 void PrepareToClose(){if(beforeClose_)beforeClose_(GetSafeHwnd());}
 JobDialog(const std::shared_ptr<Storage::Job>& job,const char* title,bool hidden,CWnd* owner,bool keepWindow=false,bool storageOnly=false,bool singleFile=false,bool cancellable=false,bool fileLayout=false)
  :CDialog(storageOnly?IDD_STORAGE_WAIT:(cancellable&&!fileLayout)?IDD_CD_IMAGE_PROGRESS:singleFile?IDD_COPY_SINGLE_PROGRESS:IDD_COPY_PROGRESS,owner),job_(job),title_(title),hidden_(hidden),keepWindow_(keepWindow),storageOnly_(storageOnly),cancellable_(cancellable),owner_(owner?owner->GetSafeHwnd():nullptr) {
  if(!hidden_){HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);uninitialize_=SUCCEEDED(init);
   if((SUCCEEDED(init)||init==RPC_E_CHANGED_MODE)&&SUCCEEDED(CoCreateInstance(CLSID_TaskbarList,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&taskbar_)))&&FAILED(taskbar_->HrInit())){taskbar_->Release();taskbar_=nullptr;}}
 }
 ~JobDialog(){state(TBPF_NOPROGRESS);if(taskbar_)taskbar_->Release();if(uninitialize_)CoUninitialize();}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();SetWindowText(title_);if(!cancellable_){if(auto cancel=GetDlgItem(IDCANCEL))cancel->ShowWindow(SW_HIDE);}
  if(storageOnly_){spinner_.SubclassDlgItem(IDC_SW_SPINNER,this);spinner_.Start();}
#ifdef HFV_TESTING
  extern void ProbeStorageSpinner(CWnd*);
  if(storageOnly_&&hidden_)ProbeStorageSpinner(this);
  ++storageUiTest.dialogCreations;
  if(!storageOnly_)storageUiTest.copyBarCount=(GetDlgItem(IDC_CP_BAR)?1:0)+(GetDlgItem(IDC_CP_FILE_BAR)?1:0);
  if(storageOnly_){
   unsigned children=0;for(HWND child=::GetWindow(m_hWnd,GW_CHILD);child;child=::GetWindow(child,GW_HWNDNEXT))++children;
   storageUiTest.simpleStorageLayout=children==3&&GetDlgItem(IDC_CP_PHASE)&&GetDlgItem(IDC_SW_SPINNER)&&GetDlgItem(IDC_SW_EXPLANATION)&&!GetDlgItem(IDC_CP_BAR)&&!GetDlgItem(IDC_CP_FILE)&&!GetDlgItem(IDC_CP_FILE_BAR)&&!GetDlgItem(IDC_CP_STATUS);
  }
#endif
  DialogClose::SetKind(m_hWnd,cancellable_?DialogClose::Kind::Copy:DialogClose::Kind::Dialog);
  auto menu=GetSystemMenu(FALSE);if(menu)menu->EnableMenuItem(SC_CLOSE,MF_BYCOMMAND|((modeless_||cancellable_)?MF_ENABLED:MF_GRAYED));
  if(modeless_&&GetDlgItem(IDC_CP_HINT))
   SetDlgItemText(IDC_CP_HINT,mutation_?"You can keep browsing while this operation runs.":"You can keep browsing while files are copied.");
  if(GetDlgItem(IDC_CP_PATH_LABEL)){
   if(imageDestination_.empty()){
    SetDlgItemText(IDC_CP_PATH_LABEL,"Current item");
    SetDlgItemText(IDC_CP_IMAGE_LABEL,"Progress");
   }
   else{
    BrowserPresentation::UnicodeControl(m_hWnd,IDC_CP_FILE,L"STATIC");
    SetDlgItemTextW(m_hWnd,IDC_CP_FILE,imageDestination_.c_str());
   }
  }
  if(!storageOnly_)GetDlgItem(IDC_CP_BAR)->SendMessage(PBM_SETMARQUEE,TRUE,40);
  if(GetDlgItem(IDC_CP_FILE_BAR))((CProgressCtrl*)GetDlgItem(IDC_CP_FILE_BAR))->SetRange32(0,1000);update();SetTimer(1,50,nullptr);return TRUE;
 }
 int WaitForJob(const std::shared_ptr<Storage::Job>& job) {
  job_=job;update();SetTimer(1,50,nullptr);
  if(job_->done())PostMessage(WM_APP+94,0,0);
  return RunModalLoop(MLF_NOKICKIDLE);
 }
 void OnCancel() override {if(modeless_&&completed_){DestroyWindow();return;}if(cancellable_){if(modeless_)cancelled_();else job_->cancel();GetDlgItem(IDCANCEL)->EnableWindow(FALSE);SetDlgItemText(IDCANCEL,"Cancelling...");}}
 void OnOK() override {}
 bool Dispatching()const{return dispatchDepth_!=0;}
 LRESULT WindowProc(UINT message,WPARAM w,LPARAM l) override {
  struct Dispatch{unsigned& depth;explicit Dispatch(unsigned& n):depth(n){++depth;}~Dispatch(){--depth;}} dispatch(dispatchDepth_);
  if(message==WM_DESTROY)PrepareToClose();
  if(message==WM_CLOSE){if(modeless_||cancellable_)OnCancel();return 0;}
  if(message==WM_APP+94){
   if(keepWindow_&&job_->done()){KillTimer(1);EndModalLoop(IDOK);}
   return 0;
  }
#ifdef HFV_TESTING
  if(hidden_&&message==WM_WINDOWPOSCHANGING)reinterpret_cast<WINDOWPOS*>(l)->flags&=~SWP_SHOWWINDOW;
#endif
  if(message==WM_TIMER&&w==1){
   if(completed_||handlingTimer_)return 0;handlingTimer_=true;struct Timer{bool& active;~Timer(){active=false;}}timer{handlingTimer_};
#ifdef HFV_TESTING
   if(hidden_&&cancellable_&&storageUiTest.cancelOnTimer&&!job_->cancelled())OnCancel();
#endif
#ifdef HFV_TESTING
   ++storageUiTest.timerTicks;
   if(storageUiTest.releaseAfterTwoTimers&&storageUiTest.timerTicks>=2)SetEvent(storageUiTest.releaseAfterTwoTimers);
   if(owner_&&!::IsWindowEnabled(owner_))storageUiTest.disabledDuring=true;
#endif
   update();
   const auto promptState=job_->conflictState();
   if(!promptState.path.empty()){
    KillTimer(1);
    auto answer = Storage::ConflictChoice::None;
#ifdef HFV_TESTING
    if(hidden_)answer=!modeless_&&storageUiTest.answer==Storage::ConflictChoice::None?Storage::ConflictChoice::Cancel:storageUiTest.answer;
    else
#endif
    {
     ConflictDialog prompt(promptState,this,job_->progress().utf8,modeless_?job_:nullptr);INT_PTR choice=prompt.DoModal();
     answer=choice==IDC_REPLACE?Storage::ConflictChoice::Replace:choice==IDC_REPLACE_ALL?Storage::ConflictChoice::ReplaceAll:choice==IDC_SKIP?Storage::ConflictChoice::Skip:Storage::ConflictChoice::Cancel;
    }
    if(modeless_)resolve_(promptState.id,answer);
    else if(answer!=Storage::ConflictChoice::None)job_->resolve(promptState.id,answer);
    SetTimer(1,50,nullptr);
   }
   if(modeless_||job_->done()){
    if(modeless_){
     try{if(advance_&&advance_())return 0;}catch(const std::exception& error){failed_(error.what());}
     KillTimer(1);state(TBPF_NOPROGRESS);completed_=true;DialogClose::SetKind(m_hWnd,DialogClose::Kind::Dialog);std::string displayError;
     Storage::Result result=job_->result();
     try{result=finished_();}catch(CException* error){char text[512]{};error->GetErrorMessage(text,ARRAYSIZE(text));displayError=text[0]?text:"MFC completion error";error->Delete();}catch(const std::exception& error){displayError=error.what();if(displayError.empty())displayError="Completion callback failed";}catch(...){displayError="Unknown completion error";}
     if(!GetSafeHwnd()||!IsWindow(m_hWnd))return 0;
     if(!displayError.empty()){auto status=operation_+(result.code?" failed; completion display failed":" completed; completion display failed");SetDlgItemText(IDC_CP_PHASE,status.c_str());ResultDetails(result.message.empty()?displayError:result.message+"; "+displayError);SetDlgItemText(IDCANCEL,"Close");GetDlgItem(IDCANCEL)->EnableWindow(TRUE);GetDlgItem(IDCANCEL)->ShowWindow(SW_SHOW);if(!hidden_)ShowWindow(SW_SHOWNOACTIVATE);return 0;}
     if(dismissOnCompletion_||(result.outcome==Storage::Outcome::Succeeded&&successMessage_.empty())){DestroyWindow();return 0;}
     const bool succeeded=result.outcome==Storage::Outcome::Succeeded;
     const char* ending=succeeded?" completed":result.outcome==Storage::Outcome::Cancelled ? " cancelled" :
                       result.outcome==Storage::Outcome::Failed ? " failed" : " finished with skipped files";
     auto status=operation_+ending;
     if(result.partial)status+=" (partial completion)";
     SetDlgItemText(IDC_CP_PHASE,status.c_str());
     auto detail=succeeded?successMessage_:result.message;
     auto append=[&](const char* message){if(!detail.empty())detail+=" ";detail+=message;};
     if(result.partial)append("Completed changes were retained.");
     switch(result.rollback){
     case Storage::Rollback::Restored: append("The failed change was rolled back."); break;
     case Storage::Rollback::Failed: append("Rollback failed. Inspect the destination before retrying."); break;
     case Storage::Rollback::Unverified: append("The final storage state could not be verified."); break;
     default: break;
     }
     ResultDetails(detail);SetDlgItemText(IDCANCEL,"Close");GetDlgItem(IDCANCEL)->EnableWindow(TRUE);GetDlgItem(IDCANCEL)->ShowWindow(SW_SHOW);if(!hidden_)ShowWindow(SW_SHOWNOACTIVATE);return 0;
    }
    KillTimer(1);if(keepWindow_)EndModalLoop(IDOK);else{state(TBPF_NOPROGRESS);PrepareToClose();EndDialog(IDOK);}}
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
 std::vector<HWND> otherBrowsers_;
public:
 explicit OwnerRecovery(CWnd* owner):window_(owner?owner->GetSafeHwnd():nullptr),focus_(GetFocus()),enabled_(window_&&IsWindowEnabled(window_)){
  // Storage is still serialized. Disable the other real browser frames too:
  // otherwise a tree control could move its highlight while its read is busy.
  auto app=static_cast<CHFVExplorerApp*>(AfxGetApp());
  for(auto frame:app->m_browsers){auto window=frame->GetSafeHwnd();if(window!=window_&&IsWindowEnabled(window))otherBrowsers_.push_back(window);}
  for(auto window:otherBrowsers_)EnableWindow(window,FALSE);
 }
 void disable(){if(enabled_&&IsWindow(window_))EnableWindow(window_,FALSE);}
 void restore(HWND dialog=nullptr){
  if(restored_)return;restored_=true;
  for(auto window:otherBrowsers_)if(IsWindow(window))EnableWindow(window,TRUE);
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
unsigned actionDepth=0,quietRefreshDepth=0;
void quietWait(const std::shared_ptr<Storage::Job>& job){
 struct Lifetime{std::shared_ptr<Storage::Job> job;~Lifetime(){job->wait();}}lifetime{job};
 while(WaitForSingleObject(job->event(),10)!=WAIT_OBJECT_0){
  // Do not run input commands against a listing whose snapshot is in flight.
  // Leave input/quit messages queued for the normal loop after the read.
  for(UINT type:{UINT(WM_PAINT),UINT(WM_TIMER),UINT(VolumeCoord::Notification),UINT(WM_NULL)}){
   MSG message;while(PeekMessage(&message,nullptr,type,type,PM_REMOVE))DispatchMessage(&message);
  }
 }
}

std::unique_ptr<NativeSession> nativeSession;
void wait(const std::shared_ptr<Storage::Job>& job,const char* title,bool hidden,bool storageOnly=false,bool singleFile=false,bool cancellable=false,bool retainNative=true,CWnd* explicitOwner=nullptr,bool fileLayout=false) {
 INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
 ActiveUiJob active;
 if(quietRefreshDepth&&storageOnly){quietWait(job);return;}
 CWnd* owner=explicitOwner?explicitOwner:AfxGetMainWnd();
 if(actionDepth&&storageOnly&&retainNative){
  if(!nativeSession)nativeSession.reset(new NativeSession(job,title,hidden,owner));
  nativeSession->wait(job);return;
 }
 if(!storageOnly||!retainNative)nativeSession.reset();
 OwnerRecovery restore(owner);
 struct Lifetime{
  std::shared_ptr<Storage::Job> job;
  ~Lifetime(){if(!job->done()){job->cancel();job->wait();}}
 } lifetime={job};
 JobDialog dialog(job,title,hidden,owner,false,storageOnly,singleFile,cancellable,fileLayout);
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
namespace BackgroundCopies {
#ifdef HFV_TESTING
bool enabled=false;
Storage::Result lastResult;
Storage::Result LastResult(){return lastResult;}
#endif
namespace {
std::atomic<unsigned> active{0};
class ModeDialog:public CDynCopyModeSelect {
 bool hidden_;
public:
 bool ready=false,accepted=false;
 BOOL OnInitDialog()override{BOOL result=CDynCopyModeSelect::OnInitDialog();DialogClose::SetKind(m_hWnd,DialogClose::Kind::CopyMode);return result;}
 ModeDialog(bool hidden):CDynCopyModeSelect(CWnd::GetDesktopWindow()),hidden_(hidden){}
 void OnOK()override{if(!UpdateData(TRUE))return;accepted=ready=true;DestroyWindow();}
 void OnCancel()override{ready=true;DestroyWindow();}
 LRESULT WindowProc(UINT message,WPARAM w,LPARAM l)override{
#ifdef HFV_TESTING
  if(hidden_&&message==WM_WINDOWPOSCHANGING)reinterpret_cast<WINDOWPOS*>(l)->flags&=~SWP_SHOWWINDOW;
#endif
  if(message==WM_CLOSE){OnCancel();return 0;}
  return CDynCopyModeSelect::WindowProc(message,w,l);
 }
};
int lastMode[2]={0,0};int lastAd=-1;
struct Batch {
 JobController controller;
 std::unique_ptr<JobDialog> dialog;
 std::unique_ptr<ModeDialog> mode;
 bool hidden = false;

 Batch(std::vector<FileOperations::Request> requests, std::function<void(bool)> completed,
       std::function<void(const FileOperations::Completion&)> observed)
     : controller(std::move(requests), std::move(completed), std::move(observed)) {}

 Batch(JobController::Work work,StorageCompletion completed)
     : controller(std::move(work),std::move(completed)) {}

 bool Advance() {
  if (mode) {
   if (!mode->ready)
    return true;
   if (mode->accepted) {
    int direction = mode->m_outcopy ? 1 : 0;
    lastMode[direction] = mode->m_copy_mode;
    lastAd = mode->m_appledouble != FALSE;
    controller.ChooseMode({mode->m_copy_mode, lastAd != 0, mode->m_said_ok_all != 0});
   } else {
    controller.Cancel();
    dialog->DismissOnCompletion();
   }
   mode.reset();
  }
  switch (controller.Advance()) {
  case JobController::Step::Running:
   return true;
  case JobController::Step::Finished:
   return false;
  case JobController::Step::Started:
   dialog->Next(controller.Job());
   if (!hidden)
    dialog->ShowWindow(SW_SHOWNOACTIVATE);
   return true;
  case JobController::Step::ChooseMode:
   const auto& pending = *controller.PendingMode();
   int direction = pending.destination.IsWindows() ? 1 : 0;
   dialog->Planning();
   mode = std::make_unique<ModeDialog>(hidden);
   mode->m_outcopy = direction;
   mode->m_copy_mode = lastMode[direction];
   mode->m_appledouble = lastAd < 0 ? static_cast<CHFVExplorerApp*>(AfxGetApp())->m_copyin_appledouble : lastAd;
   if(pending.source.IsMounted())
    mode->m_copying_fname=pending.source.expectedName.empty()?L"selected file":MacBrowser::Wide(pending.source.expectedName);
   else if(pending.source.IsWindows())
    mode->m_copying_fname=pending.source.WindowsPath();
   else mode->m_copying_fname=VolumeCoord::Wide(pending.source.ClassicPath());
   if (!mode->Create(IDD_DYNAMIC_COPYMODE_SELECT, CWnd::GetDesktopWindow()))
    throw std::runtime_error("Cannot create copy mode window");
   NativeDpi::CenterPopup(mode->GetSafeHwnd(),dialog->GetSafeHwnd());
   dialog->ShowWindow(SW_HIDE);
   if (!hidden)
    mode->ShowWindow(SW_SHOWNOACTIVATE);
   return true;
  }
  return false;
 }

 void Cancel() {
  controller.Cancel();
  if (mode && mode->GetSafeHwnd())
   mode->OnCancel();
 }

 Storage::Result Finish() {
  if (!controller.Finished()) {
   --active;
   controller.Finish();
#ifdef HFV_TESTING
   lastResult = controller.Result();
#endif
   BrowserLifetime::Collect();
  }
  return controller.Result();
 }
};
std::vector<std::unique_ptr<Batch>> batches;
bool StartBatch(std::unique_ptr<Batch> batch,CWnd* owner,bool single,bool cancellable,const char* title,const char* operation,const char* successMessage=nullptr,const std::wstring& imageDestination={}){
 batches.erase(std::remove_if(batches.begin(),batches.end(),[](const std::unique_ptr<Batch>& b){return b->controller.Finished()&&!b->dialog->GetSafeHwnd()&&!b->dialog->Dispatching();}),batches.end());
 auto ptr=batch.get();
 ++active;
 try{
  bool hidden=false;
#ifdef HFV_TESTING
  hidden=owner&&!owner->IsWindowVisible();
#endif
  // Desktop owner: never disables or binds the lifetime to a browser frame.
  batch->dialog.reset(new JobDialog(std::make_shared<Storage::Job>(),title,hidden,CWnd::GetDesktopWindow(),false,false,single,cancellable,true));
  batch->dialog->Operation(operation);
  batch->dialog->SuccessMessage(successMessage);
  batch->dialog->ImageDestination(imageDestination);
  batch->dialog->ProgressOwner(owner?owner->GetSafeHwnd():nullptr);
  batch->dialog->Background([ptr]{return ptr->Advance();},[ptr]{return ptr->Finish();},
   [ptr]{ptr->Cancel();},
   [ptr](uint64_t id, Storage::ConflictChoice choice){ptr->controller.Resolve(id,choice);},
   [ptr](const char* message){ptr->controller.Fail(message);});
  if(!batch->dialog->Create(!imageDestination.empty()?IDD_CD_IMAGE_PROGRESS:single?IDD_COPY_SINGLE_PROGRESS:IDD_COPY_PROGRESS,CWnd::GetDesktopWindow()))throw std::runtime_error("Cannot create background operation progress");
  NativeDpi::CenterPopup(batch->dialog->GetSafeHwnd(),owner?owner->GetSafeHwnd():nullptr);
  batch->hidden=hidden;batch->dialog->Planning();batch->Advance();
  batches.push_back(std::move(batch));return true;
 }catch(...){if(batch){batch->Cancel();if(auto job=batch->controller.Job())job->wait();}--active;throw;}
}
}
bool Enabled(){
#ifdef HFV_TESTING
 return enabled;
#else
 return true;
#endif
}
bool Active(){return active.load()!=0;}
bool Start(std::vector<FileOperations::Request> requests,CWnd* owner,bool single,std::function<void(bool)> completed,std::function<void(const FileOperations::Completion&)> observed){
 if(requests.empty())return false;
 auto kind=requests.front().OperationKind();
 auto batch=std::make_unique<Batch>(std::move(requests),std::move(completed),std::move(observed));
 bool mutation=kind!=FileOperations::Kind::Copy&&kind!=FileOperations::Kind::LegacyCopy&&kind!=FileOperations::Kind::ExportFork&&kind!=FileOperations::Kind::ReplaceResource;
 const char* title=kind==FileOperations::Kind::CreateHardLink?"Creating hard link":kind==FileOperations::Kind::SetCompression?"Changing file compression":kind==FileOperations::Kind::Rename?"Renaming files":kind==FileOperations::Kind::Delete?"Deleting files":kind==FileOperations::Kind::CreateFolder?"Creating folder":kind==FileOperations::Kind::Inspect?"Reading properties":kind==FileOperations::Kind::SetAttributes?"Updating extended attributes":kind==FileOperations::Kind::ReadAttributeFile?"Reading attribute value":kind==FileOperations::Kind::ExportAttribute?"Exporting attribute":(kind==FileOperations::Kind::SetProperties||kind==FileOperations::Kind::SetBasicMetadata)?"Updating properties":kind==FileOperations::Kind::ReplaceResource?"Importing resource fork":"Copying files";
 const char* operation=kind==FileOperations::Kind::CreateHardLink?"Hard-link creation":kind==FileOperations::Kind::SetCompression?"Compression change":kind==FileOperations::Kind::Rename?"Rename":kind==FileOperations::Kind::Delete?"Delete":kind==FileOperations::Kind::CreateFolder?"Folder creation":kind==FileOperations::Kind::Inspect?"Properties read":kind==FileOperations::Kind::SetAttributes?"Attribute update":kind==FileOperations::Kind::ReadAttributeFile?"Attribute read":kind==FileOperations::Kind::ExportAttribute?"Attribute export":(kind==FileOperations::Kind::SetProperties||kind==FileOperations::Kind::SetBasicMetadata)?"Properties update":kind==FileOperations::Kind::ReplaceResource?"Resource import":"Copy";
 if(mutation)single=true;
 return StartBatch(std::move(batch),owner,single,true,title,operation);
}
bool StartStorage(JobController::Work work,CWnd* owner,const char* title,
                  const char* operation,std::function<void(bool)> completed,StorageJobOptions options){
 return StartStorageResult(std::move(work),owner,title,operation,
  [completed=std::move(completed)](const Storage::Result& result){if(completed)completed(!result.code);},options);
}
bool StartStorageResult(JobController::Work work,CWnd* owner,const char* title,
                        const char* operation,StorageCompletion completed,StorageJobOptions options){
 return StartBatch(std::make_unique<Batch>(std::move(work),std::move(completed)),
                   owner,true,options.cancellable,title,operation,options.successMessage,options.imageDestination);
}
void TransferOwner(HWND closing,HWND replacement){
 for(auto& batch:batches)if(batch->dialog->ProgressOwner()==closing)batch->dialog->ProgressOwner(replacement);
}
void CancelAll(){for(auto& batch:batches)batch->Cancel();}
void Shutdown(){CancelAll();Storage::copyWorker().shutdown();for(auto& b:batches){if(!b->controller.Finished())b->Finish();if(b->dialog->GetSafeHwnd())b->dialog->DestroyWindow();}batches.erase(std::remove_if(batches.begin(),batches.end(),[](const std::unique_ptr<Batch>& b){return !b->dialog->Dispatching();}),batches.end());BrowserLifetime::Collect();}
#ifdef HFV_TESTING
unsigned ProgressOwners(HWND owner){unsigned n=0;for(auto& b:batches)if(!b->controller.Finished()&&b->dialog->ProgressOwner()==owner)++n;return n;}
unsigned TaskbarJobs(HWND owner){unsigned n=0;for(auto& item:taskbarEntries)if(item.second.owner==owner)++n;return n;}
unsigned DialogCount(){unsigned n=0;for(auto& b:batches)if(b->dialog->GetSafeHwnd())++n;return n;}
HWND ModeWindow(){for(auto& b:batches)if(b->mode)return b->mode->GetSafeHwnd();return nullptr;}
HWND DialogWindow(){for(auto& b:batches)if(!b->controller.Finished())return b->dialog->GetSafeHwnd();return nullptr;}
#endif
}
StorageQuietRefresh::StorageQuietRefresh(){++quietRefreshDepth;}
StorageQuietRefresh::~StorageQuietRefresh(){--quietRefreshDepth;}
StorageAction::StorageAction():active_(GetCurrentThreadId()==StorageUi::state().thread&&!StorageUi::busy()){
 if(active_)++actionDepth;
}
StorageAction::~StorageAction(){
 if(active_&&--actionDepth==0){ActiveUiJob closing;nativeSession.reset();}
}
void InitializeStorageUi(){StorageUi::initialize(waitNative);}
Storage::Result ShowStorageJob(std::function<Storage::Result(Storage::Job&)> work,const char* title,bool hidden,bool singleFile,bool cancellable,CWnd* owner) {
 if(StorageUi::busy())return {1,ERROR_BUSY,"Another storage operation is active"};
 auto job=Storage::worker().submit(std::move(work));wait(job,title,hidden,false,singleFile,cancellable,true,owner);return job->result();
}
Storage::Result ShowStorageWaitJob(std::function<Storage::Result(Storage::Job&)> work,bool hidden,CWnd* owner) {
 if(StorageUi::busy())return {1,ERROR_BUSY,"Another storage operation is active"};
 auto job=Storage::worker().submit(std::move(work));wait(job,"Waiting for storage",hidden,true,false,false,false,owner);return job->result();
}
Storage::Result ShowSubmittedStorageJob(const std::shared_ptr<Storage::Job>& job,const char* title,bool hidden,bool singleFile,bool cancellable,CWnd* owner){
 if(!job)return {1,ERROR_INVALID_PARAMETER,"No operation was submitted"};
 // Never leave a conflict-waiting job orphaned if presentation is refused.
 if(StorageUi::busy()){job->cancel();return {1,ERROR_BUSY,"Another storage dialog is active"};}
 wait(job,title,hidden,false,singleFile,cancellable,true,owner,true);return job->result();
}
void ShutdownStorageUi(){
 FileOperations::CancelPending();BackgroundCopies::Shutdown();MetadataDialogs::Shutdown();
 nativeSession.reset();StorageUi::state().wait=nullptr;StorageUi::state().thread=0;
 Storage::navigationWorker().shutdown();
 Storage::worker().shutdown();
}
