// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "HFVPreviewView.h"
#include "BrowserPresentation.h"
#include "MainMacBrowser.h"
#include "VolumeSession.h"
#include "HfsPlusDisk.h"
#include "ApfsContainer.h"
#include "ApfsVolume.h"
#include "ApfsFormat.h"
#include "StorageJobDialog.h"
#include "BackgroundCopies.h"
#include "MetadataDialogs.h"
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
 BrowserWindowTestDoc():CHFVExplorerDoc(false){Mounts().AddWindows(host[0],DRIVE_FIXED,host);}
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
 auto volume=MacBrowser::OpenImage(wide);volume->MakeDirectory(2,u"One");volume->MakeDirectory(2,u"Two");volume->MakeDirectory(2,u"Cafe \u65e5\u672c");
 if(name==u"First"){
  MacBrowser::Entry entry;entry.name=u"Read Me";entry.forks[0].size=5;
  memcpy(entry.finder.data(),"TEXTttxt",8);
  const char bytes[]={'C','a','f',char(0x8e),'\r'};
  volume->Put(2,entry,[&](unsigned,uint64_t at,void* out,size_t count){memcpy(out,bytes+at,count);},false);
 }
 return wide;
}
std::wstring Caption(CWnd* window){wchar_t text[32768]{};GetWindowTextW(window->GetSafeHwnd(),text,ARRAYSIZE(text));return text;}
void CheckDpiAwareness(HWND frame) {
 Check(IsProcessDPIAware()!=FALSE,"shipping manifest did not enable DPI awareness");
 using Query=HANDLE(WINAPI*)(HWND);
 using Equal=BOOL(WINAPI*)(HANDLE,HANDLE);
 using Valid=BOOL(WINAPI*)(HANDLE);
 using Set=HANDLE(WINAPI*)(HANDLE);
 const auto user=GetModuleHandleW(L"user32.dll");
 const auto query=reinterpret_cast<Query>(GetProcAddress(user,"GetWindowDpiAwarenessContext"));
 const auto equal=reinterpret_cast<Equal>(GetProcAddress(user,"AreDpiAwarenessContextsEqual"));
 const auto valid=reinterpret_cast<Valid>(GetProcAddress(user,"IsValidDpiAwarenessContext"));
 const auto set=reinterpret_cast<Set>(GetProcAddress(user,"SetThreadDpiAwarenessContext"));
 const auto perMonitorV2=reinterpret_cast<HANDLE>(INT_PTR(-4));
 const auto systemAware=reinterpret_cast<HANDLE>(INT_PTR(-2));
 if(!query||!equal||!valid||!valid(perMonitorV2)){
  Check(NativeDpi::Window(frame)==NativeDpi::System(),"legacy system-DPI fallback");
  if(query&&equal)Check(equal(query(frame),systemAware)!=FALSE,"legacy window is not system-DPI aware");
  return;
 }
 Check(equal(query(frame),perMonitorV2)!=FALSE,"shipping frame did not enter PMv2");
 Check(set!=nullptr,"system-DPI context unavailable");
 const auto previous=set(systemAware);
 Check(previous!=nullptr,"could not enter system-DPI context");
 const auto legacy=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,1,1,nullptr,nullptr,nullptr,nullptr);
 const auto systemDpi=NativeDpi::System();
 set(previous);
 const bool fallback=legacy&&equal(query(legacy),systemAware)&&NativeDpi::Window(legacy)==systemDpi;
 if(legacy)DestroyWindow(legacy);
 Check(fallback,"system-aware window did not use the system DPI");
}
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
  Check(sa.AwaitNavigationForTest(),"startup folder completion");
  HWND ah=a->GetSafeHwnd();
  CheckDpiAwareness(ah);
  auto* tree = sa.Tree();
  const auto windowsRoot = tree->GetTreeCtrl().GetRootItem();
  const auto* root = tree->BrowserNodeAt(windowsRoot);
  Check(root && root->Folder() && root->Folder()->location.IsWindows(), "Windows root lacks a typed location");
  auto path = root->Folder()->location.WindowsPath();
  if (path.back() != L'\\') path += L'\\';
  path += std::wstring(280, L'x') + L"\u65e5";
  const auto child = tree->InsertWindowsFolder(windowsRoot, root->mount.index, L"DPI fixture",
      FileOperations::Location::Windows(path), true);
  Check(child && tree->FindWindowsFolder(windowsRoot, path) == child, "tree lost a long Unicode path");
  char legacy[8] = {};
  const auto data = tree->GetTreeCtrl().GetItemData(child);
  Check(!tree->get_lparam_path(data, legacy, sizeof(legacy)) && !legacy[0],
        "legacy path adapter truncated a typed folder");
  tree->GetTreeCtrl().DeleteItem(child);
  Check(!tree->BrowserNode(data), "deleted tree binding remained accessible");
  {
   auto& inventory=sa.Mounts();
   BrowserMountInventory::ClassicMount removable;
   removable.m_file_name=host+"\\reused.dsk";
   const auto index=inventory.AddClassic(removable);
   const auto address=BrowserMountInventory::Address{BrowserMountInventory::Kind::ClassicVolume,index};
   const auto oldRoot=tree->initialize_volume(index,"Temporary classic",2,0,0,0,0);
   sa.SetMountRoot(address,oldRoot);
   const auto oldNode=tree->BrowserNodeAt(oldRoot);
   const auto oldMount=inventory.Lookup(address);
   Check(oldNode&&oldMount&&inventory.IsCurrent(*oldMount),"initial mount was not bound to the tree");
   const auto handle=oldNode->mount;
   inventory.RemoveClassic(index);
   Check(inventory.AddClassic(removable)==index,"classic inventory did not reuse the free slot");
   sa.SetMountRoot(handle,oldRoot); // A stale mount cannot bind a tree root.
   Check(!inventory.IsCurrent(*oldMount)&&!inventory.Lookup(handle)&&!tree->BrowserNodeAt(oldRoot),
        "reuse revived a stale navigation or tree binding");
   tree->GetTreeCtrl().DeleteItem(oldRoot);
   Check(!sa.MountRoot(address)&&inventory.Lookup(address),"deleting a root changed its mount or kept a dead tree handle");
   inventory.RemoveClassic(index);
  }
  fprintf(report,"PASS: executable DPI awareness, system-DPI fallback and stale mount handles.\n");
  a->OnUpdateFrameTitle(TRUE);Check(Caption(a)==VolumeCoord::Wide(std::string((LPCSTR)host))+L" \u2014 HFVExplorer","startup Windows folder title was replaced by Untitled");Check(!a->IsWindowVisible(),"test unexpectedly displayed a browser");
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
  app.SeedRecentImageForTest(firstImage);
  a->SendMessage(WM_COMMAND,ID_FILE_MRU_FILE1);
  Check(sa.AwaitNavigationForTest()&&sa.MacBrowser().Active(),"recent image command did not open in its originating browser");
  Check(!sb.MacBrowser().Active(),"recent image command opened in the other browser");
  app.ActivateBrowser(b);Check(app.OpenDocumentFile(secondImage)==sb.Document(),"image open did not target second browser");
  Check(sb.AwaitNavigationForTest()&&sa.MacBrowser().Active()&&sb.MacBrowser().Active(),"opening an image cleared another browser");
  Check(!sa.Preview()->Enabled()&&!sb.Preview()->Enabled(),"preview should start hidden");
  a->SendMessage(WM_COMMAND,ID_VIEW_PREVIEW_PANE);
  Check(sa.Preview()->Enabled()&&(sa.Preview()->GetStyle()&WS_VISIBLE)&&!sb.Preview()->Enabled(),
        "preview command changed the wrong frame");
  const int previewItem=Find(sa,L"Read Me");
  Check(previewItem>=0,"preview fixture is absent from the typed Mac listing");
  sa.List()->GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);
  sa.List()->GetListCtrl().SetItemState(previewItem,LVIS_SELECTED,LVIS_SELECTED);
  const HWND pane=sa.Preview()->GetSafeHwnd();
  const HWND edit=FindWindowExW(pane,nullptr,L"EDIT",nullptr);
  bool previewReady=false;
  for(int n=0;n<500&&!previewReady;++n){
   MSG message{};
   while(PeekMessage(&message,pane,WM_TIMER,WM_TIMER,PM_REMOVE))DispatchMessage(&message);
   previewReady=BrowserPresentation::Text(edit)==L"Caf\u00e9\r\n";
   if(!previewReady)Sleep(10);
  }
  Check(previewReady,"selection notification did not publish the native text preview");
  auto& details=sa.List()->GetListCtrl();
  const auto otherFont=sb.List()->GetListCtrl().SendMessage(WM_GETFONT);
  RECT original{}, suggested{};
  a->GetWindowRect(&original);
  suggested=original;
  suggested.right+=25;
  suggested.bottom+=15;
  auto change=[&](UINT dpi,const RECT& rectangle){
   a->SendMessage(NativeDpi::Changed,MAKEWPARAM(dpi,dpi),LPARAM(&rectangle));
  };
  auto listIconSize=[](CListCtrl& control, int kind) {
   int cx=0,cy=0;
   const auto handle=reinterpret_cast<HIMAGELIST>(control.SendMessage(LVM_GETIMAGELIST,kind));
   ImageList_GetIconSize(handle,&cx,&cy);
   return CSize(cx,cy);
  };
  auto* toolbar=a->GetControlBar(AFX_IDW_TOOLBAR);
  Check(toolbar!=nullptr,"browser toolbar was not created");
  const auto previewButton=FindWindowExW(pane,nullptr,L"BUTTON",nullptr);
  change(96,original);
  const auto button96=toolbar->SendMessage(TB_GETBUTTONSIZE);
  Check(listIconSize(details,LVSIL_NORMAL)==CSize(32,32),"baseline browser icon size");
  const int originalWidth=details.GetColumnWidth(0);
  LOGFONT font96{},font144{};
  GetObject(HGDIOBJ(details.SendMessage(WM_GETFONT)),sizeof(font96),&font96);
  change(144,suggested);
  GetObject(HGDIOBJ(details.SendMessage(WM_GETFONT)),sizeof(font144),&font144);
  Check(font96.lfHeight<0&&font144.lfHeight==NativeDpi::Scale(font96.lfHeight,144),"list font at 150 percent");
  RECT actual{};
  a->GetWindowRect(&actual);
  Check(EqualRect(&actual,&suggested),"frame ignored the DPI rectangle");
  Check(details.GetColumnWidth(0)==NativeDpi::Scale(originalWidth,144),"details width at 150 percent");
  Check(listIconSize(details,LVSIL_NORMAL)==CSize(48,48)&&listIconSize(details,LVSIL_SMALL)==CSize(24,24),
        "browser image lists at 150 percent");
  RECT previewBounds{};
  Check(::GetWindowRect(previewButton,&previewBounds)&&previewBounds.right-previewBounds.left==72,
        "preview button at 150 percent");
  const auto button144=toolbar->SendMessage(TB_GETBUTTONSIZE);
  Check(LOWORD(button144)>LOWORD(button96)&&HIWORD(button144)>HIWORD(button96),"toolbar buttons did not scale");
  Check(sb.List()->GetListCtrl().SendMessage(WM_GETFONT)==otherFont,"DPI change affected another browser");
  Check(details.GetItemState(previewItem,LVIS_SELECTED)!=0&&BrowserPresentation::Text(edit)==L"Caf\u00e9\r\n",
        "DPI change discarded a selected row or its preview");
  details.SetColumnWidth(0,253);
  for(int repeat=0;repeat<3;++repeat){
   change(192,suggested);
   Check(listIconSize(details,LVSIL_NORMAL)==CSize(64,64),"browser image list at 200 percent");
   Check(details.GetColumnWidth(0)==NativeDpi::Scale(253,192,144),"user column width at 200 percent");
   change(96,original);
   Check(details.GetColumnWidth(0)==NativeDpi::Scale(253,96,144),"user column width at 100 percent");
   change(144,suggested);
   Check(details.GetColumnWidth(0)==253,"round trips drifted from the user column width");
  }
  sa.List()->set_style(LVS_LIST);
  change(192,suggested);
  sa.List()->set_style(LVS_REPORT);
  Check(details.GetColumnWidth(0)==NativeDpi::Scale(253,192,144),"List mode transition changed Details widths");
  change(96,original);
  details.SetColumnWidth(0,originalWidth);
  Check(listIconSize(details,LVSIL_NORMAL)==CSize(32,32),"browser image list return to 100 percent");
  fprintf(report,"PASS: DPI suggested rectangle, 96/144/192 layout, rounding, fonts and independent browser state.\n");
  const HWND hide=FindWindowExW(pane,nullptr,L"BUTTON",nullptr);
  Check(hide!=nullptr,"preview Hide button is missing");
  SendMessage(pane,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(hide),BN_CLICKED),reinterpret_cast<LPARAM>(hide));
  Check(!sa.Preview()->Enabled()&&!(sa.Preview()->GetStyle()&WS_VISIBLE)&&!sb.Preview()->Enabled(),
        "Hide did not close only the originating preview pane");
  a->OnUpdateFrameTitle(TRUE);b->OnUpdateFrameTitle(TRUE);Check(Caption(a)==L"First \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","disk captions leaked between frames or MFC restored Untitled");
  Check(sa.MacBrowser().Activate(Find(sa,L"Cafe \u65e5\u672c")),"open Unicode title folder");Check(sa.AwaitNavigationForTest()&&Caption(a)==L"First:Cafe \u65e5\u672c \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","Unicode folder caption or frame isolation failed");Check(sa.MacBrowser().ShowTestFolder(0,2),"restore title test root");
  sa.List()->GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);sb.List()->GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);
  app.ActivateBrowser(b);a->SendMessage(WM_COMMAND,ID_EDIT_SELECT_ALL);
  Check(sa.List()->GetListCtrl().GetSelectedCount()==sa.List()->GetListCtrl().GetItemCount()&&sa.List()->GetListCtrl().GetItemCount()>=2&&sb.List()->GetListCtrl().GetSelectedCount()==0,"selection command used another window's list");
  Check(sa.MacBrowser().Activate(Find(sa,L"One"))&&sa.AwaitNavigationForTest()&&Find(sa,L"One")<0&&Find(sb,L"One")>=0,"native folder navigation leaked between real frames");
  app.ActivateBrowser(b);a->SendMessage(WM_COMMAND,ID_VIEW_REFRESH);
  Check(sa.AwaitNavigationForTest()&&Find(sa,L"One")<0&&Find(sb,L"One")>=0,"refresh command lost a window's current folder");a->OnUpdateFrameTitle(TRUE);Check(Caption(a)==L"First:One \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","refresh changed the current folder caption");
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
  // Verify the accelerator in the shipping resource without altering host keyboard state.
  auto accelerators=LoadAccelerators(AfxGetResourceHandle(),MAKEINTRESOURCE(IDR_MAINFRAME));
  std::vector<ACCEL> keys(CopyAcceleratorTable(accelerators,nullptr,0));
  CopyAcceleratorTable(accelerators,keys.data(),int(keys.size()));
  const auto shortcut=std::find_if(keys.begin(),keys.end(),[](const ACCEL& key){
   return key.key==VK_RETURN&&key.cmd==ID_TOOLS_NEW_BROWSER&&(key.fVirt&(FVIRTKEY|FCONTROL))==(FVIRTKEY|FCONTROL);
  });
  Check(shortcut!=keys.end(),"Ctrl+Enter is not wired to New browser");
  a->SendMessage(WM_COMMAND,MAKEWPARAM(shortcut->cmd,1));
  Check(app.m_browsers.size()==3,"accelerator command did not open a browser");
  auto c=app.m_browsers.back();HWND ch=c->GetSafeHwnd();
  c->SendMessage(WM_CLOSE);Check(!IsWindow(ch)&&IsWindow(ah)&&IsWindow(bh)&&app.m_browsers.size()==2,"closing the newest window closed another browser");NoQuit();
  const auto sharedPath=VolumeCoord::Wide(std::string((LPCSTR)firstImage));
  Check(sa.MacBrowser().ShowTestFolder(0,2)&&sb.MacBrowser().OpenImage(sharedPath)==1&&sb.AwaitNavigationForTest(),
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
   const bool navigated=sb.MacBrowser().Activate(Find(sb,L"Two")) && sb.AwaitNavigationForTest() && Find(sb,L"Peer listing")>=0;
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
   Check(sa.MacBrowser().Activate(Find(sa,L"One")) && sa.AwaitNavigationForTest() && Find(sa,L"Slow payload")>=0,
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
    HANDLE publicationEntered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    HANDLE publicationRelease=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    std::shared_ptr<Storage::Job> job;
    ~WritePause(){
     VolumeSession::PauseVolumeWriteForTest(nullptr,nullptr);
     FileOperations::PausePublicationForTest(nullptr,nullptr);
     if(release)SetEvent(release);
     if(publicationRelease)SetEvent(publicationRelease);
     if(job&&!job->done()){job->cancel();job->wait();}
     if(entered)CloseHandle(entered);
     if(release)CloseHandle(release);
     if(publicationEntered)CloseHandle(publicationEntered);
     if(publicationRelease)CloseHandle(publicationRelease);
    }
   };
   for(bool cancel:{false,true}){
    if(cancel)seedWindows(0x42,true);
    Check(sb.MacBrowser().ShowTestFolder(1,2),"return the peer browser to the root");
    WritePause pause;
    Check(pause.entered&&pause.release&&pause.publicationEntered&&pause.publicationRelease,"create import pauses");
    const auto key=shared->ResourceKey();
    const auto revision=VolumeCoord::Revision(key);
    if(!cancel)FileOperations::PausePublicationForTest(pause.publicationEntered,pause.publicationRelease);
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
     Check(sb.MacBrowser().Activate(Find(sb,L"Uncached"))&&sb.AwaitNavigationForTest()&&!sb.ListingAvailable()&&
           status().Find("Showing the last folder listing")>=0&&!pause.job->done(),
           "uncached folder did not wait for the import");
     Check(sb.MacBrowser().ShowTestFolder(1,2),"cached root could not replace the deferred navigation");
    }
    const auto priorRefresh=sa.notificationRefreshes;
    Check(sb.MacBrowser().Activate(Find(sb,L"One"))&&sb.AwaitNavigationForTest()&&Find(sb,L"Slow payload")>=0&&
              status().Find("Showing the last folder listing")>=0&&!sb.MacBrowser().Writable()&&
              (Find(sb,L"Import payload")>=0)==cancel&&!pause.job->done(),
          "peer did not display the other browser's committed listing during import");
    if(cancel)pause.job->cancel();
    VolumeSession::PauseVolumeWriteForTest(nullptr,nullptr);
    SetEvent(pause.release);
    if(!cancel){
     Check(WaitForSingleObject(pause.publicationEntered,10000)==WAIT_OBJECT_0&&
           !VolumeCoord::Readable(key)&&VolumeCoord::Revision(key)==revision&&
           Find(sb,L"Import payload")<0&&!sb.MacBrowser().Writable(),
           "a committed write became readable before publishing its revision");
     FileOperations::PausePublicationForTest(nullptr,nullptr);
     SetEvent(pause.publicationRelease);
    }
    Check(WaitForSingleObject(pause.job->event(),10000)==WAIT_OBJECT_0&&
              (cancel?pause.job->result().error==ERROR_CANCELLED:!pause.job->result().code),
          "paused import completed with an unexpected result");
    Check(VolumeCoord::Readable(key)&&VolumeCoord::Revision(key)==revision+1,
          "the released reservation did not expose its new revision exactly once");
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
   fprintf(report,"PASS: shared cached listing through atomic revision publication, cancelled overwrite and refresh in both browsers\n");
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
  Check(sa.MacBrowser().Refresh()&&sb.MacBrowser().Refresh()&&sa.AwaitNavigationForTest()&&sb.AwaitNavigationForTest()&&Find(sa,L"Committed before crash")>=0&&
        Find(sb,L"Committed before crash")>=0&&VolumeSession::VolumeSessionPidForTest(shared)!=pid&&shared->Writable(),
        "refresh did not recover both browsers after a worker crash");
  oldSelection.expectedName.clear();
  auto stale=FileOperations::Submit(FileOperations::RenameEntry{oldSelection,u"Stale rename"});
  Check(WaitForSingleObject(stale.Job()->event(),10000)==WAIT_OBJECT_0&&stale.Job()->result().code,
        "selection from the crashed worker was accepted");
  auto fresh=sa.MacBrowser().CaptureItem(Find(sa,L"Committed before crash"));
  auto retry=FileOperations::Submit(FileOperations::RenameEntry{fresh,u"Recovered rename"});
  Check(WaitForSingleObject(retry.Job()->event(),10000)==WAIT_OBJECT_0&&!retry.Job()->result().code&&
        sa.MacBrowser().Refresh()&&sa.AwaitNavigationForTest()&&Find(sa,L"Recovered rename")>=0,
        "fresh selection could not write after journal recovery");
  fprintf(report,"PASS: two browsers share one persistent worker, rename refreshes both without remounting, "
                 "worker crash requires refresh, stale selections are rejected and fresh writes succeed.\n");
  {
   const auto path=VolumeCoord::Wide(std::string((LPCSTR)(host+"\\apfs-browser-writes.img")));
   if(const auto baseline=_wgetenv(L"HFV_APFS_NAMESPACE_BASE")){
    Check(CopyFileW(baseline,path.c_str(),TRUE)!=FALSE,"copy shared APFS browser fixture");
   }else{
    auto image=HfsPlus::CreateFormatOutput(path.c_str(),8*1024*1024);
    Storage::Job job;
    Apfs::Format(*image,u"APFS browser",false,job);
    const auto initial=Apfs::Probe(*image);
    Check(initial&&Apfs::CreateEmptyFile(*image,*initial,2,u"Adapter file",job)==2,"populate APFS browser fixture");
    image->Keep();
   }
   Check(sa.MacBrowser().OpenImage(path)==1&&sa.AwaitNavigationForTest()&&
         sb.MacBrowser().OpenImage(path)==1&&sb.AwaitNavigationForTest(),"open a writable APFS image in both browsers");
   const auto fileRow=Find(sa,L"Adapter file"),peerRow=Find(sb,L"Adapter file");
   Check(fileRow>=0&&peerRow>=0,"browser did not list the APFS file");
   const auto source=sa.MacBrowser().CaptureItem(fileRow);
   const auto peer=sb.MacBrowser().CaptureItem(peerRow);
   const auto mount=source.Volume();
   Check(mount&&mount==peer.Volume()&&mount->Writable()&&mount->FreeBytes()>0,
         "APFS browsers did not share a writable mount with free-space information");

   auto& list=sa.List()->GetListCtrl();
   list.SetItemState(-1,0,LVIS_SELECTED);
   list.SetItemState(fileRow,LVIS_SELECTED,LVIS_SELECTED);
   Check(sa.MacBrowser().CanCreateFolder()&&sa.MacBrowser().CanRename()&&sa.MacBrowser().CanDelete()&&
         sa.MacBrowser().Writable()&&sa.MacBrowser().SelectionWritable()&&
         sa.MacBrowser().CanImportResource()&&sa.MacBrowser().CanExport(),
         "APFS browser advertised the wrong namespace or transfer commands");
   const auto waitForRows=[&](const wchar_t* name,bool present){
    const auto deadline=GetTickCount64()+10000;
    while(GetTickCount64()<deadline){
     if((Find(sa,name)>=0)==present&&(Find(sb,name)>=0)==present)return true;
     MSG message{};
     while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){
      TranslateMessage(&message);DispatchMessage(&message);
     }
     Sleep(1);
    }
    return false;
   };
   const auto outcome=[](FileOperations::Operation& operation){
    return WaitForSingleObject(operation.Job()->event(),10000)==WAIT_OBJECT_0&&
           !operation.Job()->result().code;
   };
   const auto before=VolumeCoord::Revision(mount->ResourceKey());
   auto create=FileOperations::Submit(FileOperations::CreateFolder{sa.MacBrowser().TargetFolder(),u"Browser folder"});
   Check(outcome(create)&&waitForRows(L"Browser folder",true),"APFS folder creation did not refresh both browser windows");
   auto rename=FileOperations::Submit(FileOperations::RenameEntry{source,u"Browser renamed"});
   Check(outcome(rename)&&waitForRows(L"Browser renamed",true)&&waitForRows(L"Adapter file",false),
         "APFS rename did not refresh both browser windows");
   auto stale=FileOperations::Submit(FileOperations::RenameEntry{peer,u"Stale APFS name"});
   Check(WaitForSingleObject(stale.Job()->event(),10000)==WAIT_OBJECT_0&&stale.Job()->result().code,
         "A stale APFS selection from the second window was accepted");
   const auto created=sb.MacBrowser().CaptureItem(Find(sb,L"Browser folder"));
   auto nested=FileOperations::Submit(FileOperations::CreateFolder{created,u"Child folder"});
   Check(outcome(nested)&&!mount->List(created.Id()).empty(),"APFS folder could not be populated through a browser job");
   auto remove=FileOperations::Submit(FileOperations::DeleteEntries{{created}});
   Check(outcome(remove)&&waitForRows(L"Browser folder",false)&&VolumeCoord::Revision(mount->ResourceKey())>before,
         "APFS deletion did not refresh both browser windows");
   fprintf(report,"PASS: ordinary APFS browser open shares an exclusive writable mount and refreshes both windows after namespace edits.\n");

   const auto incomingPath=VolumeCoord::Wide(std::string((LPCSTR)(host+"\\APFS incoming.txt")));
   const auto writeHost=[](const std::wstring& path,const std::string& bytes){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"create Windows APFS copy fixture");
    struct Close {HANDLE file;~Close(){CloseHandle(file);}} close{file};
    DWORD written=0;
    Check(WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)!=FALSE&&written==bytes.size(),
          "write Windows APFS copy fixture");
    ULARGE_INTEGER value{};
    value.QuadPart=(9561628800ull+2170000777ull)*10000000;
    FILETIME stamp{value.LowPart,value.HighPart};
    Check(SetFileTime(file,&stamp,nullptr,&stamp)!=FALSE,"set Windows APFS copy fixture dates");
   };
   const auto target=sa.MacBrowser().TargetFolder();
   const auto resourcePath=VolumeCoord::Wide(std::string((LPCSTR)(host+"\\APFS resource.bin")));
   writeHost(resourcePath,"fork");
   MacBrowser::Entry resourceTarget;
   Check(mount->FindChild(2,u"Browser renamed",resourceTarget),"Missing browser resource import target");
   auto resourceImport=FileOperations::Submit(FileOperations::ImportResourceFork{
       FileOperations::Location::Windows(resourcePath),FileOperations::Location::Mounted(mount,resourceTarget.id)});
   const auto imported=outcome(resourceImport);
   Check(imported,resourceImport.Job()->result().message.c_str());
   std::array<char,4> fork{};
   mount->Read(resourceTarget.id,1,0,fork.data(),fork.size());
   Check(std::string(fork.begin(),fork.end())=="fork"&&
         mount->Stat(resourceTarget.id).forks[0].size==resourceTarget.forks[0].size,
         "APFS resource import changed data fork length or resource contents");
   const auto from=FileOperations::Location::Windows(incomingPath);
   FileOperations::OrdinaryCopy copyToApfs{{from},target};
   std::string oldBytes(9003,'a'),newBytes(7333,'b');
   for(size_t i=0;i<oldBytes.size();++i)oldBytes[i]=char((i>>7)+i*11);
   writeHost(incomingPath,oldBytes);
   const auto sourceDates=MacBrowser::DescribeWindowsFile(incomingPath);
   auto import=FileOperations::Submit(copyToApfs);
   Check(outcome(import)&&waitForRows(L"APFS incoming.txt",true),"Windows-to-APFS copy did not refresh both windows");
   const auto readImported=[&]{
    MacBrowser::Entry entry;
    Check(mount->FindChild(2,u"APFS incoming.txt",entry),"Missing imported APFS file");
    std::string bytes(size_t(entry.forks[0].size),'\0');
    mount->Read(entry.id,0,0,bytes.data(),bytes.size());
    return std::make_pair(entry,std::move(bytes));
   };
   const auto saved=readImported();
   Check(saved.second==oldBytes&&saved.first.datesValid&&saved.first.created==sourceDates.created&&
         saved.first.modified==sourceDates.modified,"APFS copy lost file data or timestamps");

   writeHost(incomingPath,newBytes);
   for(const auto choice:{Storage::ConflictChoice::Skip,Storage::ConflictChoice::Cancel,Storage::ConflictChoice::Replace}){
    auto overwrite=FileOperations::Submit(copyToApfs);
    const auto deadline=GetTickCount64()+10000;
    while(overwrite.Job()->conflict().empty()&&!overwrite.Job()->done()&&GetTickCount64()<deadline)Sleep(1);
    Check(!overwrite.Job()->conflict().empty(),"APFS overwrite did not ask for approval");
    overwrite.Job()->resolve(choice);
    const auto done=outcome(overwrite);
    const auto updated=readImported();
    Check((choice==Storage::ConflictChoice::Cancel?!done&&overwrite.Job()->result().error==ERROR_CANCELLED:done)&&
          updated.second==(choice==Storage::ConflictChoice::Replace?newBytes:oldBytes)&&
          updated.first.identity==saved.first.identity&&updated.first.created==sourceDates.created,
          "APFS overwrite choice changed the wrong data, outcome, or file identity");
   }
   const auto folder=mount->MakeDirectory(2,u"APFS copy target");
   auto sameImage=FileOperations::Submit(FileOperations::OrdinaryCopy{
      {FileOperations::Location::Mounted(mount,saved.first.id)},FileOperations::Location::Mounted(mount,folder)});
   Check(outcome(sameImage),"Same-image APFS file transfer did not complete");
   MacBrowser::Entry nestedFile;
   Check(mount->FindChild(folder,u"APFS incoming.txt",nestedFile)&&nestedFile.forks[0].size==newBytes.size(),
         "Same-image APFS copy produced no data entry");
   std::string nestedBytes(newBytes.size(),'\0');
   mount->Read(nestedFile.id,0,0,nestedBytes.data(),nestedBytes.size());
   Check(nestedBytes==newBytes,"Same-image APFS copy changed the source bytes");

   const auto unsupported=VolumeCoord::Wide(std::string((LPCSTR)host))+L"\\APFS unsupported \U0001fae0";
   Check(CreateDirectoryW(unsupported.c_str(),nullptr)!=FALSE,"create unsupported APFS folder source");
   const auto planned=FileOperations::Location::Windows(unsupported);
   auto blocked=FileOperations::Submit(FileOperations::OrdinaryCopy{{from,planned},FileOperations::Location::Mounted(mount,folder)});
   Check(!outcome(blocked)&&blocked.Job()->conflict().empty()&&
         mount->FindChild(folder,u"APFS incoming.txt",nestedFile)&&mount->List(folder).size()==1,
         "An APFS folder with an unsupported name was not rejected before writing");
   fprintf(report,"PASS: Windows/APFS copying, overwrite Replace/Skip/Cancel, timestamps, same-image streaming, and unsupported folder preflight.\n");

   const auto hostPath=VolumeCoord::Wide(std::string((LPCSTR)host));
   const auto stampFolder=[](const std::wstring& path,uint64_t created,uint64_t modified){
    HANDLE handle=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    Check(handle!=INVALID_HANDLE_VALUE,"open Windows folder for test timestamps");
    struct Close {HANDLE file;~Close(){CloseHandle(file);}} close{handle};
    const auto asFileTime=[](uint64_t seconds){
     ULARGE_INTEGER value{};
     value.QuadPart=(9561628800ull+seconds)*10000000;
     return FILETIME{value.LowPart,value.HighPart};
    };
    const auto creation=asFileTime(created),change=asFileTime(modified);
    Check(SetFileTime(handle,&creation,nullptr,&change)!=FALSE,"set Windows folder timestamps");
   };
   const auto foundFolder=[&](uint32_t parent,const std::u16string& name){
    MacBrowser::Entry result;
    Check(mount->FindChild(parent,name,result)&&result.directory&&result.datesValid,"missing copied APFS folder or dates");
    return result;
   };
   const auto datesMatch=[](const MacBrowser::Entry& expected,const MacBrowser::Entry& actual){
    return expected.datesValid&&actual.datesValid&&expected.created==actual.created&&expected.modified==actual.modified;
   };
   const auto windowsTree=hostPath+L"\\APFS tr\u00e9e";
   const auto windowsChild=windowsTree+L"\\Nested";
   Check(CreateDirectoryW(windowsTree.c_str(),nullptr)&&CreateDirectoryW(windowsChild.c_str(),nullptr),
         "create Windows folder hierarchy for APFS copy");
   writeHost(windowsTree+L"\\Top file.txt",oldBytes);
   writeHost(windowsChild+L"\\Nested \u65e5\u672c.txt",newBytes);
   stampFolder(windowsChild,2160000001,2160000002);
   stampFolder(windowsTree,2150000001,2150000002);
   auto treeCopy=FileOperations::Submit(FileOperations::OrdinaryCopy{{FileOperations::Location::Windows(windowsTree)},target});
   Check(outcome(treeCopy)&&waitForRows(L"APFS tr\u00e9e",true),"recursive APFS copy did not refresh both windows");
   const auto treeCopyRoot=foundFolder(2,u"APFS tre\u0301e");
   const auto treeCopyChild=foundFolder(treeCopyRoot.id,u"Nested");
   Check(datesMatch(MacBrowser::DescribeWindowsFile(windowsTree),treeCopyRoot)&&
         datesMatch(MacBrowser::DescribeWindowsFile(windowsChild),treeCopyChild),
         "APFS recursive copy did not preserve source folder dates");
   MacBrowser::Entry copiedPayload;
   Check(mount->FindChild(treeCopyRoot.id,u"Top file.txt",copiedPayload)&&copiedPayload.forks[0].size==oldBytes.size()&&
         mount->FindChild(treeCopyChild.id,u"Nested \u65e5\u672c.txt",copiedPayload)&&copiedPayload.forks[0].size==newBytes.size(),
         "APFS recursive copy lost either level of the hierarchy");
   std::string treeData(newBytes.size(),'\0');
   mount->Read(copiedPayload.id,0,0,treeData.data(),treeData.size());
   Check(treeData==newBytes,"APFS recursive copy changed nested file data");

   for(const auto choice:{Storage::ConflictChoice::Replace,Storage::ConflictChoice::Skip,Storage::ConflictChoice::Cancel}){
    const bool cancel=choice==Storage::ConflictChoice::Cancel;
    const bool overwrite=choice==Storage::ConflictChoice::Replace;
    const auto leaf=cancel?L"APFS merge cancel":overwrite?L"APFS merge replace":L"APFS merge skip";
    const auto hostFolder=hostPath+L"\\"+leaf;
    Check(CreateDirectoryW(hostFolder.c_str(),nullptr)!=FALSE,"create Windows source for APFS folder merge");
    writeHost(hostFolder+L"\\one.txt",newBytes);
    writeHost(hostFolder+L"\\two.txt",newBytes);
    stampFolder(hostFolder,2180000001,2180000002);
    auto resolved=MacBrowser::ResolveWindowsFile(hostFolder);
    const auto children=resolved.first->List(resolved.second);
    Check(children.size()==2,"APFS merge fixture has unexpected children");
    resolved.first->Close();

    MacBrowser::Entry desired;
    desired.name=MacBrowser::U16(leaf);
    desired.directory=desired.datesValid=true;
    desired.created=2090000001;
    desired.modified=2090000002;
    const auto seeded=mount->MakeDirectoryWithAttributes(2,desired);
    MacBrowser::Entry collision;
    collision.name=children[1].name;
    collision.forks[0].size=oldBytes.size();
    mount->Put(seeded,collision,[&](unsigned,uint64_t at,void* data,size_t size){
     memcpy(data,oldBytes.data()+at,size);
    },false);
    const auto expected=mount->Stat(seeded);
    auto operation=FileOperations::Submit(FileOperations::OrdinaryCopy{{FileOperations::Location::Windows(hostFolder)},target});
    const auto deadline=GetTickCount64()+10000;
    while(operation.Job()->conflict().empty()&&!operation.Job()->done()&&GetTickCount64()<deadline)Sleep(1);
    Check(!operation.Job()->conflict().empty()&&operation.Job()->progress().files==1,
          "APFS merge did not reach a conflict after creating a child");
    operation.Job()->resolve(choice);
    const bool done=outcome(operation);
    Check((cancel?!done&&operation.Job()->result().error==ERROR_CANCELLED:done)&&
          datesMatch(expected,mount->Stat(seeded)),"APFS folder merge did not restore destination dates");
    MacBrowser::Entry kept,added;
    Check(mount->FindChild(seeded,children[0].name,added)&&added.forks[0].size==newBytes.size()&&
          mount->FindChild(seeded,children[1].name,kept)&&kept.forks[0].size==(overwrite?newBytes.size():oldBytes.size()),
          "APFS merge lost a committed child or used the wrong overwrite decision");
   }
   const auto compressionItem=FileOperations::Location::Mounted(mount,copiedPayload.id);
   const auto originalPayload=mount->Stat(copiedPayload.id);
   auto compression=FileOperations::Submit(FileOperations::ChangeCompression{
       compressionItem,originalPayload,mount->GetAttributes(copiedPayload.id),
       HfsPlus::Volume::CompressionMode::Zlib});
   const bool compressed=outcome(compression);
   Check(compressed,compression.Job()->result().message.c_str());
   Check(mount->Stat(copiedPayload.id).compressed,"APFS compression request did not set compression");
   std::string compressedRead(newBytes.size(),'\0');
   mount->Read(copiedPayload.id,0,0,compressedRead.data(),compressedRead.size());
   Check(compressedRead==newBytes,"APFS browser compression job changed file contents");
   const auto finishProperties=[]{
    const auto deadline=GetTickCount64()+15000;
    while(BackgroundCopies::Active()&&GetTickCount64()<deadline){
     MSG message{};
     while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){
      TranslateMessage(&message);DispatchMessage(&message);
     }
     Sleep(1);
    }
    Check(!BackgroundCopies::Active(),"APFS Properties job timed out");
   };
   const auto folderItem=FileOperations::Location::Mounted(mount,treeCopyRoot.id);
   Check(MetadataDialogs::Show(folderItem,a),"open APFS folder Properties");
   finishProperties();
   const auto summary=MetadataDialogs::Window();
   const auto editMetadata=GetDlgItem(summary,IDC_APFS_EDIT_METADATA);
   Check(editMetadata&&(GetWindowLongPtr(editMetadata,GWL_STYLE)&WS_VISIBLE)&&
         BrowserPresentation::Text(editMetadata)==L"Edit Metadata\u2026",
         "APFS Properties did not expose the Finder information editor");
   SendMessage(summary,WM_COMMAND,IDC_ATTR_OPEN,0);
   const auto attributes=MetadataDialogs::Window();
   Check(attributes&&attributes!=summary&&IsWindowEnabled(GetDlgItem(attributes,IDOK)),
         "APFS extended attributes were not editable");
   SetDlgItemTextW(attributes,IDC_ATTR_NAME,L"user.apfs");
   SetDlgItemTextW(attributes,IDC_ATTR_VALUE,L"12 34");
   SendMessage(attributes,WM_COMMAND,IDC_ATTR_SET,0);
   SendMessage(attributes,WM_COMMAND,IDOK,0);
   finishProperties();
   Check(!BackgroundCopies::LastResult().code&&mount->GetAttributes(treeCopyRoot.id).at(u"user.apfs")==HfsPlus::Bytes({0x12,0x34}),
         "APFS Properties did not save an extended attribute");
   SendMessage(summary,WM_COMMAND,IDC_APFS_EDIT_METADATA,0);
   const auto editor=MetadataDialogs::Window();
   Check(editor&&editor!=summary&&GetDlgItem(editor,IDC_MFS_FINDER),"missing APFS Finder editor");
   SetDlgItemTextW(editor,IDC_MFS_FINDER,L"GG");
   SendMessage(editor,WM_COMMAND,IDOK,0);
   Check(IsWindow(editor)&&!MetadataDialogs::LastError().empty(),"invalid Finder hex closed the editor");
   std::wstring finder=L"00 ";
   for(unsigned i=1;i<31;++i)finder+=L"00 ";
   finder+=L"A5";
   SetDlgItemTextW(editor,IDC_MFS_FINDER,finder.c_str());
   CheckDlgButton(editor,IDC_MFS_LOCKED,BST_CHECKED);
   SYSTEMTIME modified{};
   modified.wYear=2050;modified.wMonth=2;modified.wDay=3;modified.wHour=4;modified.wMinute=5;modified.wSecond=6;
   Check(SendDlgItemMessageW(editor,IDC_MFS_MODIFIED,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&modified)),
         "APFS date picker rejected a valid date after 2040");
   FILETIME fileTime{};
   Check(SystemTimeToFileTime(&modified,&fileTime),"could not encode APFS test date");
   const auto expectedModified=((uint64_t(fileTime.dwHighDateTime)<<32)|fileTime.dwLowDateTime)/10000000ULL-9561628800ULL;
   SendMessage(editor,WM_COMMAND,IDOK,0);
   finishProperties();
   Check(!BackgroundCopies::LastResult().code&&mount->Stat(treeCopyRoot.id).finder[31]==0xa5&&
         mount->Stat(treeCopyRoot.id).modified==expectedModified&&mount->Stat(treeCopyRoot.id).protectedFile&&
         !IsWindow(editor)&&a->IsWindowEnabled(),"APFS Finder editor did not apply or restore the browser");
   MetadataDialogs::Shutdown();
   fprintf(report,"PASS: APFS Properties routes to a working Finder information, timestamp and lock editor for folders.\n");
   fprintf(report,"PASS: recursive APFS folder copy, source timestamps, and destination dates after completed, skipped and cancelled merges.\n");
  }
  if(const auto fixture=_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE")){
   const auto path=VolumeCoord::Wide(std::string(directory)+"\\apfs-mounted-snapshot.img");
   Check(CopyFileW(fixture,path.c_str(),TRUE)!=FALSE,"copy the macOS mounted-snapshot fixture");
   Check(sa.MacBrowser().OpenImage(path)==1&&sa.AwaitNavigationForTest()&&
         sb.MacBrowser().OpenImage(path)==1&&sb.AwaitNavigationForTest(),"open the macOS image in both browsers");
   const auto volume=sa.MacBrowser().SelectedVolume();
   Check(volume&&volume==sb.MacBrowser().SelectedVolume(),"APFS browsers did not share the mounted image");
   const auto key=volume->ResourceKey();
   const auto originalRoot=MacBrowser::Wide(volume->Name());
   MacBrowser::Entry liveFixture,liveFile;
   Check(volume->FindChild(2,u"Fixture",liveFixture)&&volume->FindChild(liveFixture.id,u"entry-0",liveFile),
         "locate the macOS file for snapshot restore");
   HfsPlus::Bytes historicalBytes(size_t(liveFile.forks[0].size));
   volume->Read(liveFile.id,0,0,historicalBytes.data(),historicalBytes.size());
   const auto restoreFolder=volume->MakeDirectory(2,u"Snapshot restore folder");
   MacBrowser::Entry folderFile;
   folderFile.name=u"Historical child";
   folderFile.forks[0].size=4201;
   folderFile.forks[1].size=4603;
   volume->Put(restoreFolder,folderFile,[](unsigned fork,uint64_t,void* bytes,size_t size){
    memset(bytes,fork?0x36:0x52,size);
   },false);
   const auto history=originalRoot+L" [Snapshot: Mounted test]";
   Check(sb.MacBrowser().Activate(Find(sb,L"Fixture"))&&sb.AwaitNavigationForTest()&&
         Find(sb,L"entry-0")>=0,"navigate the peer to the live APFS folder");
   auto firstRefresh=sa.notificationRefreshes,secondRefresh=sb.notificationRefreshes;
   const auto checkRefresh=[&](uint64_t previous){
    Check(VolumeCoord::Revision(key)==previous+1,"Mounted APFS snapshot write did not publish a revision");
    const auto deadline=GetTickCount64()+10000;
    while((sa.notificationRefreshes==firstRefresh||sb.notificationRefreshes==secondRefresh)&&GetTickCount64()<deadline){
     MSG message{};
     while(PeekMessage(&message,nullptr,0,0,PM_REMOVE))DispatchMessage(&message);
     Sleep(1);
    }
    Check(sa.notificationRefreshes>firstRefresh&&sb.notificationRefreshes>secondRefresh&&
          sa.AwaitNavigationForTest()&&sb.AwaitNavigationForTest()&&
          sa.MacBrowser().SelectedVolume()==volume&&sb.MacBrowser().SelectedVolume()==volume,
          "Snapshot refresh replaced or lost the mounted image");
   };
   Storage::Job create;
   auto version=VolumeCoord::Revision(key);
   Check(Apfs::MountedSnapshotNames(*volume).empty()&&
         Apfs::ChangeMountedSnapshot(*volume,u"Mounted test",true,create)==std::vector<std::u16string>{u"Mounted test"},
         "create a snapshot without closing the mounted APFS image");
   checkRefresh(version);
   Check(Find(sa,history.c_str())>=0&&Find(sb,L"entry-0")>=0,
         "open APFS browsers did not show the new snapshot and retain the live folder");
   Check(sa.MacBrowser().Activate(Find(sa,history.c_str()))&&sa.AwaitNavigationForTest()&&
         Find(sa,L"Fixture")>=0,"browse the new snapshot before deletion");
   MacBrowser::Entry liveRoot;
   Check(volume->FindChild(2,volume->Name(),liveRoot),"missing live APFS root");
   auto changed=volume->Stat(liveFile.id);
   const HfsPlus::Bytes replacement(113,0x9a);
   changed.forks[0].size=replacement.size();
   volume->Put(liveFixture.id,changed,[&](unsigned,uint64_t offset,void* bytes,size_t size){
    std::copy_n(replacement.data()+offset,size,static_cast<uint8_t*>(bytes));
   },true);
   Storage::Job deleteFolder;
   Check(volume->RemoveTreeAtomically(restoreFolder,deleteFolder,false),"remove the live folder before restoring it");
   const auto selectRow=[](BrowserSession& session,const wchar_t* label){
    auto& list=session.List()->GetListCtrl();
    const auto row=Find(session,label);
    Check(row>=0,"missing APFS snapshot restore selection");
    list.SetItemState(-1,0,LVIS_SELECTED);
    list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    Check(session.MacBrowser().CanRestoreSnapshotSelection(),"APFS snapshot selection did not enable Restore");
   };
   const auto readLive=[&]{
    const auto file=volume->Stat(liveFile.id);
    HfsPlus::Bytes data(size_t(file.forks[0].size));
    volume->Read(file.id,0,0,data.data(),data.size());
    return data;
   };
   const bool background=BackgroundCopies::enabled;
   BackgroundCopies::enabled=false;
   Check(sa.MacBrowser().Activate(Find(sa,L"Fixture"))&&sa.AwaitNavigationForTest(),"browse the snapshot folder to restore a file");
   selectRow(sa,L"entry-0");
   const auto savedAnswer=storageUiTest.answer;
   storageUiTest.answer=Storage::ConflictChoice::Cancel;
   a->SendMessage(WM_COMMAND,ID_TOOLS_APFS_RESTORE);
   Check(readLive()==replacement,"Cancelling APFS restore changed the live file");
   selectRow(sa,L"entry-0");
   storageUiTest.answer=Storage::ConflictChoice::Replace;
   a->SendMessage(WM_COMMAND,ID_TOOLS_APFS_RESTORE);
   Check(readLive()==historicalBytes,"Approving APFS restore did not replace the live file");
   const auto historyIndex=sa.MacBrowser().SelectedVolume();
   const auto treeSelection=sa.Tree()->GetTreeCtrl().GetSelectedItem();
   const auto historyRoot=sa.Tree()->GetTreeCtrl().GetParentItem(treeSelection);
   Check(historyRoot&&historyIndex==volume,"missing APFS snapshot parent");
   sa.Tree()->GetTreeCtrl().SelectItem(historyRoot);
   Check(sa.AwaitNavigationForTest()&&Find(sa,L"Snapshot restore folder")>=0,"return to the snapshot root");
   selectRow(sa,L"Snapshot restore folder");
   a->SendMessage(WM_COMMAND,ID_TOOLS_APFS_RESTORE);
   storageUiTest.answer=savedAnswer;
   BackgroundCopies::enabled=background;
   MacBrowser::Entry restoredFolder,restoredChild;
   Check(volume->FindChild(liveRoot.id,folderFile.name,restoredChild)==false&&
         volume->FindChild(liveRoot.id,u"Snapshot restore folder",restoredFolder)&&
         volume->FindChild(restoredFolder.id,folderFile.name,restoredChild),
         "Snapshot folder restore did not recreate the folder and child at the original path");
   for(unsigned fork=0;fork<2;++fork){
    HfsPlus::Bytes actual(size_t(restoredChild.forks[fork].size));
    volume->Read(restoredChild.id,fork,0,actual.data(),actual.size());
    Check(actual==HfsPlus::Bytes(size_t(folderFile.forks[fork].size),fork?0x36:0x52),
          "Snapshot folder restore did not preserve both child forks");
   }
   fprintf(report,"PASS: APFS snapshot GUI restore prompted before overwrite, preserved data on cancellation and restored a folder with both forks.\n");
   Storage::Job remove;
   firstRefresh=sa.notificationRefreshes;
   secondRefresh=sb.notificationRefreshes;
   version=VolumeCoord::Revision(key);
   Check(Apfs::ChangeMountedSnapshot(*volume,u"Mounted test",false,remove).empty(),
         "delete an APFS snapshot without closing the mounted image");
   checkRefresh(version);
   Check(Find(sa,history.c_str())<0&&Find(sa,L"Fixture")>=0&&Find(sb,L"entry-0")>=0,
         "open APFS browsers did not remove the snapshot and retain the live folder");
   fprintf(report,"PASS: both open APFS browser windows refreshed after mounted snapshot creation and deletion.\n");
  }
  // Make the first frame the MFC main window before closing it: this is the
  // case that CFrameWnd's default SDI shutdown used to close every document.
  app.ActivateBrowser(a);a->SendMessage(WM_CLOSE);
  Check(!IsWindow(ah)&&IsWindow(bh)&&app.m_browsers.size()==1&&app.m_pMainWnd==b&&app.ActiveBrowser()==b,"first window closure did not transfer ownership to the surviving browser");NoQuit();
  b->SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);Check((sb.List()->GetStyle()&LVS_TYPEMASK)==LVS_REPORT&&sb.MacBrowser().Refresh(),"remaining window is not usable after first closes");
  b->SendMessage(WM_COMMAND,ID_TOOLS_NEW_BROWSER);Check(app.m_browsers.size()==2,"remaining window cannot create another browser");
  // Explicit File/Exit closes the full collection; a title-bar close does not.
  b->SendMessage(WM_COMMAND,ID_APP_EXIT);Check(app.m_browsers.empty()&&!IsWindow(bh)&&app.m_pMainWnd==nullptr&&app.ActiveBrowser()==nullptr,"File Exit did not close the remaining browser collection");
  // WM_QUIT may be synthesized only after the remaining posted messages drain.
  bool quit=false;
  const auto exitStart=GetTickCount64();
  while(!quit&&GetTickCount64()-exitStart<2000){
   MSG message{};
   if(!PeekMessage(&message,nullptr,0,0,PM_REMOVE)){Sleep(1);continue;}
   if(message.message==WM_QUIT)quit=true;
   else DispatchMessage(&message);
  }
  Check(quit,"last window did not terminate the message loop");
  fprintf(report,"PASS: independent real hidden top-level frames/documents/panes; New browser menu and Ctrl+Enter; view/layout and image-open command routing; per-window disk/folder and Unicode captions, startup and MFC title-update persistence; busy/failed-creation guards; closing newest and first/main windows without WM_QUIT; surviving browser refresh/new commands; explicit File Exit and final-window shutdown. No hardware access or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());result=1;}
 catch(CException* e){char text[512]{};e->GetErrorMessage(text,ARRAYSIZE(text));fprintf(report,"FAIL: MFC exception: %s\n",text);e->Delete();result=1;}
 fclose(report);return result;
}
#endif
