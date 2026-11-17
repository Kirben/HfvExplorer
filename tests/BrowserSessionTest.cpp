// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "BrowserDirectory.h"
#include "MainMacBrowser.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "utils.h"
#include "HFVExplorerView.h"
#include "FloppyWriter.h"
#include "ReadOnlyCdBackend.h"
#include "FakeFloppy.h"
#include <cstdio>
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
int Find(List& list,const wchar_t* name){for(int row=0;row<list.GetListCtrl().GetItemCount();++row)if(Text(list.GetListCtrl(),row)==name)return row;return -1;}
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
  doc.m_hfs_count=1;doc.m_volumes[0].m_file_name=path;doc.m_volumes[0].m_volume_name="Same";Check(doc.m_volumes[0].open()!=0,"open session classic volume");
  tree.enable_selchange(0);doc.m_volumes[0].m_rootitem=tree.Classic();tree.GetTreeCtrl().SelectItem(doc.m_volumes[0].m_rootitem);tree.enable_selchange(1);doc.open_sub_directory(0,2,"Same");
 }
};
}
int RunBrowserSessionTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;CString root=directory;FILE* report=fopen(root+"\\browser-session-report.txt","w");if(!report)return 2;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());app.m_pre=nullptr;app.m_pStatusBar=nullptr;app.m_mac_window_mode=0;app.m_dosicons=DI_NO_ICONS;app.m_show_invisible_mac=app.m_show_invisible_fat=1;InitializeStorageUi();
 int result=0;
 try{
  auto first=std::make_unique<Panes>();Panes second;auto& a=*first;auto& b=second;
  // Keep every legacy alias pointed at B while issuing commands to A.
  app.m_doc=&b.doc;app.m_tree=&b.tree;app.m_list=&b.list;app.m_pMainWnd=&b.owner;
  auto va=std::make_shared<Fixture>(u"First"),vb=std::make_shared<Fixture>(u"Second");va->owner=&a.owner;va->other=&b.owner;va->slow=true;
  int ia=a.Session().MacBrowser().AttachTestVolume(va,"same-key"),ib=b.Session().MacBrowser().AttachTestVolume(vb,"same-key");
  storageUiTest={};storageUiTest.releaseAfterTwoTimers=va->gate;Check(a.Session().MacBrowser().ShowTestFolder(ia,2)&&va->ownerOnly&&!va->wrongThread&&storageUiTest.timerTicks>0,"session A worker/owner routing used global window B");storageUiTest.releaseAfterTwoTimers=nullptr;va->slow=false;
  Check(b.Session().MacBrowser().ShowTestFolder(ib,2)&&Find(a.list,L"First file")>=0&&Find(b.list,L"Second file")>=0,"native mount/row state leaked between sessions");
  auto selected=b.tree.GetTreeCtrl().GetSelectedItem();int row=Find(a.list,L"Folder");Check(a.Session().MacBrowser().Activate(row)&&Find(a.list,L"First child")>=0,"independent folder activation");
  Check(b.tree.GetTreeCtrl().GetSelectedItem()==selected&&Find(b.list,L"Second file")>=0,"navigation changed another window");
  a.list.m_sorted_by_column=FIELD_SIZE;a.list.m_sort_is_ascending=false;a.Session().finderLayout=true;Check(b.list.m_sorted_by_column==FIELD_NAME&&b.list.m_sort_is_ascending&&!b.Session().finderLayout,"sort/layout state shared between windows");a.Session().finderLayout=false;
  a.list.GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);a.list.GetListCtrl().SetItemState(Find(a.list,L"First child"),LVIS_SELECTED,LVIS_SELECTED);
  Check(a.Session().MacBrowser().Capture(false,false),"capture session A selection");Check(a.Session().MacBrowser().TestClipboardCount()==1&&b.Session().MacBrowser().TestClipboardCount()==1&&a.list.m_clipboard.GetSize()==0&&b.list.m_clipboard.GetSize()==0,"native clipboard overwrote legacy clipboard");
  va->fail=true;Check(!a.Session().MacBrowser().ShowTestFolder(ia,2)&&!a.list.BrowserDirectoryWritable()&&Find(b.list,L"Second file")>=0&&a.owner.IsWindowEnabled()&&b.owner.IsWindowEnabled(),"session failure affected another window or owner");va->fail=false;
  Check(a.Session().MacBrowser().ShowTestFolder(ia,2),"session error retry");
  a.doc.AddIcon('AAAA');b.doc.AddIcon('BBBB');Check(a.doc.HasIcon('AAAA')&&!a.doc.HasIcon('BBBB')&&b.doc.HasIcon('BBBB'),"classic Finder icon tables are shared");
  CString wa=root+"\\first",wb=root+"\\second";Check(CreateDirectoryA(wa,nullptr)&&CreateDirectoryA(wb,nullptr),"create host fixtures");
  {CFile file;Check(file.Open(wa+"\\first.txt",CFile::modeCreate|CFile::modeWrite)!=FALSE,"host first file");}{CFile file;Check(file.Open(wb+"\\second.txt",CFile::modeCreate|CFile::modeWrite)!=FALSE,"host second file");}
  a.Windows(wa);b.Windows(wb);Check(Find(a.list,L"First.txt")>=0&&Find(a.list,L"Second.txt")<0&&Find(b.list,L"Second.txt")>=0,"Windows directory state used another session");
  app.m_pMainWnd=nullptr;a.doc.open_fat_directory(0,const_cast<char*>((LPCSTR)wa),1);app.m_pMainWnd=&b.owner;Check(Find(a.list,L"First.txt")>=0,"startup before global main-window assignment");
  a.Classic(root+"\\first.dsk","first classic");b.Classic(root+"\\second.dsk","second classic");
  a.doc.open_sub_directory(0,2,"Same");Check(Find(a.list,L"first classic")>=0&&Find(a.list,L"second classic")<0&&Find(b.list,L"second classic")>=0,"same-named classic volumes or view bindings shared across sessions");
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
   bool probedRemovable=false;BrowserDirectory::testObserveRead=[&](const char* path){probedRemovable|=std::string(path).find("Z:")==0;};
   Check(startup.OnNewDocument()!=FALSE,"initialize real browser document");BrowserDirectory::testObserveRead={};
   Check(!probedRemovable&&startup.GetBrowserSession().Tree()->GetTreeCtrl().GetSelectedItem()==startup.m_fats[1].m_rootitem,"startup selected/probed an empty floppy with detection disabled");
