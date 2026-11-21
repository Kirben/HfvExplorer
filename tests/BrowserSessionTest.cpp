// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "DocumentLaunch.h"
#include "HFVExplorer.h"
#include "HFVCommandLineInfo.h"
#include "BrowserSession.h"
#include "CopyProgress.h"
#include "BrowserDirectory.h"
#include "MainMacBrowser.h"
#include "FindFiles.h"
#include "HfsPlusDisk.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "utils.h"
#include "HFVExplorerView.h"
#include "FloppyWriter.h"
#include "ReadOnlyCdBackend.h"
#include "FakeFloppy.h"
#include <cstdio>
#include <atomic>
#include <fcntl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
}
namespace {
void Check(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
struct Doc:CHFVExplorerDoc {
 Doc():CHFVExplorerDoc(false){m_bAutoDelete=FALSE;}
 using CHFVExplorerDoc::FindClassicLauncher;
 using CHFVExplorerDoc::my_chdir;
 using CHFVExplorerDoc::NavigateWindowsFolder;
 using CHFVExplorerDoc::NavigateClassicFolder;
 void AddIcon(unsigned type){local_icn_sharp_t small{},large{};small.icon=large.icon=reinterpret_cast<icn_sharp_hand>(1);folderReader_->insert_into_hash_table(type,'TEST',&small,&large);}
 bool HasIcon(unsigned type){return folderReader_->map_type_and_creator(type,'TEST',0)!=nullptr;}
};
struct Frame:CMainFrame {
 void LoadWindowPosition()override{}
 void SaveWindowPosition()override{}
 void PostNcDestroy()override{}
};
struct Tree:HFVExplorerTreeView {
 void Init(Doc& doc){m_pDocument=&doc;Initialize();enable_selchange(1);}
 HTREEITEM Host(const CString& path){CString label="Windows",copy=path;return initialize_fat_volume(0,&label,DRIVE_FIXED,&copy,FALSE);}
 HTREEITEM Classic(int slot){return initialize_volume(slot,"Same",2,0,0,0,0);}
 void Unbind(){m_pDocument=nullptr;}
 void PostNcDestroy()override{}
};
struct List:CHFVExplorerListView {void Bind(Doc* doc){m_pDocument=doc;}void PostNcDestroy()override{};};
std::wstring Text(CListCtrl& list,int row){wchar_t text[1024]{};LVITEMW item{};item.pszText=text;item.cchTextMax=ARRAYSIZE(text);SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));return text;}
int Find(CHFVExplorerListView& list,const wchar_t* name){for(int row=0;row<list.GetListCtrl().GetItemCount();++row)if(Text(list.GetListCtrl(),row)==name)return row;return -1;}
class Fixture:public MacBrowser::Volume {
 std::u16string name_;
public:
 bool fail=false,wrongThread=false;HANDLE gate=CreateEvent(nullptr,TRUE,FALSE,nullptr);
 ~Fixture(){CloseHandle(gate);}
 explicit Fixture(const std::u16string& name):name_(name){}
 std::u16string Name()const override{return name_;}
 const char* Kind()const override{return "Session fixture";}bool Writable()const override{return false;}bool CaseSensitive()const override{return true;}uint64_t FreeBytes()const override{return 0;}
 MacBrowser::Entry Stat(uint32_t id)override{MacBrowser::Entry e;e.id=id;e.parent=id==2?1:id==5?3:2;e.name=id==2?name_:id==3?u"Folder":id==4?name_+u" file":name_+u" child";e.directory=id==2||id==3;return e;}
 std::vector<MacBrowser::Entry> List(uint32_t id)override{
  wrongThread|=GetCurrentThreadId()==StorageUi::state().thread;
  if(fail)throw HfsPlus::Error("Injected session read failure");return id==2?std::vector<MacBrowser::Entry>{Stat(3),Stat(4)}:std::vector<MacBrowser::Entry>{Stat(5)};
 }
 void Read(uint32_t,unsigned,uint64_t,void*,size_t)override{throw HfsPlus::Error("Not a content test");}
 uint32_t MakeDirectory(uint32_t,const std::u16string&)override{throw HfsPlus::Error("Read-only");}
 uint32_t Put(uint32_t,const MacBrowser::Entry&,MacBrowser::Reader,bool)override{throw HfsPlus::Error("Read-only");}
 void Rename(uint32_t,uint32_t,const std::u16string&)override{throw HfsPlus::Error("Read-only");}
 void Remove(uint32_t)override{throw HfsPlus::Error("Read-only");}
};
struct Panes {
 Doc doc;CWnd owner;Tree tree;List list;
 Panes(){
  Check(owner.CreateEx(0,AfxRegisterWndClass(0),"Hidden browser session",WS_OVERLAPPED,0,0,700,400,nullptr,0)!=FALSE,"create session owner");
  doc.GetBrowserSession().Attach(&owner,&tree,&list);list.Bind(&doc);
  Check(tree.Create(nullptr,nullptr,WS_CHILD|TVS_HASLINES,CRect(0,0,200,400),&owner,100,nullptr)!=FALSE,"create session tree");
  Check(list.Create(nullptr,nullptr,WS_CHILD|LVS_REPORT,CRect(200,0,700,400),&owner,101,nullptr)!=FALSE,"create session list");tree.Init(doc);list.Initialize();
 }
 ~Panes(){doc.GetBrowserSession().MacBrowser().Clear();tree.enable_selchange(0);list.list_clear();tree.Unbind();list.Bind(nullptr);list.DestroyWindow();tree.DestroyWindow();owner.DestroyWindow();}
 BrowserSession& Session(){return doc.GetBrowserSession();}
 void Windows(const CString& path){if(doc.Mounts().WindowsCount()==0)doc.Mounts().AddWindows(path[0],DRIVE_FIXED,path);tree.enable_selchange(0);doc.GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::WindowsDrive,0},tree.Host(path));tree.GetTreeCtrl().SelectItem(Session().MountRoot({BrowserMountInventory::Kind::WindowsDrive,0}));tree.enable_selchange(1);Check(doc.NavigateWindowsFolder(0,std::string(path.GetString()))&&Session().AwaitNavigationForTest(),"open Windows fixture through navigation");}
 void Classic(const CString& path,const char* filename,int slot=0){
  {CFile image;Check(image.Open(path,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create session classic image");image.SetLength(2*1024*1024);}
  char label[]="Same",type[]="TEXT",app[]="APPL",creator[]="TEST";
  Check(hfs_format(const_cast<char*>((LPCSTR)path),0,label)==0,"format session classic image");
  auto v=hfs_mount(const_cast<char*>((LPCSTR)path),0,O_RDWR);Check(v!=nullptr,"mount session classic image");std::string file="Same:";file+=filename;
  char folder[]="Same:Apps",nested[]="Same:Apps:Tools";
  auto launcher=std::string(nested)+":"+filename+" app";
  Check(hfs_create(v,file.data(),type,creator)==0&&hfs_mkdir(v,folder)==0&&hfs_mkdir(v,nested)==0&&
        hfs_create(v,launcher.data(),app,creator)==0&&hfs_umount(v)==0,"populate session classic image");
  Check(doc.insert_volume(path,0,0,0,0)==slot,"register session classic volume");
  Check(doc.Mounts().OpenClassicForTest(slot),"open session classic volume");
  tree.enable_selchange(0);
  const auto root=tree.Classic(slot);
  doc.GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::ClassicVolume,slot},root);
  tree.GetTreeCtrl().SelectItem(root);
  tree.enable_selchange(1);Check(doc.NavigateClassicFolder(slot,2)&&Session().AwaitNavigationForTest(),"open classic fixture through navigation");
 }
};
class NavigationFixture : public Fixture {
public:
 HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
 std::atomic<bool> changed{false};
 std::atomic<unsigned> statReads{0};
 explicit NavigationFixture(const std::u16string& name):Fixture(name){}
 ~NavigationFixture(){CloseHandle(entered);}
 MacBrowser::Entry Stat(uint32_t id)override{++statReads;return Fixture::Stat(id);}
 std::vector<MacBrowser::Entry> List(uint32_t id)override{
  if(id!=3)return {Stat(3),Stat(4)};
  SetEvent(entered);
  Check(WaitForSingleObject(gate,5000)==WAIT_OBJECT_0,"navigation test read timed out");
  return {Stat(5)};
 }
 void ValidateMedia()override{if(changed)throw HfsPlus::Error("Media changed during navigation");}
};

