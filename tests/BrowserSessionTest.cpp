// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
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
 void AddIcon(unsigned type){local_icn_sharp_t small{},large{};small.icon=large.icon=reinterpret_cast<icn_sharp_hand>(1);insert_into_hash_table(type,'TEST',&small,&large);}
 bool HasIcon(unsigned type){return map_type_and_creator(type,'TEST',0)!=nullptr;}
};
struct Frame:CMainFrame {
 void LoadWindowPosition()override{}
 void SaveWindowPosition()override{}
 void PostNcDestroy()override{}
};
struct Tree:HFVExplorerTreeView {
 void Init(Doc& doc){m_pDocument=&doc;Initialize();enable_selchange(1);}
 HTREEITEM Host(const CString& path){CString label="Windows",copy=path;return initialize_fat_volume(0,&label,DRIVE_FIXED,&copy,FALSE);}
 HTREEITEM Classic(){return initialize_volume(0,"Same",2,0,0,0,0);}
 void Unbind(){m_pDocument=nullptr;}
 void PostNcDestroy()override{}
};
struct List:CHFVExplorerListView {void Bind(Doc* doc){m_pDocument=doc;}void PostNcDestroy()override{};};
std::wstring Text(CListCtrl& list,int row){wchar_t text[1024]{};LVITEMW item{};item.pszText=text;item.cchTextMax=ARRAYSIZE(text);SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));return text;}
int Find(CHFVExplorerListView& list,const wchar_t* name){for(int row=0;row<list.GetListCtrl().GetItemCount();++row)if(Text(list.GetListCtrl(),row)==name)return row;return -1;}
class Fixture:public MacBrowser::Volume {
 std::u16string name_;
public:
 bool fail=false,slow=false,wrongThread=false,ownerOnly=false;CWnd* owner=nullptr;CWnd* other=nullptr;HANDLE gate=CreateEvent(nullptr,TRUE,FALSE,nullptr);
 ~Fixture(){CloseHandle(gate);}
 explicit Fixture(const std::u16string& name):name_(name){}
 std::u16string Name()const override{return name_;}
 const char* Kind()const override{return "Session fixture";}bool Writable()const override{return false;}bool CaseSensitive()const override{return true;}uint64_t FreeBytes()const override{return 0;}
 MacBrowser::Entry Stat(uint32_t id)override{MacBrowser::Entry e;e.id=id;e.parent=id==2?1:id==5?3:2;e.name=id==2?name_:id==3?u"Folder":id==4?name_+u" file":name_+u" child";e.directory=id==2||id==3;return e;}
 std::vector<MacBrowser::Entry> List(uint32_t id)override{
  wrongThread|=GetCurrentThreadId()==StorageUi::state().thread;
  if(slow){Check(WaitForSingleObject(gate,10000)==WAIT_OBJECT_0,"Session read was not released by UI timers");ownerOnly=owner&&!owner->IsWindowEnabled()&&other&&other->IsWindowEnabled();}
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
 void Windows(const CString& path){doc.m_fat_count=1;doc.m_fats[0].m_letter=path[0];doc.m_fats[0].m_type=DRIVE_FIXED;tree.enable_selchange(0);doc.m_fats[0].m_rootitem=tree.Host(path);tree.GetTreeCtrl().SelectItem(doc.m_fats[0].m_rootitem);tree.enable_selchange(1);doc.open_fat_directory(0,const_cast<char*>((LPCSTR)path),1);}
 void Classic(const CString& path,const char* filename){
  {CFile image;Check(image.Open(path,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create session classic image");image.SetLength(2*1024*1024);}
  char label[]="Same",type[]="TEXT",creator[]="TEST";Check(hfs_format(const_cast<char*>((LPCSTR)path),0,label)==0,"format session classic image");
  auto v=hfs_mount(const_cast<char*>((LPCSTR)path),0,O_RDWR);Check(v!=nullptr,"mount session classic image");std::string file="Same:";file+=filename;
  Check(hfs_create(v,&file[0],type,creator)==0&&hfs_umount(v)==0,"populate session classic image");
  Check(doc.insert_volume(path,0,0,0,0)==0,"register session classic volume");
  doc.m_volumes[0].m_volume_name="Same";Check(doc.m_volumes[0].open()!=0,"open session classic volume");
  tree.enable_selchange(0);doc.m_volumes[0].m_rootitem=tree.Classic();tree.GetTreeCtrl().SelectItem(doc.m_volumes[0].m_rootitem);tree.enable_selchange(1);doc.open_sub_directory(0,2,"Same");
 }
};
class NavigationFixture : public Fixture {
public:
 HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
 std::atomic<bool> changed{false};
 explicit NavigationFixture(const std::u16string& name):Fixture(name){}
 ~NavigationFixture(){CloseHandle(entered);}
 std::vector<MacBrowser::Entry> List(uint32_t id)override{
  if(id!=3)return {Stat(3),Stat(4)};
  SetEvent(entered);
  Check(WaitForSingleObject(gate,5000)==WAIT_OBJECT_0,"navigation test read timed out");
  return {Stat(5)};
 }
 void ValidateMedia()override{if(changed)throw HfsPlus::Error("Media changed during navigation");}
};

template<class Predicate>
void PumpUntil(Predicate ready,const char* message)
{
 DWORD start=GetTickCount();
 while(!ready()){
  Check(GetTickCount()-start<8000,message);
  MSG msg;
  while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);}
  Sleep(5);
 }
}

FindHit SearchHit(FindLocation location,const char* pattern)
{
 Storage::Job job;
 auto results=std::make_shared<FindResults>();
 const auto status=MakeFindFilesJob({std::move(location),pattern},results)(job);
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
 session.asyncNavigationForTest=true;
 tree.SelectItem(child);
 PumpUntil([&]{return WaitForSingleObject(slow->entered,0)==WAIT_OBJECT_0;}, "slow folder never started");
 Check(session.Navigating()&&Find(first.list,L"Slow file")>=0&&first.owner.IsWindowEnabled(),
    "pending navigation hid its previous listing or blocked the owner");
 tree.SelectItem(quickRoot);
 PumpUntil([&]{return !session.Navigating();},"replacement navigation waited for the stale read");
 Check(Find(first.list,L"Other file")>=0,"replacement folder did not publish");
 Check(browser.Refresh()&&session.Navigating()&&Find(first.list,L"Other file")>=0,
       "refresh did not use the per-window request or discarded the prior listing");
 PumpUntil([&]{return !session.Navigating();},"asynchronous refresh did not finish");
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
  window.asyncNavigationForTest=true;
  auto& control=pane.tree.GetTreeCtrl();
  control.SelectItem(control.GetChildItem(control.GetSelectedItem()));
  PumpUntil([&]{return WaitForSingleObject(closing->entered,0)==WAIT_OBJECT_0;},"closing read never started");
 }
 SetEvent(closing->gate);
 PumpUntil([&]{return VolumeCoord::Available({{closing->ResourceKey(),VolumeCoord::Mode::Write}});},"closed window retained its read lease");
 Check(Find(first.list,L"Slow child")<0,"closed-window navigation affected another window");
}

}
int RunBrowserSessionTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;CString root=directory;FILE* report=fopen(root+"\\browser-session-report.txt","w");if(!report)return 2;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());app.m_mac_window_mode=0;app.m_dosicons=DI_NO_ICONS;app.m_show_invisible_mac=app.m_show_invisible_fat=1;InitializeStorageUi();
 int result=0;
 try{
  TestNavigation();
  fprintf(report,"PASS: stale selection and Find results, worker media validation, per-drive media notifications, and closing during a read.\n");
  auto first=std::make_unique<Panes>();Panes second;auto& a=*first;auto& b=second;
  // Keep the MFC main-window pointer on B while issuing commands to A.
  app.m_pMainWnd=&b.owner;
  auto va=std::make_shared<Fixture>(u"First"),vb=std::make_shared<Fixture>(u"Second");va->owner=&a.owner;va->other=&b.owner;va->slow=true;
  int ia=a.Session().MacBrowser().AttachTestVolume(va,"same-key"),ib=b.Session().MacBrowser().AttachTestVolume(vb,"same-key");
  storageUiTest={};storageUiTest.releaseAfterTwoTimers=va->gate;Check(a.Session().MacBrowser().ShowTestFolder(ia,2)&&va->ownerOnly&&!va->wrongThread&&storageUiTest.timerTicks>0,"session A worker/owner routing used global window B");storageUiTest.releaseAfterTwoTimers=nullptr;va->slow=false;
  Check(b.Session().MacBrowser().ShowTestFolder(ib,2)&&Find(a.list,L"First file")>=0&&Find(b.list,L"Second file")>=0,"native mount/row state leaked between sessions");
  auto selected=b.tree.GetTreeCtrl().GetSelectedItem();int row=Find(a.list,L"Folder");Check(a.Session().MacBrowser().Activate(row)&&Find(a.list,L"First child")>=0,"independent folder activation");
  Check(b.tree.GetTreeCtrl().GetSelectedItem()==selected&&Find(b.list,L"Second file")>=0,"navigation changed another window");
  a.list.m_sorted_by_column=FIELD_SIZE;a.list.m_sort_is_ascending=false;a.Session().finderLayout=true;Check(b.list.m_sorted_by_column==FIELD_NAME&&b.list.m_sort_is_ascending&&!b.Session().finderLayout,"sort/layout state shared between windows");a.Session().finderLayout=false;
  a.list.GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);a.list.GetListCtrl().SetItemState(Find(a.list,L"First child"),LVIS_SELECTED,LVIS_SELECTED);
  Check(a.Session().MacBrowser().Capture(false,false),"capture session A selection");Check(a.Session().MacBrowser().TestClipboardCount()==1&&b.Session().MacBrowser().TestClipboardCount()==1,"shared clipboard lost the captured selection");
  va->fail=true;Check(!a.Session().MacBrowser().ShowTestFolder(ia,2)&&!a.list.BrowserDirectoryWritable()&&Find(b.list,L"Second file")>=0&&a.owner.IsWindowEnabled()&&b.owner.IsWindowEnabled(),"session failure affected another window or owner");va->fail=false;
  Check(a.Session().MacBrowser().ShowTestFolder(ia,2),"session error retry");
  a.doc.AddIcon('AAAA');b.doc.AddIcon('BBBB');Check(a.doc.HasIcon('AAAA')&&!a.doc.HasIcon('BBBB')&&b.doc.HasIcon('BBBB'),"classic Finder icon tables are shared");
  CString wa=root+"\\first",wb=root+"\\second";Check(CreateDirectoryA(wa,nullptr)&&CreateDirectoryA(wb,nullptr),"create host fixtures");
  {CFile file;Check(file.Open(wa+"\\first.txt",CFile::modeCreate|CFile::modeWrite)!=FALSE,"host first file");}{CFile file;Check(file.Open(wb+"\\second.txt",CFile::modeCreate|CFile::modeWrite)!=FALSE,"host second file");}
  a.Windows(wa);b.Windows(wb);Check(Find(a.list,L"first.txt")>=0&&Find(a.list,L"second.txt")<0&&Find(b.list,L"second.txt")>=0,"Windows directory state used another session");
  {
   auto& session=a.Session();
   session.asyncNavigationForTest=true;
   HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
   HANDLE release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
   struct Cleanup{HANDLE entered,release;~Cleanup(){SetEvent(release);BrowserDirectory::testObserveRead={};CloseHandle(release);CloseHandle(entered);}} cleanup{entered,release};
   BrowserDirectory::testObserveRead=[=](const char* path){
    if(wa.CompareNoCase(path)==0){SetEvent(entered);WaitForSingleObject(release,5000);}
   };
   Check(a.doc.NavigateWindowsFolder(0,(LPCSTR)wa,true),"queue asynchronous Windows folder refresh");
   PumpUntil([&]{return WaitForSingleObject(entered,0)==WAIT_OBJECT_0;},"Windows worker read did not start");
   Check(session.Navigating()&&a.owner.IsWindowEnabled()&&Find(a.list,L"first.txt")>=0,
         "slow Windows refresh disabled its window or cleared the listing");
   SetEvent(release);
   PumpUntil([&]{return !session.Navigating();},"Windows refresh did not finish");
   Check(Find(a.list,L"first.txt")>=0&&session.ListingAvailable(),"Windows refresh did not publish its listing");
   session.asyncNavigationForTest=false;
  }
  {
   auto nested=wa+"\\Nested\\Deep";
   Check(CreateDirectoryA(wa+"\\Nested",nullptr)&&CreateDirectoryA(nested,nullptr),"create unexpanded Windows search folders");
   auto filename=nested+"\\reveal.txt";
   {CFile file;Check(file.Open(filename,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create Windows search file");}
   FindLocation scope;scope.directory=(LPCSTR)wa;
   auto hit=SearchHit(scope,"reveal.txt");
   auto& session=a.Session();
   Check(session.RevealFindHit(hit,0),"queue Windows Find result");
   PumpUntil([&]{return !session.Navigating();},"Windows Find did not complete");
   Check(Selected(a.list,L"reveal.txt")&&session.MacBrowser().TargetFolder().WindowsPath()==hit.destination.WindowsPath(),
       "Windows Find did not materialize its folder path and select its file");
   auto copy=filename+".replacement";
   {CFile file;Check(file.Open(copy,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create replacement search file");}
   Check(MoveFileExA(copy,filename,MOVEFILE_REPLACE_EXISTING)!=FALSE,"replace the Windows search result");
   Check(session.RevealFindHit(hit,0),"queue stale Windows Find result");
   PumpUntil([&]{return !session.Navigating();},"stale Windows Find did not complete");
   Check(!session.ListingAvailable(),"a stale Windows Find result silently rebound to a replacement file");
   a.tree.GetTreeCtrl().SelectItem(a.doc.m_fats[0].m_rootitem);
   Check(DeleteFileA(filename)&&RemoveDirectoryA(nested)&&RemoveDirectoryA(wa+"\\\\Nested"),
       "clean up the Windows Find fixture before the startup check");
  }
  app.m_pMainWnd=nullptr;a.doc.open_fat_directory(0,const_cast<char*>((LPCSTR)wa),1);app.m_pMainWnd=&b.owner;Check(Find(a.list,L"first.txt")>=0,"startup before global main-window assignment");
  a.Classic(root+"\\first.dsk","first classic");b.Classic(root+"\\second.dsk","second classic");
  a.doc.open_sub_directory(0,2,"Same");Check(Find(a.list,L"first classic")>=0&&Find(a.list,L"second classic")<0&&Find(b.list,L"second classic")>=0,"same-named classic volumes or view bindings shared across sessions");
  a.Session().asyncNavigationForTest=true;
  auto classicRoot=a.doc.m_volumes[0].m_rootitem;
  Check(a.doc.NavigateClassicFolder(0,2,true)&&a.Session().Navigating()&&
        Find(a.list,L"first classic")>=0,"classic refresh did not retain the listing");
  PumpUntil([&]{return !a.Session().Navigating();},"classic refresh did not finish");
  Check(Find(a.list,L"first classic")>=0&&a.tree.GetTreeCtrl().GetSelectedItem()==classicRoot,
        "classic asynchronous read changed selection or lost its listing");
  {
   const std::string image=(LPCSTR)a.doc.m_volumes[0].m_file_name;
   auto volume=hfs_mount(const_cast<char*>(image.c_str()),0,O_RDWR);
   char folder[]="Same:Nested",child[]="Same:Nested:found",type[]="TEXT",creator[]="TEST";
   Check(volume&&hfs_mkdir(volume,folder)==0&&hfs_create(volume,child,type,creator)==0&&hfs_umount(volume)==0,
       "create unexpanded classic Find fixture");
   FindLocation scope;scope.mac=true;scope.directory="Same:";scope.volume=image;
   scope.origin=a.Session().MacBrowser().TargetFolder();
   auto hit=SearchHit(scope,"found");
   Check(a.Session().RevealFindHit(hit,0),"queue classic Find result");
   PumpUntil([&]{return !a.Session().Navigating();},"classic Find did not complete");
   Check(Selected(a.list,L"found"),("classic Find did not select the result: "+
       a.Session().navigationErrorForTest+" (listing available "+std::to_string(a.Session().ListingAvailable())+
       ", rows "+std::to_string(a.list.GetListCtrl().GetItemCount())+")").c_str());
   Check(a.Session().MacBrowser().CaptureItem(Find(a.list,L"found")).expectedCatalogId==hit.reveal.expectedCatalogId,
       "classic file row lost the catalog ID supplied by Find");
   volume=hfs_mount(const_cast<char*>(image.c_str()),0,O_RDWR);
   Check(volume&&hfs_delete(volume,child)==0&&hfs_create(volume,child,type,creator)==0&&hfs_umount(volume)==0,
       "replace classic Find fixture at the same path");
   Check(a.Session().RevealFindHit(hit,0),"queue replaced classic Find result");
   PumpUntil([&]{return !a.Session().Navigating();},"replaced classic Find did not complete");
   Check(!a.Session().ListingAvailable()&&!a.Session().navigationErrorForTest.empty(),
       "Find rebound the classic catalog ID to a replacement file");
   a.tree.GetTreeCtrl().SelectItem(classicRoot);
   PumpUntil([&]{return !a.Session().Navigating();},"return to classic root after Find");
   Check(a.doc.NavigateStartupDirectory("Same:Nested",true),"queue named classic startup folder");
   PumpUntil([&]{return !a.Session().Navigating();},"classic startup folder did not publish");
   Check(Find(a.list,L"found")>=0&&Find(b.list,L"second classic")>=0,
         "classic startup path opened the wrong folder or volume");
   a.tree.GetTreeCtrl().SelectItem(classicRoot);
   PumpUntil([&]{return !a.Session().Navigating();},"return to classic root after startup folder");
  }
  a.Session().asyncNavigationForTest=false;
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
  a.doc.m_volumes[0].close();b.doc.m_volumes[0].close();
  Check(b.Session().MacBrowser().ShowTestFolder(ib,2),"restore B native view");
  bool refused=false;try{BrowserSession invalid;invalid.Attach(&a.owner,&b.tree,&a.list);}catch(const std::logic_error&){refused=true;}Check(refused,"pane binding stolen by another session");
  // Real splitter/frame creation, before MFC publishes m_pMainWnd.
  {Doc startup;Frame frame;CCreateContext context{};context.m_pCurrentDoc=&startup;context.m_pNewViewClass=RUNTIME_CLASS(CHFVExplorerView);
   app.m_pMainWnd=nullptr;Check(frame.LoadFrame(IDR_MAINFRAME,WS_OVERLAPPEDWINDOW,nullptr,&context)!=FALSE,"create real hidden browser frame");
   Check(!frame.IsWindowVisible()&&app.m_pMainWnd==nullptr&&startup.GetBrowserSession().Owner()==&frame,"startup frame/session binding");
   app.m_startup_floppies=FALSE;startup.m_fat_count=2;
   startup.m_fats[0].m_letter='Z';startup.m_fats[0].m_type=DRIVE_REMOVABLE;startup.m_fats[0].m_volume_name="Z:\\";
   startup.m_fats[1].m_letter=wa[0];startup.m_fats[1].m_type=DRIVE_FIXED;startup.m_fats[1].m_volume_name=wa;
   app.m_startup_hds=FALSE;
   const auto invalid=root+"\\invalid-startup.dsk";
   {CFile file;Check(file.Open(invalid,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create invalid startup image");file.SetLength(512);}
   Check(startup.insert_volume(invalid,0,0,0,0)==0,"add invalid discovered image before fixed startup root");
   auto& boot=startup.GetBrowserSession();
   boot.asyncNavigationForTest=true;
   bool probedRemovable=false;
   BrowserDirectory::testObserveRead=[&](const char* path){probedRemovable|=std::string(path).find("Z:")==0;};
   Check(startup.OnNewDocument()!=FALSE&&boot.Navigating()&&!boot.ListingAvailable(),"startup did not queue its first folder");
   PumpUntil([&]{return !boot.Navigating();},"startup Windows folder did not publish");
   BrowserDirectory::testObserveRead={};
   Check(startup.m_volumes[0].m_free&&!probedRemovable&&
         boot.Tree()->GetTreeCtrl().GetSelectedItem()==startup.m_fats[1].m_rootitem,
         "startup did not skip the invalid image or probed the disabled floppy");
   Check(boot.List()->GetListCtrl().GetItemCount()==2,"real startup directory listing");
#ifdef HFV_FLOPPY_TESTING
   {std::vector<BYTE> image;std::string why;Check(PrepareFloppyHfs("Manual",image,why),"prepare manual floppy fixture");auto fixture=root+"\\manual-floppy.dsk";{CFile file;Check(file.Open(fixture,CFile::modeCreate|CFile::modeWrite),"save manual floppy fixture");file.Write(image.data(),UINT(image.size()));}
    auto volume=hfs_mount(fixture.GetBuffer(),0,O_RDWR);fixture.ReleaseBuffer();char entry[]="Manual:Readable",type[]="TEXT",creator[]="TEST";Check(volume&&hfs_create(volume,entry,type,creator)==0&&hfs_umount(volume)==0,"populate manual floppy fixture");{CFile file;Check(file.Open(fixture,CFile::modeRead),"load manual floppy fixture");Check(file.Read(image.data(),UINT(image.size()))==image.size(),"read manual floppy fixture");}
    FakeFloppy disk(image);readonly_cd_backend_init();Check(readonly_floppy_attach_api(25,&disk),"attach manual floppy fixture");struct Detach{Doc& doc;~Detach(){for(int i=0;i<doc.m_hfs_count;++i)doc.m_volumes[i].close();readonly_cd_backend_final();readonly_floppy_attach_api(25,nullptr);}}detach{startup};
    app.m_disable_autofloppy=TRUE;startup.GetBrowserSession().asyncNavigationForTest=true;bool windowsProbe=false;BrowserDirectory::testObserveRead=[&](const char* path){windowsProbe|=std::string(path).find("Z:")==0;};startup.GetBrowserSession().Tree()->GetTreeCtrl().SelectItem(startup.m_fats[0].m_rootitem);
    PumpUntil([&]{return !startup.GetBrowserSession().Navigating();},"floppy navigation did not finish");
    BrowserDirectory::testObserveRead={};
    auto& session=startup.GetBrowserSession();bool found=false;for(int row=0;row<session.List()->GetListCtrl().GetItemCount();++row){auto binding=session.List()->BrowserRowAt(row);if(binding&&binding->entry.name==u"Readable")found=true;}
    Check(disk.opens>0&&!windowsProbe&&found&&startup.m_hfs_count==1&&session.Tree()->GetTreeCtrl().GetSelectedItem()==startup.m_volumes[0].m_rootitem,"explicit floppy selection enumerated Windows before mounting HFS with startup detection disabled");
    const int row=Find(*session.List(),L"Readable");
    auto original=session.MacBrowser().CaptureItem(row);
    Check(original.IsPhysical()&&original.MediaGenerationKnown(),"floppy row did not bind its media generation");
    auto& mounted=startup.m_volumes[0];
    ++mounted.m_mediaGeneration;
    auto stale=session.MacBrowser().CaptureItem(row);
    --mounted.m_mediaGeneration;
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
    volume->MakeDirectory(2,fixture.second);
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
   session.asyncNavigationForTest=true;
   imageDocument.m_fat_count=1;
   imageDocument.m_fats[0].m_letter=wa[0];
   imageDocument.m_fats[0].m_type=DRIVE_FIXED;
   imageDocument.m_fats[0].m_volume_name=wa;
   Check(imageDocument.OnOpenDocument(CString(firstImage.c_str()))&&session.Navigating()&&
         !session.ListingAvailable()&&app.m_pMainWnd==nullptr,"command-line image did not schedule before showing frame");
   PumpUntil([&]{return !session.Navigating();},"startup image did not publish");
   Check(session.ListingAvailable()&&Find(*session.List(),L"Startup image")>=0&&
         imageDocument.m_fats[0].m_rootitem,"startup image lost its root or the Windows tree");

   HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr);
   HANDLE release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
   struct Gate {
    HANDLE entered,release;
    ~Gate(){BrowserDirectory::testObserveRead={};SetEvent(release);CloseHandle(release);CloseHandle(entered);}
   } gate{entered,release};
   BrowserDirectory::testObserveRead=[=](const char* tag){
    if(!strcmp(tag,"image-root")){SetEvent(entered);WaitForSingleObject(release,5000);}
   };
   Check(session.MacBrowser().OpenImage(nextImage)==1,"queue a delayed image open");
   PumpUntil([&]{return WaitForSingleObject(entered,0)==WAIT_OBJECT_0;},"image root read did not start");
   Check(Find(*session.List(),L"Startup image")>=0&&!session.ListingAvailable()&&frame.IsWindowEnabled(),
         "image opening replaced the previous listing or disabled the frame");
   session.Tree()->GetTreeCtrl().SelectItem(imageDocument.m_fats[0].m_rootitem);
   PumpUntil([&]{return !session.Navigating();},"folder selection waited for superseded image opening");
   SetEvent(release);
   const auto key=VolumeCoord::ImageKey(nextImage);
   PumpUntil([&]{return VolumeCoord::Available({{key,VolumeCoord::Mode::Write}});},"superseded image retained its lease");
   BrowserDirectory::testObserveRead={};
   Check(Find(*session.List(),L"first.txt")>=0&&Find(*session.List(),L"Peer image")<0,
         "superseded image replaced a newer folder selection");

   auto missing=VolumeCoord::Wide((LPCSTR)(root+"\\missing.dsk"));
   Check(session.MacBrowser().OpenImage(missing)==1,"queue missing image");
   PumpUntil([&]{return !session.Navigating();},"missing image did not complete");
   Check(session.ListingAvailable()&&Find(*session.List(),L"first.txt")>=0&&!session.navigationErrorForTest.empty(),
         "failed image open invalidated the previously loaded folder");
   session.MacBrowser().OpenImages({firstImage,missing,nextImage});
   PumpUntil([&]{return !session.Navigating();},"image batch did not complete");
   Check(session.ListingAvailable()&&Find(*session.List(),L"Peer image")>=0,"image batch stopped at an invalid image");

   const auto startupPath=wa+"\\StartupFolder";
   Check(CreateDirectoryA(startupPath,nullptr)!=FALSE,"create unexpanded Windows startup folder");
   {CFile file;Check(file.Open(startupPath+"\\Marker",CFile::modeCreate|CFile::modeWrite)!=FALSE,"create startup folder marker");}
   Check(imageDocument.NavigateStartupDirectory((LPCSTR)startupPath,false),"queue named Windows startup folder");
   PumpUntil([&]{return !session.Navigating();},"named Windows startup folder did not publish");
   Check(Find(*session.List(),L"Marker")>=0&&session.MacBrowser().TargetFolder().IsWindows(),
         "named Windows startup path did not materialize the containing tree");
   frame.DestroyWindow();
   fprintf(report,"PASS: pre-show image open, stale image rejection, previous listing on failure, and multi-image continuation.\n");
  }
  app.m_pMainWnd=&b.owner;
  first.reset();Check(app.m_pMainWnd==&b.owner&&b.doc.HasIcon('BBBB')&&b.Session().MacBrowser().ShowTestFolder(ib,2)&&Find(b.list,L"Second file")>=0,"closing A invalidated session B");
  fprintf(report,"PASS: two hidden browser sessions with independent mounts, navigation, selection, sort/layout, native clipboard and classic Finder icon tables; Windows folders and same-named classic HFS images; explicit slow-read owner routing, errors/retry, pre-main-window startup, real hidden frame creation/commands/teardown, pane ownership and closing one session while the other stays usable. No physical devices or computer control.\n");
  fprintf(report,"PASS: with floppy detection disabled and an unavailable removable root listed first, startup chooses a fixed root without probing removable media.\n");
 }catch(const std::exception& e){BrowserDirectory::testObserveRead={};fprintf(report,"FAIL: %s\n",e.what());result=1;}
 storageUiTest.releaseAfterTwoTimers=nullptr;app.m_pMainWnd=nullptr;fclose(report);return result;
}
#endif
