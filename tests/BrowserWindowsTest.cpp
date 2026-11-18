// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "VolumeSession.h"
#include "HfsPlusDisk.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "utils.h"
#include <cstdio>
namespace {
CString host;
bool rejectNew=false;
void Check(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
class BrowserWindowTestDoc:public CHFVExplorerDoc {
 DECLARE_DYNCREATE(BrowserWindowTestDoc)
public:
 BrowserWindowTestDoc():CHFVExplorerDoc(false){m_fat_count=1;m_fats[0].m_letter=host[0];m_fats[0].m_type=DRIVE_FIXED;m_fats[0].m_volume_name=host;}
 BOOL OnNewDocument()override{if(rejectNew)return FALSE;return CHFVExplorerDoc::OnNewDocument();}
};
IMPLEMENT_DYNCREATE(BrowserWindowTestDoc,CHFVExplorerDoc)
class BrowserWindowTestFrame:public CMainFrame {
 DECLARE_DYNCREATE(BrowserWindowTestFrame)
public:
 void LoadWindowPosition()override{}
 void SaveWindowPosition()override{}
 void ActivateFrame(int)override{} // Real frames, menus and routing; never show UI.
};
IMPLEMENT_DYNCREATE(BrowserWindowTestFrame,CMainFrame)
int Find(BrowserSession& session,const wchar_t* name){
 auto& list=session.List()->GetListCtrl();
 for(int i=0;i<list.GetItemCount();++i){wchar_t text[512]{};LVITEMW item{};item.pszText=text;item.cchTextMax=ARRAYSIZE(text);SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));if(!wcscmp(text,name))return i;}
 return -1;
}
std::wstring Image(const CString& path,const std::u16string& name){
 int count=MultiByteToWideChar(CP_ACP,0,path,-1,nullptr,0);std::wstring wide(count,L'\0');MultiByteToWideChar(CP_ACP,0,path,-1,&wide[0],count);wide.resize(count-1);
 auto disk=HfsPlus::CreateImage(wide.c_str(),8*1024*1024);HfsPlus::Volume::Format(*disk,name,true);disk.reset();
 auto volume=MacBrowser::OpenImage(wide);volume->MakeDirectory(2,u"One");volume->MakeDirectory(2,u"Two");volume->MakeDirectory(2,u"Cafe \u65e5\u672c");return wide;
}
std::wstring Caption(CWnd* window){wchar_t text[32768]{};GetWindowTextW(window->GetSafeHwnd(),text,ARRAYSIZE(text));return text;}
void NoQuit(){MSG message{};Check(!PeekMessage(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE),"closing a browser posted WM_QUIT with another window open");}
struct MenuState:CCmdUI{bool enabled=false;void Enable(BOOL value=TRUE)override{enabled=value!=FALSE;}};
}
int RunBrowserWindowsTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\browser-windows-report.txt","w");if(!report)return 2;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 int result=0;
 try{
  app.InstallBrowserTemplate(RUNTIME_CLASS(BrowserWindowTestDoc),RUNTIME_CLASS(BrowserWindowTestFrame));
  auto a=app.CreateBrowser(FALSE);Check(a&&app.m_browsers.size()==1&&app.m_pMainWnd==a,"first browser registration");auto& sa=a->GetBrowserSession();
  HWND ah=a->GetSafeHwnd();a->OnUpdateFrameTitle(TRUE);Check(Caption(a)==VolumeCoord::Wide(std::string((LPCSTR)host))+L" \u2014 HFVExplorer","startup Windows folder title was replaced by Untitled");Check(!a->IsWindowVisible(),"test unexpectedly displayed a browser");
  MenuState menu;menu.m_nID=ID_TOOLS_NEW_BROWSER;Check(a->OnCmdMsg(ID_TOOLS_NEW_BROWSER,CN_UPDATE_COMMAND_UI,&menu,nullptr)&&menu.enabled,"New browser has no enabled update handler");
  a->SendMessage(WM_COMMAND,ID_TOOLS_NEW_BROWSER);Check(app.m_browsers.size()==2,"New browser menu did not create an independent window");
  auto b=app.m_browsers.back();auto& sb=b->GetBrowserSession();HWND bh=b->GetSafeHwnd();
  Check(a!=b&&sa.Document()!=sb.Document()&&sa.Tree()!=sb.Tree()&&sa.List()!=sb.List()&&!b->IsWindowVisible(),"browser windows share document/panes or test UI became visible");
  Check(GetWindow(ah,GW_OWNER)==nullptr&&GetWindow(bh,GW_OWNER)==nullptr,"browser was created as an owned window");
  // Deliberately keep app ownership at B, then route commands to inactive A.
  app.ActivateBrowser(b);a->SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);
  Check((sa.List()->GetStyle()&LVS_TYPEMASK)==LVS_REPORT&&(sb.List()->GetStyle()&LVS_TYPEMASK)==LVS_ICON,"view command changed the wrong frame");
  Check(app.ActiveBrowser()==b,"command routing changed MFC's active browser");
  a->SendMessage(WM_COMMAND,ID_VIEW_MAC_WINDOW);Check(sa.finderLayout&&!sb.finderLayout,"Finder layout leaked between windows");
  b->SendMessage(WM_COMMAND,ID_HFV_VIEW_LIST);Check(sa.finderLayout&&(sb.List()->GetStyle()&LVS_TYPEMASK)==LVS_LIST,"second frame command used first frame");
  a->SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);
  auto firstImage=host+"\\first.dsk",secondImage=host+"\\second.dsk";Image(firstImage,u"First");Image(secondImage,u"Second");
  app.InitRecentImagesForTest();
  app.AddToRecentFileList(firstImage);
  a->SendMessage(WM_COMMAND,ID_FILE_MRU_FILE1);
  Check(sa.MacBrowser().Active(),"recent image command did not open in its originating browser");
  Check(!sb.MacBrowser().Active(),"recent image command opened in the other browser");
  app.ActivateBrowser(b);Check(app.OpenDocumentFile(secondImage)==sb.Document(),"image open did not target second browser");
  Check(sa.MacBrowser().Active()&&sb.MacBrowser().Active(),"opening an image cleared another browser");
  a->OnUpdateFrameTitle(TRUE);b->OnUpdateFrameTitle(TRUE);Check(Caption(a)==L"First \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","disk captions leaked between frames or MFC restored Untitled");
  Check(sa.MacBrowser().Activate(Find(sa,L"Cafe \u65e5\u672c")),"open Unicode title folder");Check(Caption(a)==L"First:Cafe \u65e5\u672c \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","Unicode folder caption or frame isolation failed");Check(sa.MacBrowser().ShowTestFolder(0,2),"restore title test root");
  sa.List()->GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);sb.List()->GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);
  app.ActivateBrowser(b);a->SendMessage(WM_COMMAND,ID_EDIT_SELECT_ALL);
  Check(sa.List()->GetListCtrl().GetSelectedCount()==sa.List()->GetListCtrl().GetItemCount()&&sa.List()->GetListCtrl().GetItemCount()>=2&&sb.List()->GetListCtrl().GetSelectedCount()==0,"selection command used another window's list");
  Check(sa.MacBrowser().Activate(Find(sa,L"One"))&&Find(sa,L"One")<0&&Find(sb,L"One")>=0,"native folder navigation leaked between real frames");
  app.ActivateBrowser(b);a->SendMessage(WM_COMMAND,ID_VIEW_REFRESH);
  Check(Find(sa,L"One")<0&&Find(sb,L"One")>=0,"refresh command lost a window's current folder");a->OnUpdateFrameTitle(TRUE);Check(Caption(a)==L"First:One \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","refresh changed the current folder caption");
  bool bothDisabled=false;
  auto wait=ShowStorageJob([&](Storage::Job&){for(int n=0;n<100;++n){if(!IsWindowEnabled(ah)&&!IsWindowEnabled(bh)){bothDisabled=true;break;}Sleep(5);}return Storage::Result{};},"Hidden multiwindow operation",true,true,false,a);
  Check(wait.code==0&&bothDisabled&&a->IsWindowEnabled()&&b->IsWindowEnabled(),"storage progress did not disable/restore all browser frames");
  b->EnableWindow(FALSE);
  wait=ShowStorageJob([](Storage::Job&){Sleep(40);return Storage::Result{};},"Hidden prior disabled owner",true,true,false,a);
  Check(wait.code==0&&a->IsWindowEnabled()&&!b->IsWindowEnabled(),"storage completion enabled a previously disabled browser");b->EnableWindow(TRUE);
  // Busy guards apply to every frame, not just the disabled progress owner.
  ++StorageUi::state().active;a->SendMessage(WM_CLOSE);b->SendMessage(WM_COMMAND,ID_TOOLS_NEW_BROWSER);--StorageUi::state().active;
  Check(IsWindow(ah)&&app.m_browsers.size()==2,"nested close/new command ran during storage access");
  auto previousMain=app.m_pMainWnd;rejectNew=true;Check(app.CreateBrowser(FALSE)==nullptr,"injected failed initialization succeeded");rejectNew=false;
  Check(app.m_browsers.size()==2&&app.m_pMainWnd==previousMain&&IsWindow(ah)&&IsWindow(bh),"failed browser creation damaged existing windows");NoQuit();
  // Exercise the actual resource accelerator, not a duplicate command ID.
  app.ActivateBrowser(a);MSG key{};key.hwnd=ah;key.message=WM_KEYDOWN;key.wParam=VK_RETURN;BYTE prior[256]{},state[256]{};GetKeyboardState(prior);memcpy(state,prior,sizeof state);state[VK_CONTROL]=0x80;SetKeyboardState(state);
  bool translated=TranslateAccelerator(ah,LoadAccelerators(AfxGetResourceHandle(),MAKEINTRESOURCE(IDR_MAINFRAME)),&key)!=0;SetKeyboardState(prior);
  Check(translated&&app.m_browsers.size()==3,"Ctrl+Enter is not wired to New browser");auto c=app.m_browsers.back();HWND ch=c->GetSafeHwnd();
  c->SendMessage(WM_CLOSE);Check(!IsWindow(ch)&&IsWindow(ah)&&IsWindow(bh)&&app.m_browsers.size()==2,"closing the newest window closed another browser");NoQuit();
  const auto sharedPath=VolumeCoord::Wide(std::string((LPCSTR)firstImage));
  Check(sa.MacBrowser().ShowTestFolder(0,2)&&sb.MacBrowser().OpenImage(sharedPath)==1,
        "open one image in both browsers");
  auto firstSelection=sa.MacBrowser().CaptureItem(Find(sa,L"One"));
  auto peerSelection=sb.MacBrowser().CaptureItem(Find(sb,L"One"));
  auto shared=firstSelection.Volume();
  auto pid=VolumeSession::VolumeSessionPidForTest(shared);
  Check(shared==peerSelection.Volume()&&pid&&pid!=GetCurrentProcessId(),
        "the same mounted image did not share a distinct worker process");
  {
   MacBrowser::Entry payload;payload.name=u"Slow payload";payload.forks[0].size=512*1024;
   shared->Put(firstSelection.Id(),payload,[](unsigned,uint64_t,void* out,size_t n){memset(out,0x5a,n);},false);
   MacBrowser::Entry other;
   Check(shared->FindChild(2,u"Two",other),"peer folder fixture");
   MacBrowser::Entry marker;marker.name=u"Peer listing";
   shared->Put(other.id,marker,[](unsigned,uint64_t,void*,size_t){},false);
   auto output=sharedPath.substr(0,sharedPath.find_last_of(L'\\'))+L"\\slow-export";
   Check(CreateDirectoryW(output.c_str(),nullptr)!=FALSE,"slow export fixture folder");

   struct ReadPause {
    HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    HANDLE release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    std::shared_ptr<Storage::Job> job;
    ~ReadPause(){
     VolumeSession::PauseVolumeReadForTest(nullptr,nullptr);
     if(release)SetEvent(release);
     if(job && !job->done()){job->cancel();job->wait();}
     if(release)CloseHandle(release);
     if(entered)CloseHandle(entered);
    }
   } pause;
   Check(pause.entered && pause.release,"make slow export pause");
   VolumeSession::PauseVolumeReadForTest(pause.entered,pause.release);
   FileOperations::Request request=FileOperations::OrdinaryCopy{{firstSelection},FileOperations::Location::Windows(output)};
   request.background=true;
   auto exportJob=FileOperations::Submit(std::move(request));
   pause.job=exportJob.Job();
   Check(WaitForSingleObject(pause.entered,10000)==WAIT_OBJECT_0,"background export did not reach its first read chunk");
   Check(!exportJob.Job()->done() && VolumeCoord::Readable(shared->ResourceKey()) &&
              !VolumeCoord::Available({{shared->ResourceKey(),VolumeCoord::Mode::Write}}),
         "background export did not share reads while excluding writers");
   const auto start=GetTickCount();
   const bool navigated=sb.MacBrowser().Activate(Find(sb,L"Two")) && Find(sb,L"Peer listing")>=0;
   const bool stillCopying=!exportJob.Job()->done();
   VolumeSession::PauseVolumeReadForTest(nullptr,nullptr);
   SetEvent(pause.release);
   Check(navigated && stillCopying && GetTickCount()-start<5000 &&
              VolumeSession::VolumeSessionPidForTest(shared)==pid,
         "the second browser could not list a folder while the same image was exporting");
   Check(WaitForSingleObject(exportJob.Job()->event(),10000)==WAIT_OBJECT_0 &&
              !exportJob.Job()->result().code && exportJob.Snapshot().complete,"slow background export failed");
   {
    auto file=MacBrowser::ResolveWindowsFile(output+L"\\One\\Slow payload");
    auto entry=file.first->Stat(file.second);
    uint8_t first=0,last=0;
    Check(entry.forks[0].size==512*1024,"slow export length changed");
    file.first->Read(file.second,0,0,&first,1);
    file.first->Read(file.second,0,entry.forks[0].size-1,&last,1);
    Check(first==0x5a && last==0x5a,"slow export content changed");
   }
   Check(sb.MacBrowser().ShowTestFolder(1,2),"restore second browser root");
   fprintf(report,"PASS: second hidden browser navigates the same mounted image during a paused export, with writers excluded and worker PID unchanged.\n");
  }
  {
   Check(sa.MacBrowser().Activate(Find(sa,L"One")) && Find(sa,L"Slow payload")>=0,
         "first browser did not cache the import destination");
   const auto directory=sharedPath.substr(0,sharedPath.find_last_of(L'\\'));
   const auto filename=directory+L"\\Import payload";
   auto seedWindows=[&](uint8_t byte,bool replace){
    auto source=MacBrowser::OpenWindows(directory);
    MacBrowser::Entry entry;entry.name=u"Import payload";entry.forks[0].size=512*1024;
    source->Put(2,entry,[&](unsigned,uint64_t,void* out,size_t n){memset(out,byte,n);},replace);
   };
   seedWindows(0x31,false);
   shared->MakeDirectory(2,u"Uncached");
   auto status=[&]{CString text;sb.Status()->GetPaneText(0,text);return text;};
   struct WritePause {
    HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr),release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    std::shared_ptr<Storage::Job> job;
    ~WritePause(){
     VolumeSession::PauseVolumeWriteForTest(nullptr,nullptr);
     if(release)SetEvent(release);
     if(job&&!job->done()){job->cancel();job->wait();}
     if(entered)CloseHandle(entered);
     if(release)CloseHandle(release);
    }
   };
   for(bool cancel:{false,true}){
    if(cancel)seedWindows(0x42,true);
    Check(sb.MacBrowser().ShowTestFolder(1,2),"return the peer browser to the root");
    WritePause pause;
    Check(pause.entered&&pause.release,"create import pause");
    VolumeSession::PauseVolumeWriteForTest(pause.entered,pause.release);
    FileOperations::Request request=FileOperations::OrdinaryCopy{{FileOperations::Location::Windows(filename)},firstSelection};
    request.background=true;
    auto imported=FileOperations::Submit(std::move(request));
    pause.job=imported.Job();
    if(cancel){
     const auto deadline=GetTickCount()+10000;
     while(pause.job->conflict().empty()&&!pause.job->done()&&LONG(deadline-GetTickCount())>0)Sleep(1);
     Check(!pause.job->conflict().empty(),"overwrite conflict missing");
     pause.job->resolve(Storage::ConflictChoice::Replace);
    }
    Check(WaitForSingleObject(pause.entered,10000)==WAIT_OBJECT_0&&!VolumeCoord::Readable(shared->ResourceKey()),
          "import did not pause with exclusive image access");
    if(!cancel){
     Check(sb.MacBrowser().Activate(Find(sb,L"Uncached"))&&status().Find("Waiting for")>=0&&!pause.job->done(),
           "uncached folder did not wait for the import");
     Check(sb.MacBrowser().ShowTestFolder(1,2),"cached root could not replace the deferred navigation");
    }
    const auto priorRefresh=sa.notificationRefreshes;
    Check(sb.MacBrowser().Activate(Find(sb,L"One"))&&Find(sb,L"Slow payload")>=0&&
              status().Find("Showing the last folder listing")>=0&&!sb.MacBrowser().Writable()&&
              (Find(sb,L"Import payload")>=0)==cancel&&!pause.job->done(),
          "peer did not display the other browser's committed listing during import");
    if(cancel)pause.job->cancel();
    VolumeSession::PauseVolumeWriteForTest(nullptr,nullptr);
    SetEvent(pause.release);
    Check(WaitForSingleObject(pause.job->event(),10000)==WAIT_OBJECT_0&&
              (cancel?pause.job->result().error==ERROR_CANCELLED:!pause.job->result().code),
          "paused import completed with an unexpected result");
    const auto deadline=GetTickCount()+10000;
    while((Find(sa,L"Import payload")<0||Find(sb,L"Import payload")<0||
           sa.notificationRefreshes==priorRefresh||!sb.MacBrowser().Writable())&&LONG(deadline-GetTickCount())>0){
     MSG message{};while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessage(&message);}
     Sleep(1);
    }
    MacBrowser::Entry importedFile;
    Check(Find(sa,L"Import payload")>=0&&Find(sb,L"Import payload")>=0&&
              sa.notificationRefreshes>priorRefresh&&sb.MacBrowser().Writable()&&
              shared->FindChild(firstSelection.Id(),u"Import payload",importedFile),"both cached views did not refresh");
    uint8_t sample=0;shared->Read(importedFile.id,0,0,&sample,1);
    Check(sample==0x31&&VolumeSession::VolumeSessionPidForTest(shared)==pid,
          "cancelled overwrite changed the original or restarted the worker");
   }
   Check(sa.MacBrowser().ShowTestFolder(0,2)&&sb.MacBrowser().ShowTestFolder(1,2),"restore roots after import test");
   fprintf(report,"PASS: shared cached listing during paused import and cancelled overwrite, with refresh in both browsers\n");
  }
  auto renamed=FileOperations::Submit(FileOperations::RenameEntry{firstSelection,u"Renamed"});
  Check(WaitForSingleObject(renamed.Job()->event(),10000)==WAIT_OBJECT_0&&
        !renamed.Job()->result().code,"rename on persistent session failed");
  const auto deadline=GetTickCount()+10000;
  while((Find(sa,L"Renamed")<0||Find(sb,L"Renamed")<0)&&LONG(deadline-GetTickCount())>0) {
   MSG message{};
   while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessage(&message);}
   Sleep(1);
  }
  Check(Find(sa,L"Renamed")>=0&&Find(sb,L"Renamed")>=0&&VolumeSession::VolumeSessionPidForTest(shared)==pid,
        "rename did not refresh both browsers within the same worker session");
  auto oldSelection=sa.MacBrowser().CaptureItem(Find(sa,L"Renamed"));
  VolumeSession::CrashVolumeSessionOnRenameForTest(shared);
  auto refused=FileOperations::Submit(FileOperations::RenameEntry{oldSelection,u"Committed before crash"});
  Check(WaitForSingleObject(refused.Job()->event(),10000)==WAIT_OBJECT_0&&refused.Job()->result().code&&
        refused.Job()->result().rollback==Storage::Rollback::Unverified,
        "lost rename reply was not reported as unverified");
  Check(sa.MacBrowser().Refresh()&&sb.MacBrowser().Refresh()&&Find(sa,L"Committed before crash")>=0&&
        Find(sb,L"Committed before crash")>=0&&VolumeSession::VolumeSessionPidForTest(shared)!=pid&&shared->Writable(),
        "refresh did not recover both browsers after a worker crash");
  oldSelection.expectedName.clear();
  auto stale=FileOperations::Submit(FileOperations::RenameEntry{oldSelection,u"Stale rename"});
  Check(WaitForSingleObject(stale.Job()->event(),10000)==WAIT_OBJECT_0&&stale.Job()->result().code,
        "selection from the crashed worker was accepted");
  auto fresh=sa.MacBrowser().CaptureItem(Find(sa,L"Committed before crash"));
  auto retry=FileOperations::Submit(FileOperations::RenameEntry{fresh,u"Recovered rename"});
  Check(WaitForSingleObject(retry.Job()->event(),10000)==WAIT_OBJECT_0&&!retry.Job()->result().code&&
        sa.MacBrowser().Refresh()&&Find(sa,L"Recovered rename")>=0,
        "fresh selection could not write after journal recovery");
  fprintf(report,"PASS: two browsers share one persistent worker, rename refreshes both without remounting, "
                 "worker crash requires refresh, stale selections are rejected and fresh writes succeed.\n");
  // Make the first frame the MFC main window before closing it: this is the
  // case that CFrameWnd's default SDI shutdown used to close every document.
  app.ActivateBrowser(a);a->SendMessage(WM_CLOSE);
  Check(!IsWindow(ah)&&IsWindow(bh)&&app.m_browsers.size()==1&&app.m_pMainWnd==b&&app.ActiveBrowser()==b,"first window closure did not transfer ownership to the surviving browser");NoQuit();
  b->SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);Check((sb.List()->GetStyle()&LVS_TYPEMASK)==LVS_REPORT&&sb.MacBrowser().Refresh(),"remaining window is not usable after first closes");
  b->SendMessage(WM_COMMAND,ID_TOOLS_NEW_BROWSER);Check(app.m_browsers.size()==2,"remaining window cannot create another browser");
  // Explicit File/Exit closes the full collection; a title-bar close does not.
  b->SendMessage(WM_COMMAND,ID_APP_EXIT);Check(app.m_browsers.empty()&&!IsWindow(bh)&&app.m_pMainWnd==nullptr&&app.ActiveBrowser()==nullptr,"File Exit did not close the remaining browser collection");
  MSG quit{};Check(PeekMessage(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)!=FALSE,"last window did not terminate the message loop");
  fprintf(report,"PASS: independent real hidden top-level frames/documents/panes; New browser menu and Ctrl+Enter; view/layout and image-open command routing; per-window disk/folder and Unicode captions, startup and MFC title-update persistence; busy/failed-creation guards; closing newest and first/main windows without WM_QUIT; surviving browser refresh/new commands; explicit File Exit and final-window shutdown. No hardware access or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());result=1;}
 catch(CException* e){char text[512]{};e->GetErrorMessage(text,ARRAYSIZE(text));fprintf(report,"FAIL: MFC exception: %s\n",text);e->Delete();result=1;}
 fclose(report);return result;
}
#endif