startup.open_fat_directory(0,const_cast<char*>((LPCSTR)wa),1);
   Check(startup.GetBrowserSession().List()->GetListCtrl().GetItemCount()==2,"real startup directory listing");
#ifdef HFV_FLOPPY_TESTING
   {std::vector<BYTE> image;std::string why;Check(PrepareFloppyHfs("Manual",image,why),"prepare manual floppy fixture");auto fixture=root+"\\manual-floppy.dsk";{CFile file;Check(file.Open(fixture,CFile::modeCreate|CFile::modeWrite),"save manual floppy fixture");file.Write(image.data(),UINT(image.size()));}
    auto volume=hfs_mount(fixture.GetBuffer(),0,O_RDWR);fixture.ReleaseBuffer();char entry[]="Manual:Readable",type[]="TEXT",creator[]="TEST";Check(volume&&hfs_create(volume,entry,type,creator)==0&&hfs_umount(volume)==0,"populate manual floppy fixture");{CFile file;Check(file.Open(fixture,CFile::modeRead),"load manual floppy fixture");Check(file.Read(image.data(),UINT(image.size()))==image.size(),"read manual floppy fixture");}
    FakeFloppy disk(image);readonly_cd_backend_init();Check(readonly_floppy_attach_api(25,&disk),"attach manual floppy fixture");struct Detach{Doc& doc;~Detach(){for(int i=0;i<doc.m_hfs_count;++i)doc.m_volumes[i].close();readonly_cd_backend_final();readonly_floppy_attach_api(25,nullptr);}}detach{startup};
    app.m_disable_autofloppy=TRUE;bool windowsProbe=false;BrowserDirectory::testObserveRead=[&](const char* path){windowsProbe|=std::string(path).find("Z:")==0;};startup.GetBrowserSession().Tree()->GetTreeCtrl().SelectItem(startup.m_fats[0].m_rootitem);BrowserDirectory::testObserveRead={};
    auto& session=startup.GetBrowserSession();bool found=false;for(int row=0;row<session.List()->GetListCtrl().GetItemCount();++row){auto binding=session.List()->BrowserRowAt(row);if(binding&&binding->entry.name==u"Readable")found=true;}
    Check(disk.opens>0&&!windowsProbe&&found&&startup.m_hfs_count==1&&session.Tree()->GetTreeCtrl().GetSelectedItem()==startup.m_volumes[0].m_rootitem,"explicit floppy selection enumerated Windows before mounting HFS with startup detection disabled");
    fprintf(report,"PASS: selecting a simulated Mac floppy with startup/automatic detection disabled mounts HFS and lists its file without attempting Windows folder enumeration.\n");
   }
#endif
   frame.SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);Check(!startup.GetBrowserSession().finderLayout,"frame command routing");
   frame.DestroyWindow();Check(startup.GetBrowserSession().Tree()==nullptr&&startup.GetBrowserSession().List()==nullptr&&startup.GetBrowserSession().Owner()==nullptr,"frame pane lifetime detachment");
  }
  app.m_doc=&b.doc;app.m_tree=&b.tree;app.m_list=&b.list;app.m_pMainWnd=&b.owner;
  first.reset();Check(app.m_doc==&b.doc&&app.m_tree==&b.tree&&app.m_list==&b.list&&b.doc.HasIcon('BBBB')&&b.Session().MacBrowser().ShowTestFolder(ib,2)&&Find(b.list,L"Second file")>=0,"closing A invalidated session B");
  fprintf(report,"PASS: two hidden browser sessions with independent mounts, navigation, selection, sort/layout, native clipboard and classic Finder icon tables; Windows folders and same-named classic HFS images; explicit slow-read owner routing, errors/retry, pre-main-window startup, real hidden frame creation/commands/teardown, pane ownership and closing one session while the other stays usable. No physical devices or computer control.\n");
  fprintf(report,"PASS: with floppy detection disabled and an unavailable removable root listed first, startup chooses a fixed root without probing removable media.\n");
 }catch(const std::exception& e){BrowserDirectory::testObserveRead={};fprintf(report,"FAIL: %s\n",e.what());result=1;}
 storageUiTest.releaseAfterTwoTimers=nullptr;app.m_doc=nullptr;app.m_tree=nullptr;app.m_list=nullptr;app.m_pMainWnd=nullptr;fclose(report);return result;
}
#endif