template<class Predicate>
void PumpUntil(Predicate ready,const char* message,bool dispatchTimers=true)
{
 DWORD start=GetTickCount();
 while(!ready()){
  Check(GetTickCount()-start<8000,message);
  MSG msg;
  while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
   if(!dispatchTimers&&msg.message==WM_TIMER)continue;
   TranslateMessage(&msg);
   DispatchMessage(&msg);
  }
  Sleep(5);
 }
}

FindHit SearchHit(FindScope scope,const char* pattern)
{
 Storage::Job job;
 auto results=std::make_shared<FindResults>();
 const auto status=MakeFindFilesJob({std::move(scope),pattern},results)(job);
 Check(!status.code&&results->hits.size()==1,"search fixture did not return one typed hit");
 return results->hits.front();
}

bool Selected(CHFVExplorerListView& view,const wchar_t* name)
{
 auto row=Find(view,name);
 return row>=0&&(view.GetListCtrl().GetItemState(row,LVIS_SELECTED)&LVIS_SELECTED);
}

void TestNavigation()
{
 auto slow=std::make_shared<NavigationFixture>(u"Slow");
 auto other=std::make_shared<Fixture>(u"Other");
 Panes first;
 auto& session=first.Session();
 auto& browser=session.MacBrowser();
 int second=browser.AttachTestVolume(other,"other");
 Check(browser.ShowTestFolder(second,2),"open other volume");
 auto quickRoot=first.tree.GetTreeCtrl().GetSelectedItem();
 int index=browser.AttachTestVolume(slow,"slow",25);
 Check(browser.ShowTestFolder(index,2),"open slow volume");
 auto& tree=first.tree.GetTreeCtrl();
 auto slowRoot=tree.GetSelectedItem();
 auto child=tree.GetChildItem(slowRoot);

 tree.SelectItem(child);
 PumpUntil([&]{return WaitForSingleObject(slow->entered,0)==WAIT_OBJECT_0;}, "slow folder never started");
 Check(session.Navigating()&&Find(first.list,L"Slow file")>=0&&first.owner.IsWindowEnabled(),
    "pending navigation hid its previous listing or blocked the owner");
 tree.SelectItem(quickRoot);
 PumpUntil([&]{return !session.Navigating();},"replacement navigation waited for the stale read",false);
 Check(Find(first.list,L"Other file")>=0,"replacement folder did not publish");
 Check(browser.Refresh()&&session.Navigating()&&Find(first.list,L"Other file")>=0,
       "refresh did not use the per-window request or discarded the prior listing");
 PumpUntil([&]{return !session.Navigating();},"asynchronous refresh did not finish",false);
 SetEvent(slow->gate);
 PumpUntil([&]{return VolumeCoord::Available({{slow->ResourceKey(),VolumeCoord::Mode::Write}});},"stale navigation retained its read lease");
 Check(Find(first.list,L"Other file")>=0&&Find(first.list,L"Slow child")<0,"stale folder replaced newer selection");

 tree.SelectItem(slowRoot);
 PumpUntil([&]{return !session.Navigating();},"return to slow root failed");
 ResetEvent(slow->gate);
 ResetEvent(slow->entered);
 FindHit hit;
 hit.destination=FileOperations::Location::Mounted(slow,3);
 hit.reveal=FileOperations::Location::Mounted(slow,5);
 hit.reveal.expectedName=u"Slow child";
 hit.reveal.expectedParent=3;
 hit.nativePath={2,3,5};
 const auto beforeCapture=slow->statReads.load();
 const auto scope=session.CaptureFindScope();
 Check(slow->statReads.load()==beforeCapture,"capturing Find accessed the mounted catalog");
 Check(scope&&scope->mount&&scope->origin.Volume()==slow&&scope->catalogRoute==std::vector<uint32_t>{2},
       "mounted Find did not capture its current tree and mount");
 hit.mount=scope->mount;
 Check(session.RevealFindHit(hit),"queue typed mounted Find result");
 PumpUntil([&]{return WaitForSingleObject(slow->entered,0)==WAIT_OBJECT_0;},"mounted Find read never started");
 tree.SelectItem(quickRoot);
 PumpUntil([&]{return !session.Navigating();},"new folder did not supersede Find");
 SetEvent(slow->gate);
 PumpUntil([&]{return VolumeCoord::Readable(slow->ResourceKey())&&
     VolumeCoord::Available({{slow->ResourceKey(),VolumeCoord::Mode::Write}});},"superseded Find retained its lease");
 Check(Find(first.list,L"Other file")>=0&&Find(first.list,L"Slow child")<0,
       "a superseded Find changed the newer listing");

 tree.SelectItem(slowRoot);
 PumpUntil([&]{return !session.Navigating();},"open root before Find media test");
 ResetEvent(slow->gate);
 ResetEvent(slow->entered);
 Check(session.RevealFindHit(hit),"queue Find on mounted media");
 PumpUntil([&]{return WaitForSingleObject(slow->entered,0)==WAIT_OBJECT_0;},"media swap read never started");
 slow->changed=true;
 SetEvent(slow->gate);
 PumpUntil([&]{return !session.Navigating();},"media swap read did not finish");
 Check(!session.ListingAvailable()&&Find(first.list,L"Slow file")>=0&&Find(first.list,L"Slow child")<0,
       "a Find listing from changed media was published or left actionable");
 slow->changed=false;
 Check(session.RevealFindHit(hit),"retry Find on valid media");
 PumpUntil([&]{return !session.Navigating();},"valid Find did not complete");
 Check(session.ListingAvailable()&&Selected(first.list,L"Slow child"),
       "mounted Find did not select the result after publication");
 tree.SelectItem(slowRoot);
 PumpUntil([&]{return !session.Navigating();},"open root before device notification");
 ResetEvent(slow->gate);
 ResetEvent(slow->entered);
 Check(session.RevealFindHit(hit),"queue Find before device notification");
 PumpUntil([&]{return WaitForSingleObject(slow->entered,0)==WAIT_OBJECT_0;},"notification read never started");
 session.NavigationMediaChanged(1u<<24);
 Check(session.Navigating(),"an unrelated media notification cancelled navigation");
 session.NavigationMediaChanged(1u<<25);
 Check(!session.RevealFindHit(hit),"a Find hit from a prior media epoch was accepted");
 SetEvent(slow->gate);
 PumpUntil([&]{return VolumeCoord::Available({{slow->ResourceKey(),VolumeCoord::Mode::Write}});},"cancelled device navigation retained its lease");
 Check(!session.Navigating()&&!session.ListingAvailable()&&Find(first.list,L"Slow file")>=0,
       "a device notification did not invalidate the pending Find");

 auto closing=std::make_shared<NavigationFixture>(u"Closing");
 {
  Panes pane;
  auto& window=pane.Session();
  int mount=window.MacBrowser().AttachTestVolume(closing,"closing");
  Check(window.MacBrowser().ShowTestFolder(mount,2),"open closing fixture");

  auto& control=pane.tree.GetTreeCtrl();
  control.SelectItem(control.GetChildItem(control.GetSelectedItem()));
  PumpUntil([&]{return WaitForSingleObject(closing->entered,0)==WAIT_OBJECT_0;},"closing read never started");
 }
 SetEvent(closing->gate);
 PumpUntil([&]{return VolumeCoord::Available({{closing->ResourceKey(),VolumeCoord::Mode::Write}});},"closed window retained its read lease",false);
 Check(Find(first.list,L"Slow child")<0,"closed-window navigation affected another window");
}

}
int RunBrowserSessionTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;CString root=directory;FILE* report=fopen(root+"\\browser-session-report.txt","w");if(!report)return 2;
 setvbuf(report,nullptr,_IONBF,0);
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());app.m_mac_window_mode=0;app.m_dosicons=DI_NO_ICONS;app.m_show_invisible_mac=app.m_show_invisible_fat=1;InitializeStorageUi();
 int result=0;
 try{
  {
   using Kind=BrowserMountInventory::Kind;
   BrowserMountInventory mounts;
   BrowserMountInventory::ClassicMount before,after;
   before.m_file_name="previous.dsk";
   after.m_file_name="replacement.dsk";
   before.m_volume_name=after.m_volume_name="Same name";
   const auto index=mounts.AddClassic(before);
   const auto old=mounts.Lookup({Kind::ClassicVolume,index});
   Check(old.has_value(),"register a classic volume in the inventory");
   mounts.RemoveClassic(index);
   Check(mounts.AddClassic(after)==index,"classic mount did not reuse the free slot");
   const auto current=mounts.Lookup({Kind::ClassicVolume,index});
   Check(current&&current->address.incarnation!=old->address.incarnation&&
         !mounts.IsCurrent(*old)&&!mounts.PublishClassic(*old,before),
         "a delayed classic listing could publish into a reused slot");
   Check(mounts.PublishClassic(*current,after),"a current mount could not publish without a tree");
   const auto drive=mounts.AddWindows('Y',DRIVE_REMOVABLE);
   const auto priorDrive=mounts.FindWindowsDrive(24);
   mounts.SetWindowsType(drive,DRIVE_NO_ROOT_DIR);
   Check(mounts.FindWindowsSlot('y')==drive&&!mounts.FindWindowsDrive(24),
         "retired Windows slot disappeared from drive reconciliation");
   mounts.SetWindowsType(drive,DRIVE_REMOVABLE);
   Check(priorDrive&&mounts.FindWindowsDrive(24)&&!mounts.IsCurrent(*priorDrive),
         "a Windows drive replacement revived a stale mount address");
   fprintf(report,"PASS: mount slot reuse rejects obsolete classic publications and Windows drive addresses.\n");
  }
  TestNavigation();
  {
   using namespace DocumentLaunch;
   const auto host=FileOperations::Location::Windows(VolumeCoord::Wide((LPCSTR)root)+L"\\File \u65e5.txt");
   Check(WindowsArgument(host)==L"\""+host.WindowsPath()+L"\"",
         "Windows launcher changed its captured path");
   fprintf(report,"PASS: Windows arguments constructed without launching a program.\n");
  }
  fprintf(report,"PASS: stale selection and Find results, worker media validation, per-drive media notifications, and closing during a read.\n");
  auto first=std::make_unique<Panes>();Panes second;auto& a=*first;auto& b=second;
  // Keep the MFC main-window pointer on B while issuing commands to A.
  app.m_pMainWnd=&b.owner;
  auto va=std::make_shared<Fixture>(u"First"),vb=std::make_shared<Fixture>(u"Second");
  int ia=a.Session().MacBrowser().AttachTestVolume(va,"same-key"),ib=b.Session().MacBrowser().AttachTestVolume(vb,"same-key");
  Check(a.Session().MacBrowser().ShowTestFolder(ia,2)&&!va->wrongThread&&a.owner.IsWindowEnabled()&&b.owner.IsWindowEnabled(),"session A navigation blocked another window or used the UI thread");
  Check(b.Session().MacBrowser().ShowTestFolder(ib,2)&&Find(a.list,L"First file")>=0&&Find(b.list,L"Second file")>=0,"native mount/row state leaked between sessions");
  auto selected=b.tree.GetTreeCtrl().GetSelectedItem();int row=Find(a.list,L"Folder");Check(a.Session().MacBrowser().Activate(row)&&a.Session().AwaitNavigationForTest()&&Find(a.list,L"First child")>=0,"independent folder activation");
  Check(b.tree.GetTreeCtrl().GetSelectedItem()==selected&&Find(b.list,L"Second file")>=0,"navigation changed another window");
  auto nestedSelection=a.tree.GetTreeCtrl().GetSelectedItem();
  Check(b.Session().OpenFolderFrom(a.Session(),nestedSelection),"open a mounted folder in another browser");
  PumpUntil([&]{return !b.Session().Navigating();},"second window did not open the mounted folder");
  Check(Find(b.list,L"First child")>=0&&a.tree.GetTreeCtrl().GetSelectedItem()==nestedSelection&&
        b.Session().MacBrowser().TargetFolder().Volume()==va,
        "the second browser did not use the original mount and folder");
  Check(b.Session().MacBrowser().ShowTestFolder(ib,2),"restore the second browser fixture");
  a.list.m_sorted_by_column=FIELD_SIZE;a.list.m_sort_is_ascending=false;a.Session().finderLayout=true;Check(b.list.m_sorted_by_column==FIELD_NAME&&b.list.m_sort_is_ascending&&!b.Session().finderLayout,"sort/layout state shared between windows");a.Session().finderLayout=false;
  a.list.GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);a.list.GetListCtrl().SetItemState(Find(a.list,L"First child"),LVIS_SELECTED,LVIS_SELECTED);
  Check(a.Session().MacBrowser().Capture(false,false),"capture session A selection");Check(a.Session().MacBrowser().TestClipboardCount()==1&&b.Session().MacBrowser().TestClipboardCount()==1,"shared clipboard lost the captured selection");
  va->fail=true;Check(!a.Session().MacBrowser().ShowTestFolder(ia,2)&&!a.Session().ListingAvailable()&&Find(b.list,L"Second file")>=0&&a.owner.IsWindowEnabled()&&b.owner.IsWindowEnabled(),"session failure affected another window or owner");va->fail=false;
  Check(a.Session().MacBrowser().ShowTestFolder(ia,2),"session error retry");
  a.doc.AddIcon('AAAA');b.doc.AddIcon('BBBB');Check(a.doc.HasIcon('AAAA')&&!a.doc.HasIcon('BBBB')&&b.doc.HasIcon('BBBB'),"classic Finder icon tables are shared");
  CString wa=root+"\\first",wb=root+"\\second";Check(CreateDirectoryA(wa,nullptr)&&CreateDirectoryA(wb,nullptr),"create host fixtures");
  {CFile file;Check(file.Open(wa+"\\first.txt",CFile::modeCreate|CFile::modeWrite)!=FALSE,"host first file");}{CFile file;Check(file.Open(wb+"\\second.txt",CFile::modeCreate|CFile::modeWrite)!=FALSE,"host second file");}
  a.Windows(wa);b.Windows(wb);Check(Find(a.list,L"first.txt")>=0&&Find(a.list,L"second.txt")<0&&Find(b.list,L"second.txt")>=0,"Windows directory state used another session");
  {
   auto& session=a.Session();
   auto& browser=session.MacBrowser();
   auto& control=a.tree.GetTreeCtrl();
   const auto original=a.Session().MountRoot({BrowserMountInventory::Kind::WindowsDrive,0});
   const auto child=control.InsertItem("Unrelated folder",original);
   control.Expand(original,TVE_EXPAND);
   const auto selected=control.GetSelectedItem();
   const auto file=Find(a.list,L"first.txt");
   a.list.GetListCtrl().SetItemState(file,LVIS_SELECTED,LVIS_SELECTED);
   struct Gate {
    HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    HANDLE release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    std::atomic<unsigned> probes{0};
    ~Gate(){CloseHandle(entered);CloseHandle(release);}
   };
   const auto gate=std::make_shared<Gate>();
   browser.SetDriveTypeForTest([gate](int drive)->UINT{
    Check(drive==24,"media notice probed an unrelated drive");
    if(++gate->probes==1){
     SetEvent(gate->entered);
     WaitForSingleObject(gate->release,5000);
     return DRIVE_NO_ROOT_DIR;
    }
    return DRIVE_RAMDISK;
   });
   const DWORD mask=1u<<24;
   session.NavigationMediaChanged(mask);
   browser.QueueMediaChanges(mask,0);
   Check(browser.MediaUpdatesPending(),"queue changed drive probe");
   PumpUntil([&]{return WaitForSingleObject(gate->entered,0)==WAIT_OBJECT_0;},"changed drive probe did not enter",false);
   Check(a.owner.IsWindowEnabled()&&control.GetSelectedItem()==selected&&Selected(a.list,L"first.txt"),
         "slow notice blocked browsing or discarded selection");
   // Finish a newer notice while the obsolete probe still has its read lease.
   session.NavigationMediaChanged(mask);
   browser.QueueMediaChanges(mask,mask);
   Check(browser.MediaUpdatesPending(),"queue newer drive arrival");
   PumpUntil([&]{return gate->probes.load()==2;},"newer arrival waited for the old probe",false);
   SetEvent(gate->release);
   PumpUntil([&]{return !browser.MediaUpdatesPending();},"targeted drive change did not finish",false);
   const auto added=session.Mounts().FindWindowsDrive(24);
   Check(added&&a.Session().MountRoot(added->address)&&control.GetSelectedItem()==selected&&
         control.GetChildItem(original)==child&&(control.GetItemState(original,TVIS_EXPANDED)&TVIS_EXPANDED)&&
         Selected(a.list,L"first.txt")&&Find(b.list,L"second.txt")>=0,
         "media change replaced an unrelated root or published an obsolete result");
   browser.SetDriveTypeForTest([](int){return UINT(DRIVE_NO_ROOT_DIR);});
   session.NavigationMediaChanged(mask);
   browser.QueueMediaChanges(mask,0);
   PumpUntil([&]{return !browser.MediaUpdatesPending();},"targeted drive removal did not finish",false);
   Check(!session.Mounts().FindWindowsDrive(24)&&control.GetSelectedItem()==selected&&Selected(a.list,L"first.txt"),
         "targeted removal cleared another drive or its listing");
   auto count=std::make_shared<std::atomic<unsigned>>(0);
   browser.SetDriveTypeForTest([count](int){++*count;return UINT(DRIVE_RAMDISK);});
   {
    VolumeCoord::Lease busy({{VolumeCoord::DeviceKey(24),VolumeCoord::Mode::Write}});
    session.NavigationMediaChanged(mask);
    browser.QueueMediaChanges(mask,mask);
    Check(browser.ApplyMediaChanges()==MainMacBrowser::MediaUpdate::Retry&&
          count->load()==0&&!session.Mounts().FindWindowsDrive(24),
          "busy device launched a probe or published a mount");
   }
   PumpUntil([&]{return !browser.MediaUpdatesPending();},"busy drive was not retried after release");
   Check(count->load()==1&&session.Mounts().FindWindowsDrive(24),"retry lost or duplicated device arrival");

   auto delayed=std::make_shared<Gate>();
   browser.SetDriveTypeForTest([delayed](int){
    if(++delayed->probes==1){
     SetEvent(delayed->entered);
     WaitForSingleObject(delayed->release,5000);
     return UINT(DRIVE_RAMDISK);
    }
    return UINT(DRIVE_NO_ROOT_DIR);
   });
   session.NavigationMediaChanged(mask);
   browser.QueueMediaChanges(mask,mask);
   PumpUntil([&]{return WaitForSingleObject(delayed->entered,0)==WAIT_OBJECT_0;},"slow arrival did not reach its probe");
   session.NavigationMediaChanged(mask);
   browser.QueueMediaChanges(mask,0);
   SetEvent(delayed->release);
   PumpUntil([&]{return !browser.MediaUpdatesPending();},"removal did not supersede a slow arrival");
   Check(delayed->probes.load()==2&&!session.Mounts().FindWindowsDrive(24),
         "slow arrival republished a removed drive");
   browser.SetDriveTypeForTest({});
   fprintf(report,"PASS: device notices probe only named drives, discard stale results, and preserve unrelated roots, expansion and file selection.\n");
  }
  {
   auto& session=a.Session();

   HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
   HANDLE release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
   struct Cleanup{HANDLE entered,release;~Cleanup(){SetEvent(release);BrowserDirectory::testObserveRead={};CloseHandle(release);CloseHandle(entered);}} cleanup{entered,release};
   BrowserDirectory::testObserveRead=[=](const char* path){
    if(wa.CompareNoCase(path)==0){SetEvent(entered);WaitForSingleObject(release,5000);}
   };
   a.list.GetListCtrl().SetItemState(-1,0,LVIS_SELECTED|LVIS_FOCUSED);
   a.list.GetListCtrl().SetItemState(Find(a.list,L"first.txt"),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
   Check(session.RefreshSelected(),"queue asynchronous Windows folder refresh through the shared entry point");
   PumpUntil([&]{return WaitForSingleObject(entered,0)==WAIT_OBJECT_0;},"Windows worker read did not start");
   Check(session.Navigating()&&a.owner.IsWindowEnabled()&&Find(a.list,L"first.txt")>=0,
         "slow Windows refresh disabled its window or cleared the listing");
   SetEvent(release);
   PumpUntil([&]{return !session.Navigating();},"Windows refresh did not finish");
   Check(Selected(a.list,L"first.txt")&&session.ListingAvailable(),"Windows refresh did not publish its listing and restore selection");

  }
  {
   auto nested=wa+"\\Nested\\Deep";
   Check(CreateDirectoryA(wa+"\\Nested",nullptr)&&CreateDirectoryA(nested,nullptr),"create unexpanded Windows search folders");
   auto filename=nested+"\\reveal.txt";
   {CFile file;Check(file.Open(filename,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create Windows search file");}
   auto& session=a.Session();
   auto scope=session.CaptureFindScope();
   Check(scope.has_value(),"capture Windows search scope");
   scope->checkWindowsMedia=true;
   auto hit=SearchHit(*scope,"reveal.txt");
   Check(hit.destination.WindowsMediaSerial().has_value()&&
         hit.destination.WindowsMediaSerial()==hit.reveal.WindowsMediaSerial(),"Find lost its captured Windows media identity");
   auto swapped=hit;
   swapped.destination.BindWindowsMediaSerial(*hit.destination.WindowsMediaSerial()^1u);
   Check(session.RevealFindHit(swapped),"queue Find result for replaced media");
   PumpUntil([&]{return !session.Navigating();},"Find media validation did not complete");
   Check(!session.ListingAvailable()&&session.navigationErrorForTest.find("media changed")!=std::string::npos,
         "Find reopened a folder on different Windows media");
   Check(session.RevealFindHit(hit),"queue Windows Find result");
   PumpUntil([&]{return !session.Navigating();},"Windows Find did not complete");
   Check(Selected(a.list,L"reveal.txt")&&session.MacBrowser().TargetFolder().WindowsPath()==hit.destination.WindowsPath(),
       "Windows Find did not materialize its folder path and select its file");
   auto copy=filename+".replacement";
   {CFile file;Check(file.Open(copy,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create replacement search file");}
   Check(MoveFileExA(copy,filename,MOVEFILE_REPLACE_EXISTING)!=FALSE,"replace the Windows search result");
   hit.mount=session.Mounts().Lookup(hit.mount->address);
   Check(session.RevealFindHit(hit),"queue stale Windows Find result");
   PumpUntil([&]{return !session.Navigating();},"stale Windows Find did not complete");
   Check(!session.ListingAvailable(),"a stale Windows Find result silently rebound to a replacement file");
   a.tree.GetTreeCtrl().SelectItem(a.Session().MountRoot({BrowserMountInventory::Kind::WindowsDrive,0}));
   Check(DeleteFileA(filename)&&RemoveDirectoryA(nested)&&RemoveDirectoryA(wa+"\\\\Nested"),
       "clean up the Windows Find fixture before the startup check");
  }
  {
   const auto folder=VolumeCoord::Wide((LPCSTR)wa)+L"\\\u65e5\u672c";
   const auto file=folder+L"\\\u6587\u4ef6.txt";
   Check(CreateDirectoryW(folder.c_str(),nullptr)!=FALSE,"create Unicode Find folder");
   auto handle=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
   Check(handle!=INVALID_HANDLE_VALUE,"create Unicode Find file");CloseHandle(handle);
   auto& session=a.Session();
   Check(session.AwaitNavigationForTest(),"finish Windows root navigation before Find");
   const auto scope=session.CaptureFindScope();
   Check(scope.has_value(),"capture Unicode search scope");
   auto hit=SearchHit(*scope,"\xe6\x96\x87\xe4\xbb\xb6.*");
   Check(hit.destination.WindowsPath()==folder&&hit.reveal.WindowsPath()==file,"Find changed a Unicode location");
   Check(session.RevealFindHit(hit),"queue Unicode Find result");
   PumpUntil([&]{return !session.Navigating();},"Unicode Find did not complete");
   Check(Selected(a.list,L"\u6587\u4ef6.txt")&&session.MacBrowser().TargetFolder().WindowsPath()==folder,
         "Find did not select the Unicode folder and file");
   const auto child=folder+L"\\Child";
   Check(CreateDirectoryW(child.c_str(),nullptr)!=FALSE,"create child of a Unicode folder");
   Check(session.RefreshSelected()&&session.AwaitNavigationForTest()&&Selected(a.list,L"\u6587\u4ef6.txt"),
         "refresh lost the Unicode selection");
   const auto childRow=a.list.BrowserRowAt(Find(a.list,L"Child"));
   Check(childRow&&childRow->treeItem,"published child has no typed tree binding");
   a.tree.GetTreeCtrl().SelectItem(childRow->treeItem);
   Check(session.AwaitNavigationForTest(),"open the child of the Unicode folder");
   wchar_t label[1024]{};
   LVITEMW cell{};
   cell.pszText=label;
   cell.cchTextMax=ARRAYSIZE(label);
   SendMessageW(a.list.GetListCtrl().GetSafeHwnd(),LVM_GETITEMTEXTW,0,reinterpret_cast<LPARAM>(&cell));
   Check(std::wstring(label)==L" ..up to "+folder,"parent row lost its Unicode path");
   Check(RemoveDirectoryW(child.c_str())&&session.RefreshSelected()&&session.AwaitNavigationForTest()&&
         session.MacBrowser().TargetFolder().WindowsPath()==folder,
         "refresh after external deletion did not publish the Unicode parent");
   const auto selected=a.tree.GetTreeCtrl().GetSelectedItem();
   b.Windows(wa);
   Check(b.Session().OpenFolderFrom(session,selected),"open typed Unicode folder in another window");
   PumpUntil([&]{return !b.Session().Navigating();},"second window did not open the Unicode folder");
   Check(Find(b.list,L"\u6587\u4ef6.txt")>=0&&a.tree.GetTreeCtrl().GetSelectedItem()==selected,
         "the second window changed the source selection or lost the Unicode folder");
   b.tree.GetTreeCtrl().SelectItem(b.Session().MountRoot({BrowserMountInventory::Kind::WindowsDrive,0}));
   a.tree.GetTreeCtrl().SelectItem(a.Session().MountRoot({BrowserMountInventory::Kind::WindowsDrive,0}));
   Check(DeleteFileW(file.c_str())&&RemoveDirectoryW(folder.c_str()),"clean up Unicode Find fixture");
   fprintf(report,"PASS: Unicode Windows Find opens the matching folder and selects the file.\n");
  }
  app.m_pMainWnd=nullptr;a.doc.open_fat_directory(0,const_cast<char*>((LPCSTR)wa),1);app.m_pMainWnd=&b.owner;Check(Find(a.list,L"first.txt")>=0,"startup before global main-window assignment");
  {
   const CString image=root+"\\refresh-classic.dsk", replacement=root+"\\replacement-classic.dsk";
   auto checking=std::make_unique<Panes>();
   checking->Classic(image,"before refresh");
   {Panes seed;seed.Classic(replacement,"replacement file");}
   const auto key=checking->Session().Mounts().ClassicAt(0).ResourceKey();
   auto write=[&](const char16_t* name){
    auto volume=MacBrowser::OpenClassic((LPCSTR)image,":",true);
    MacBrowser::Entry entry;
    entry.name=name;
    volume->Put(2,entry,[](unsigned,uint64_t,void*,size_t){},false);
    volume->Close();
   };
   struct Pause {
    HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    HANDLE release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    std::atomic<unsigned> calls{0};
    ~Pause(){CloseHandle(entered);CloseHandle(release);}
   };
   const auto pause=std::make_shared<Pause>();
   const auto ui=GetCurrentThreadId();
   BrowserDirectory::testObserveRead=[pause,ui](const char* tag){
    if(strcmp(tag,"classic-refresh"))return;
    if(GetCurrentThreadId()==ui)throw std::runtime_error("classic refresh reopened on the UI thread");
    ++pause->calls;
    SetEvent(pause->entered);
    WaitForSingleObject(pause->release,10000);
   };
   struct Release {
    std::shared_ptr<Pause> pause;
    ~Release(){SetEvent(pause->release);BrowserDirectory::testObserveRead={};}
   } release{pause};
   auto start=[&]{
    ResetEvent(pause->entered);
    ResetEvent(pause->release);
    checking->Session().QueueClassicRefresh();
    PumpUntil([&]{return WaitForSingleObject(pause->entered,0)==WAIT_OBJECT_0;},
              "classic refresh did not start on its worker");
   };
   write(u"after refresh");
   start();
   for(int i=0;i<5;++i)checking->Session().QueueClassicRefresh();
   Check(pause->calls==1&&checking->owner.IsWindowEnabled()&&Find(checking->list,L"before refresh")>=0&&
         Find(checking->list,L"after refresh")<0,"the slow classic check replaced the listing or queued duplicates");
   SetEvent(pause->release);
   PumpUntil([&]{return !checking->Session().ClassicRefreshPendingForTest()&&
                       Find(checking->list,L"after refresh")>=0;},"classic change did not refresh the selected folder");

   write(u"interrupted check");
   start();
   Check(MoveFileExA(replacement,image,MOVEFILE_REPLACE_EXISTING)!=FALSE,"replace the classic image during its check");
   SetEvent(pause->release);
   PumpUntil([&]{return !checking->Session().ClassicRefreshPendingForTest();},"stale classic check did not finish");
   Check(Find(checking->list,L"after refresh")>=0&&Find(checking->list,L"replacement file")<0&&
         checking->Session().Mounts().ClassicAt(0).ResourceKey()==key,
         "a check published a replacement it did not capture");
   checking->Session().QueueClassicRefresh();
   PumpUntil([&]{return !checking->Session().ClassicRefreshPendingForTest()&&
                       Find(checking->list,L"replacement file")>=0;},"fresh check did not load the replacement image");
   const auto replacementKey=checking->Session().Mounts().ClassicAt(0).ResourceKey();
   Check(key!=replacementKey,"same-path image replacement kept the old resource identity");

   write(u"close during check");
   start();
   checking.reset();
   SetEvent(pause->release);
   PumpUntil([&]{return VolumeCoord::Readable(replacementKey)&&
                       VolumeCoord::Available({{replacementKey,VolumeCoord::Mode::Write}});},
             "closing a browser left its classic refresh lease held");
   fprintf(report,"PASS: classic change probes coalesce, preserve the listing, reject a replacement during reopen and can outlive a closed browser safely.\n");
  }
  a.Classic(root+"\\first.dsk","first classic");b.Classic(root+"\\second.dsk","second classic");
  a.doc.open_sub_directory(0,2,"Same");Check(Find(a.list,L"first classic")>=0&&Find(a.list,L"second classic")<0&&Find(b.list,L"second classic")>=0,"same-named classic volumes or view bindings shared across sessions");
  a.Classic(root+"\\third.dsk","third classic",1);
  Check(a.doc.FindClassicLauncher("APPL","TEST",1)=="Same:Apps:Tools:third classic app"&&
        a.doc.FindClassicLauncher("APPL","TEST",0)=="Same:Apps:Tools:first classic app",
        "classic launcher resolution lost the originating same-named volume");
  a.doc.my_chdir("Same:apps:tools:",1,1);
  Check(a.Session().Navigating()&&Find(a.list,L"third classic")>=0,"classic path open did not retain its previous listing");
  Check(a.Session().AwaitNavigationForTest(),"wait for named classic folder");
  Check(Find(a.list,L"third classic app")>=0&&Find(a.list,L"first classic app")<0,
        "nested classic folder resolution changed volumes or lost case-insensitive lookup");
  a.tree.GetTreeCtrl().SelectItem(a.Session().MountRoot({BrowserMountInventory::Kind::ClassicVolume,0}));
  Check(a.Session().AwaitNavigationForTest(),"return to first classic image");
  fprintf(report,"PASS: shared classic HFS nested folder and launcher lookup on same-named volumes.\n");
  {
   const CString image=a.doc.Mounts().ClassicAt(0).m_file_name;
   const CString renamed=root+"\\temporarily-renamed.dsk";
   Check(a.doc.Mounts().ClassicAt(0).freeBytes>0&&MoveFileA(image,renamed)&&MoveFileA(renamed,image),
        "classic browser mount should report free space and release its image handle after reading");
  }

  auto classicRoot=a.Session().MountRoot({BrowserMountInventory::Kind::ClassicVolume,0});
  Check(a.Session().RefreshSelected()&&a.Session().Navigating()&&
        Find(a.list,L"first classic")>=0,"classic refresh did not retain the listing");
  PumpUntil([&]{return !a.Session().Navigating();},"classic refresh did not finish");
  Check(Find(a.list,L"first classic")>=0&&a.tree.GetTreeCtrl().GetSelectedItem()==classicRoot,
        "classic asynchronous read changed selection or lost its listing");
  {
   const std::string image=(LPCSTR)a.doc.Mounts().ClassicAt(0).m_file_name;
   auto volume=hfs_mount(const_cast<char*>(image.c_str()),0,O_RDWR);
   char folder[]="Same:Nested",child[]="Same:Nested:found",type[]="TEXT",creator[]="TEST";
   Check(volume&&hfs_mkdir(volume,folder)==0&&hfs_create(volume,child,type,creator)==0&&hfs_umount(volume)==0,
       "create unexpanded classic Find fixture");
   const auto scope=a.Session().CaptureFindScope();
   Check(scope.has_value(),"capture classic HFS search scope");
   auto hit=SearchHit(*scope,"found");
   Check(a.Session().RevealFindHit(hit),"queue classic Find result");
   PumpUntil([&]{return !a.Session().Navigating();},"classic Find did not complete");
   Check(Selected(a.list,L"found"),("classic Find did not select the result: "+
       a.Session().navigationErrorForTest+" (listing available "+std::to_string(a.Session().ListingAvailable())+
       ", rows "+std::to_string(a.list.GetListCtrl().GetItemCount())+")").c_str());
   Check(a.Session().MacBrowser().CaptureItem(Find(a.list,L"found")).expectedCatalogId==hit.reveal.expectedCatalogId,
       "classic file row lost the catalog ID supplied by Find");
   volume=hfs_mount(const_cast<char*>(image.c_str()),0,O_RDWR);
   Check(volume&&hfs_delete(volume,child)==0&&hfs_create(volume,child,type,creator)==0&&hfs_umount(volume)==0,
       "replace classic Find fixture at the same path");
   Check(a.Session().RevealFindHit(hit),"queue replaced classic Find result");
   PumpUntil([&]{return !a.Session().Navigating();},"replaced classic Find did not complete");
   Check(!a.Session().ListingAvailable()&&!a.Session().navigationErrorForTest.empty(),
       "Find rebound the classic catalog ID to a replacement file");
   a.tree.GetTreeCtrl().SelectItem(classicRoot);
   PumpUntil([&]{return !a.Session().Navigating();},"return to classic root after Find");
   Check(a.doc.NavigateStartupDirectory("Same:Nested",true),"queue named classic startup folder");
   PumpUntil([&]{return !a.Session().Navigating();},"classic startup folder did not publish");
   Check(Find(a.list,L"found")>=0&&Find(b.list,L"second classic")>=0,
         "classic startup path opened the wrong folder or volume");
   {
    Panes mirror;
    const auto sourceFolder=a.tree.GetTreeCtrl().GetSelectedItem();
    Check(mirror.Session().OpenFolderFrom(a.Session(),sourceFolder),"open classic folder in another mount inventory");
    PumpUntil([&]{return !mirror.Session().Navigating();},"second classic browser did not finish");
    const auto original=a.Session().Mounts().FindClassicVolume(VolumeCoord::Wide(image));
    const auto duplicate=mirror.Session().Mounts().FindClassicVolume(VolumeCoord::Wide(image));
    Check(original&&duplicate&&a.Session().MountRoot(original->address)!=mirror.Session().MountRoot(duplicate->address)&&Find(mirror.list,L"found")>=0&&
          a.tree.GetTreeCtrl().GetSelectedItem()==sourceFolder,"classic mount inventory shared tree or selection state");
   }
   a.tree.GetTreeCtrl().SelectItem(classicRoot);
   PumpUntil([&]{return !a.Session().Navigating();},"return to classic root after startup folder");
  }

  fprintf(report,"PASS: typed Windows and classic HFS Find materialize an unexpanded folder, reveal the file, and reject a replacement at the same path.\n");
  auto launchA=a.Session().MacBrowser().CaptureItem(Find(a.list,L"first classic"));
  auto launchB=b.Session().MacBrowser().CaptureItem(Find(b.list,L"second classic"));
  auto verifyExport=[&](Panes& origin,const FileOperations::Location& selected,const char* leaf){
   CString staged;
   Check(StageWindowsLaunchDocument(origin.Session(),selected,staged),"could not stage the captured launcher document");
   int separator=staged.ReverseFind('\\');
   bool matches=staged.Mid(separator+1).Left(int(strlen(leaf)))==leaf&&GetFileAttributes(staged)!=INVALID_FILE_ATTRIBUTES;
   Check(matches,"launcher exported a selection from a different browser");
   return staged;
  };
  app.m_pMainWnd=&b.owner;
  auto stagedA=verifyExport(a,launchA,"first classic");
  app.m_pMainWnd=&a.owner;
  auto stagedB=verifyExport(b,launchB,"second classic");
  auto folderA=stagedA.Left(stagedA.ReverseFind('\\'));
  auto folderB=stagedB.Left(stagedB.ReverseFind('\\'));
  DeleteFile(stagedA);
  DeleteFile(stagedB);
  RemoveDirectory(folderA);
  RemoveDirectory(folderB);
  Check(folderA!=folderB,"launcher exports shared a temporary directory");
  a.doc.Mounts().CloseClassic(0);b.doc.Mounts().CloseClassic(0);
  Check(b.Session().MacBrowser().ShowTestFolder(ib,2),"restore B native view");
  bool refused=false;try{BrowserSession invalid;invalid.Attach(&a.owner,&b.tree,&a.list);}catch(const std::logic_error&){refused=true;}Check(refused,"pane binding stolen by another session");
  // Real splitter/frame creation, before MFC publishes m_pMainWnd.
  {Doc startup;Frame frame;CCreateContext context{};context.m_pCurrentDoc=&startup;context.m_pNewViewClass=RUNTIME_CLASS(CHFVExplorerView);
   app.m_pMainWnd=nullptr;Check(frame.LoadFrame(IDR_MAINFRAME,WS_OVERLAPPEDWINDOW,nullptr,&context)!=FALSE,"create real hidden browser frame");
   Check(!frame.IsWindowVisible()&&app.m_pMainWnd==nullptr&&startup.GetBrowserSession().Owner()==&frame,"startup frame/session binding");
   app.m_startup_floppies=FALSE;
   startup.Mounts().AddWindows('Z',DRIVE_REMOVABLE,"Z:\\");
   startup.Mounts().AddWindows(wa[0],DRIVE_FIXED,wa);
   app.m_startup_hds=FALSE;
   const auto invalid=root+"\\invalid-startup.dsk";
   {CFile file;Check(file.Open(invalid,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create invalid startup image");file.SetLength(512);}
   Check(startup.insert_volume(invalid,0,0,0,0)==0,"add invalid discovered image before fixed startup root");
   auto& boot=startup.GetBrowserSession();

   bool probedRemovable=false;
   BrowserDirectory::testObserveRead=[&](const char* path){probedRemovable|=std::string(path).find("Z:")==0;};
   Check(startup.OnNewDocument()!=FALSE&&boot.Navigating()&&!boot.ListingAvailable(),"startup did not queue its first folder");
   PumpUntil([&]{return !boot.Navigating();},"startup Windows folder did not publish");
   BrowserDirectory::testObserveRead={};
   Check(!startup.Mounts().Lookup({BrowserMountInventory::Kind::ClassicVolume,0})&&!probedRemovable&&
         boot.Tree()->GetTreeCtrl().GetSelectedItem()==boot.MountRoot({BrowserMountInventory::Kind::WindowsDrive,1}),
         "startup did not skip the invalid image or probed the disabled floppy");
   Check(boot.List()->GetListCtrl().GetItemCount()==2,"real startup directory listing");
#ifdef HFV_FLOPPY_TESTING
   {std::vector<BYTE> image;std::string why;Check(PrepareFloppyHfs("Manual",image,why),"prepare manual floppy fixture");auto fixture=root+"\\manual-floppy.dsk";{CFile file;Check(file.Open(fixture,CFile::modeCreate|CFile::modeWrite),"save manual floppy fixture");file.Write(image.data(),UINT(image.size()));}
    auto volume=hfs_mount(fixture.GetBuffer(),0,O_RDWR);fixture.ReleaseBuffer();char entry[]="Manual:Readable",type[]="TEXT",creator[]="TEST";Check(volume&&hfs_create(volume,entry,type,creator)==0&&hfs_umount(volume)==0,"populate manual floppy fixture");{CFile file;Check(file.Open(fixture,CFile::modeRead),"load manual floppy fixture");Check(file.Read(image.data(),UINT(image.size()))==image.size(),"read manual floppy fixture");}
    FakeFloppy disk(image);readonly_cd_backend_init();Check(readonly_floppy_attach_api(25,&disk),"attach manual floppy fixture");struct Detach{Doc& doc;~Detach(){doc.Mounts().CloseClassicVolumes();readonly_cd_backend_final();readonly_floppy_attach_api(25,nullptr);}}detach{startup};
    app.m_disable_autofloppy=TRUE;bool windowsProbe=false;BrowserDirectory::testObserveRead=[&](const char* path){windowsProbe|=std::string(path).find("Z:")==0;};startup.GetBrowserSession().Tree()->GetTreeCtrl().SelectItem(startup.GetBrowserSession().MountRoot({BrowserMountInventory::Kind::WindowsDrive,0}));
    PumpUntil([&]{return !startup.GetBrowserSession().Navigating();},"floppy navigation did not finish");
    BrowserDirectory::testObserveRead={};
    auto& session=startup.GetBrowserSession();bool found=false;for(int row=0;row<session.List()->GetListCtrl().GetItemCount();++row){auto binding=session.List()->BrowserRowAt(row);if(binding&&binding->entry.name==u"Readable")found=true;}
    Check(disk.opens>0&&!windowsProbe&&found&&startup.Mounts().ClassicCount()==1&&session.Tree()->GetTreeCtrl().GetSelectedItem()==session.MountRoot({BrowserMountInventory::Kind::ClassicVolume,0}),"explicit floppy selection enumerated Windows before mounting HFS with startup detection disabled");
    const int row=Find(*session.List(),L"Readable");
    auto original=session.MacBrowser().CaptureItem(row);
    Check(original.IsPhysical()&&original.MediaGenerationKnown(),"floppy row did not bind its media generation");
    const auto attachment=session.Mounts().Lookup({BrowserMountInventory::Kind::ClassicVolume,0});
    Check(attachment&&attachment->generation==original.MediaGeneration(),"inventory lost the floppy media generation");
    const auto& mounted=startup.Mounts().ClassicAt(0);
    const MacBrowser::ClassicMountInfo replacement{mounted.m_volume_name.GetString(),mounted.freeBytes,
        original.MediaGeneration()+1};
    startup.Mounts().UpdateClassic(0,replacement);
    Check(!session.Mounts().IsCurrent(*attachment),"inventory accepted a mount with replaced media");
    auto stale=session.MacBrowser().CaptureItem(row);
    auto restored=replacement;
    restored.generation=original.MediaGeneration();
    startup.Mounts().UpdateClassic(0,restored);
    const auto current=session.Mounts().Lookup({BrowserMountInventory::Kind::ClassicVolume,0});
    Check(current&&current->generation==attachment->generation&&!session.Mounts().IsCurrent(*attachment),
          "restoring a media generation revived an obsolete mount address");
    session.NavigationMediaChanged(1u<<25);
    Check(!session.Mounts().IsCurrent(*current),"media notice did not invalidate the captured mount");
    Check(stale.MediaGenerationKnown()&&stale.MediaGeneration()==original.MediaGeneration(),
          "a command rebound a published row to replacement floppy media");
    fprintf(report,"PASS: selecting a simulated Mac floppy with startup/automatic detection disabled mounts HFS and lists its file without attempting Windows folder enumeration.\n");
   }
#endif
   frame.SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);Check(!startup.GetBrowserSession().finderLayout,"frame command routing");
   frame.DestroyWindow();Check(startup.GetBrowserSession().Tree()==nullptr&&startup.GetBrowserSession().List()==nullptr&&startup.GetBrowserSession().Owner()==nullptr,"frame pane lifetime detachment");
  }
  {
   const auto firstImage=VolumeCoord::Wide((LPCSTR)(root+"\\startup-image.dsk"));
   const auto nextImage=VolumeCoord::Wide((LPCSTR)(root+"\\startup-peer.dsk"));
   for(const auto& fixture:std::vector<std::pair<std::wstring,std::u16string>>{
          {firstImage,u"Startup image"},{nextImage,u"Peer image"}}){
    auto image=HfsPlus::CreateImage(fixture.first.c_str(),8*1024*1024);
    HfsPlus::Volume::Format(*image,fixture.second,true);
    image.reset();
    auto volume=MacBrowser::OpenImage(fixture.first);
    const auto child=volume->MakeDirectory(2,fixture.second);
    volume->MakeDirectory(child,u"Restored nested folder");
    volume->Close();
   }

   Doc imageDocument;
   Frame frame;
   CCreateContext context{};
   context.m_pCurrentDoc=&imageDocument;
   context.m_pNewViewClass=RUNTIME_CLASS(CHFVExplorerView);
   app.m_pMainWnd=nullptr;
   Check(frame.LoadFrame(IDR_MAINFRAME,WS_OVERLAPPEDWINDOW,nullptr,&context)!=FALSE,"create image startup frame");
   auto& session=imageDocument.GetBrowserSession();

   imageDocument.Mounts().AddWindows(wa[0],DRIVE_FIXED,wa);
   Check(imageDocument.OnOpenDocument(CString(firstImage.c_str()))&&session.Navigating()&&
         !session.ListingAvailable()&&app.m_pMainWnd==nullptr,"command-line image did not schedule before showing frame");
   PumpUntil([&]{return !session.Navigating();},"startup image did not publish");
   Check(session.ListingAvailable()&&Find(*session.List(),L"Startup image")>=0&&
         session.MountRoot({BrowserMountInventory::Kind::WindowsDrive,0}),"startup image lost its root or the Windows tree");

   HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
   HANDLE release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
   struct Gate {
    HANDLE entered,release;
    ~Gate(){BrowserDirectory::testObserveRead={};SetEvent(release);CloseHandle(release);CloseHandle(entered);}
   } gate{entered,release};
   BrowserDirectory::testObserveRead=[=](const char* tag){
    if(!strcmp(tag,"image-root")){SetEvent(entered);WaitForSingleObject(release,5000);}
   };
   Check(imageDocument.OnOpenDocument(CString(nextImage.c_str())),"queue a delayed image open through the document");
   PumpUntil([&]{return WaitForSingleObject(entered,0)==WAIT_OBJECT_0;},"image root read did not start");
   Check(Find(*session.List(),L"Startup image")>=0&&!session.ListingAvailable()&&frame.IsWindowEnabled(),
         "image opening replaced the previous listing or disabled the frame");
   session.Tree()->GetTreeCtrl().SelectItem(session.MountRoot({BrowserMountInventory::Kind::WindowsDrive,0}));
   PumpUntil([&]{return !session.Navigating();},"folder selection waited for superseded image opening");
   SetEvent(release);
   const auto key=VolumeCoord::ImageKey(nextImage);
   PumpUntil([&]{return VolumeCoord::Available({{key,VolumeCoord::Mode::Write}});},"superseded image retained its lease");
   BrowserDirectory::testObserveRead={};
   const auto& mounts=session.Mounts().Mounted();
   Check(Find(*session.List(),L"first.txt")>=0&&Find(*session.List(),L"Peer image")<0&&
         std::none_of(mounts.begin(),mounts.end(),[&](const auto& mount){
          return mount.volume&&_wcsicmp(mount.key.c_str(),nextImage.c_str())==0;
         }),"superseded image published its listing or attached its mount");

   auto missing=VolumeCoord::Wide((LPCSTR)(root+"\\missing.dsk"));
   Check(session.MacBrowser().OpenImage(missing)==1,"queue missing image");
   PumpUntil([&]{return !session.Navigating();},"missing image did not complete");
   Check(session.ListingAvailable()&&Find(*session.List(),L"first.txt")>=0&&!session.navigationErrorForTest.empty(),
         "failed image open invalidated the previously loaded folder");
   session.MacBrowser().OpenImages({firstImage,missing,nextImage});
   PumpUntil([&]{return !session.Navigating();},"image batch did not complete");
   if(!session.ListingAvailable()||Find(*session.List(),L"Peer image")<0){
    fprintf(report,"Image batch state: %ls, listing available=%d, error=%s\n",
        session.WindowTitle().c_str(),session.ListingAvailable(),session.navigationErrorForTest.c_str());
    throw std::runtime_error("image batch stopped before publishing its last image");
   }

   const auto peerRow=Find(*session.List(),L"Peer image");
   Check(peerRow>=0&&session.MacBrowser().Activate(peerRow)&&session.AwaitNavigationForTest(),
         "browse the nested mounted folder before rebuilding the tree");
   const auto nested=Find(*session.List(),L"Restored nested folder");
   Check(nested>=0&&session.MacBrowser().Activate(nested)&&session.AwaitNavigationForTest(),
         "browse the second mounted tree level");
   const auto before=session.MacBrowser().TargetFolder();
   const auto selectedNode=session.Tree()->BrowserNodeAt(session.Tree()->GetTreeCtrl().GetSelectedItem());
   Check(selectedNode!=nullptr,"nested folder lacks a mount binding");
   const auto preservedMount=session.Mounts().Lookup(selectedNode->mount);
   Check(preservedMount&&session.MountRoot(preservedMount->address),"mounted tree root is missing");
   Check(before.IsMounted()&&before.Id()!=2&&imageDocument.OnOpenDocument("")&&session.Navigating(),
         "mounted tree restoration did not queue navigation");
   PumpUntil([&]{return !session.Navigating();},"mounted tree restoration did not complete");
   const auto after=session.MacBrowser().TargetFolder();
   Check(session.Mounts().IsCurrent(*preservedMount)&&session.MountRoot(preservedMount->address),
         "a tree rebuild invalidated an unchanged mount");
   Check(after.IsMounted()&&after.Volume()==before.Volume()&&after.Id()==before.Id(),
         "mounted tree restoration selected the wrong folder");

   const auto startupPath=wa+"\\Startup Folder";
   Check(CreateDirectoryA(startupPath,nullptr)!=FALSE,"create unexpanded Windows startup folder");
   {CFile file;Check(file.Open(startupPath+"\\Marker",CFile::modeCreate|CFile::modeWrite)!=FALSE,"create startup folder marker");}
   CHFVCommandLineInfo startupArguments;
   startupArguments.ParseParam("@!42 Same:Nested",FALSE,TRUE);
   Check(app.m_opendir.IsEmpty(),"retired browser protocol selected a startup folder");
   startupArguments.ParseParam("@Same:Nested",FALSE,TRUE);
   Check(app.m_opendir=="Same:Nested","classic @directory argument was changed");
   const CString startupArgument=CString("@")+startupPath;
   startupArguments.ParseParam(startupArgument,FALSE,TRUE);
   Check(app.m_opendir==startupPath,"Windows @directory argument lost spaces or changed its path");
   const CString requested=app.m_opendir;
   app.m_opendir.Empty();
   Check(imageDocument.NavigateStartupDirectory((LPCSTR)requested,false),"queue named Windows startup folder");
   PumpUntil([&]{return !session.Navigating();},"named Windows startup folder did not publish");
   Check(Find(*session.List(),L"Marker")>=0&&session.MacBrowser().TargetFolder().IsWindows(),
         "named Windows startup path did not materialize the containing tree");
   frame.DestroyWindow();
   fprintf(report,"PASS: pre-show and subsequent image opens, stale image rejection, previous listing on failure, multi-image continuation and typed mounted tree restoration.\n");
  }
  app.m_pMainWnd=&b.owner;
  first.reset();Check(app.m_pMainWnd==&b.owner&&b.doc.HasIcon('BBBB')&&b.Session().MacBrowser().ShowTestFolder(ib,2)&&Find(b.list,L"Second file")>=0,"closing A invalidated session B");
  fprintf(report,"PASS: two hidden browser sessions with independent mounts, navigation, selection, sort/layout, native clipboard and classic Finder icon tables; Windows folders and same-named classic HFS images; explicit slow-read owner routing, errors/retry, pre-main-window startup, real hidden frame creation/commands/teardown, pane ownership and closing one session while the other stays usable. No physical devices or computer control.\n");
  fprintf(report,"PASS: with floppy detection disabled and an unavailable removable root listed first, startup chooses a fixed root without probing removable media.\n");
 }catch(const std::exception& e){BrowserDirectory::testObserveRead={};fprintf(report,"FAIL: %s\n",e.what());result=1;}
 storageUiTest.releaseAfterTwoTimers=nullptr;app.m_pMainWnd=nullptr;fclose(report);return result;
}
#endif
