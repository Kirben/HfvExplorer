// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "VolumeCoordinator.h"
#include "FileOperations.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "HfsPlusDisk.h"
#include "utils.h"
#include <cstdio>
#include <thread>
#include <fcntl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
}
namespace {
using namespace VolumeCoord;
void Check(bool ok,const char* text){if(!ok)throw HfsPlus::Error(text);}
void Pump(){MSG m;while(PeekMessage(&m,nullptr,0,0,PM_REMOVE)){if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessage(&m);}}}
template<class P>void Until(P ready){DWORD end=GetTickCount()+8000;while(!ready()&&LONG(end-GetTickCount())>0){Pump();MsgWaitForMultipleObjects(0,nullptr,FALSE,15,QS_ALLINPUT);}Pump();Check(ready(),"notification delivery timed out");}
void BriefPump(){DWORD end=GetTickCount()+180;while(LONG(end-GetTickCount())>0){Pump();Sleep(5);}}
struct Doc:CHFVExplorerDoc {Doc():CHFVExplorerDoc(false){m_bAutoDelete=FALSE;}};
struct Tree:HFVExplorerTreeView {
 void Init(Doc& doc){m_pDocument=&doc;Initialize();enable_selchange(1);}void Unbind(){m_pDocument=nullptr;}
 HTREEITEM Host(const CString& path){CString label="Windows",copy=path;return initialize_fat_volume(0,&label,DRIVE_FIXED,&copy,FALSE);}
 HTREEITEM Classic(){return initialize_volume(0,"Classic",2,0,0,0,0);}
 void PostNcDestroy()override{}
};
struct List:CHFVExplorerListView {void Bind(Doc* doc){m_pDocument=doc;}void PostNcDestroy()override{};};
std::wstring Text(CListCtrl& list,int row){wchar_t text[1024]{};LVITEMW item{};item.pszText=text;item.cchTextMax=ARRAYSIZE(text);SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));return text;}
int Find(List& list,const wchar_t* name){for(int i=0;i<list.GetListCtrl().GetItemCount();++i)if(Text(list.GetListCtrl(),i)==name)return i;return -1;}
struct Panes {
 Doc doc;CWnd owner;Tree tree;List list;
 Panes(){Check(owner.CreateEx(0,AfxRegisterWndClass(0),"Hidden coordination browser",WS_OVERLAPPED,0,0,700,400,nullptr,0)!=FALSE,"create hidden browser owner");doc.GetBrowserSession().Attach(&owner,&tree,&list);list.Bind(&doc);Check(tree.Create(nullptr,nullptr,WS_CHILD|TVS_HASLINES,CRect(0,0,200,400),&owner,100,nullptr)&&list.Create(nullptr,nullptr,WS_CHILD|LVS_REPORT,CRect(200,0,700,400),&owner,101,nullptr),"create browser panes");tree.Init(doc);list.Initialize();}
 ~Panes(){tree.enable_selchange(0);list.list_clear();doc.GetBrowserSession().DetachPanes();doc.GetBrowserSession().MacBrowser().Clear();tree.Unbind();list.Bind(nullptr);list.DestroyWindow();tree.DestroyWindow();owner.DestroyWindow();}
 BrowserSession& Session(){return doc.GetBrowserSession();}
 void Native(std::shared_ptr<MacBrowser::Volume> volume){int i=Session().MacBrowser().AttachTestVolume(volume,"coordinated");Check(Session().MacBrowser().ShowTestFolder(i,2),"show native volume");}
 void Enter(const wchar_t* name){int row=Find(list,name);Check(row>=0&&Session().MacBrowser().Activate(row),"open native folder");}
 void Windows(const CString& path){doc.m_fat_count=1;doc.m_fats[0].m_letter=path[0];doc.m_fats[0].m_type=DRIVE_FIXED;doc.m_fats[0].m_volume_name=path;tree.enable_selchange(0);doc.m_fats[0].m_rootitem=tree.Host(path);tree.GetTreeCtrl().SelectItem(doc.m_fats[0].m_rootitem);tree.enable_selchange(1);doc.open_fat_directory(0,const_cast<char*>((LPCSTR)path),1);}
 void Classic(const CString& image){doc.m_hfs_count=1;doc.m_volumes[0].m_file_name=image;doc.m_volumes[0].m_volume_name="Classic";Check(doc.m_volumes[0].open()!=0,"open classic cached reader");tree.enable_selchange(0);doc.m_volumes[0].m_rootitem=tree.Classic();tree.GetTreeCtrl().SelectItem(doc.m_volumes[0].m_rootitem);tree.enable_selchange(1);doc.open_sub_directory(0,2,"Classic");doc.my_chdir("Classic:Folder:",1,0);}
};
std::shared_ptr<MacBrowser::Volume> Image(const std::wstring& path){auto source=HfsPlus::CreateImage(path.c_str(),8*1024*1024);HfsPlus::Volume::Format(*source,u"Coordinated",true,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);source.reset();return MacBrowser::OpenImage(path);}
uint32_t File(MacBrowser::Volume& v,uint32_t parent,const std::u16string& name){MacBrowser::Entry e;e.name=name;e.forks[0].size=32;return v.Put(parent,e,[](unsigned,uint64_t,void* p,size_t n){memset(p,17,n);},false);}
FileOperations::Location Native(std::shared_ptr<MacBrowser::Volume> v,uint32_t id){FileOperations::Location l;l.SetMounted(v,id);return l;}
void Done(const FileOperations::Operation& operation){Check(WaitForSingleObject(operation.Job()->event(),10000)==WAIT_OBJECT_0,"operation timed out");if(operation.Job()->result().code)throw HfsPlus::Error(operation.Job()->result().message);}
uint32_t Selected(List& list){int row=list.GetListCtrl().GetNextItem(-1,LVNI_SELECTED);auto value=row<0?nullptr:list.BrowserRowAt(row);return value?value->id:0;}
void Select(List& list,const wchar_t* name){int row=Find(list,name);Check(row>=0,"selection fixture missing");list.GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);list.GetListCtrl().SetItemState(row,LVIS_SELECTED,LVIS_SELECTED);}
class DeleteFailure:public MacBrowser::Volume {
 std::shared_ptr<MacBrowser::Volume> v_;
public:uint32_t fail;bool failList=false;
 DeleteFailure(std::shared_ptr<MacBrowser::Volume> v,uint32_t id):v_(v),fail(id){}
 std::string ResourceKey()const override{return v_->ResourceKey();}std::u16string Name()const override{return v_->Name();}const char* Kind()const override{return v_->Kind();}bool Writable()const override{return v_->Writable();}bool CaseSensitive()const override{return v_->CaseSensitive();}uint64_t FreeBytes()const override{return v_->FreeBytes();}
 MacBrowser::Entry Stat(uint32_t id)override{return v_->Stat(id);}std::vector<MacBrowser::Entry> List(uint32_t id)override{if(failList)throw HfsPlus::Error("Injected notification read error");return v_->List(id);}void Read(uint32_t id,unsigned f,uint64_t at,void* p,size_t n)override{v_->Read(id,f,at,p,n);}
 uint32_t MakeDirectory(uint32_t id,const std::u16string& n)override{return v_->MakeDirectory(id,n);}uint32_t Put(uint32_t id,const MacBrowser::Entry& e,MacBrowser::Reader r,bool replace)override{return v_->Put(id,e,r,replace);}void Rename(uint32_t id,uint32_t p,const std::u16string& n)override{v_->Rename(id,p,n);}void ValidateRemoval(uint32_t id)override{v_->ValidateRemoval(id);}void Remove(uint32_t id)override{if(id==fail)throw HfsPlus::Error("Injected partial-delete failure");v_->Remove(id);}
};
}
int RunVolumeCoordinationTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;CString root=directory;FILE* report=fopen(root+"\\volume-coordination-report.txt","w");if(!report)return 2;int result=0;auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());app.m_mac_window_mode=0;app.m_dosicons=DI_NO_ICONS;app.m_pre=nullptr;app.m_pStatusBar=nullptr;app.m_show_invisible_fat=app.m_show_invisible_mac=1;InitializeStorageUi();
 try{
  // Atomic lease rules are independent of the currently serialized storage queue.
  {Lease reader({{"lease-A",Mode::Read}});bool read=false,writeBlocked=false,other=false;
   std::thread task([&]{try{Lease two({{"lease-A",Mode::Read}});read=true;}catch(...){}try{Lease write({{"lease-A",Mode::Write},{"lease-B",Mode::Write}});}catch(const Busy&){writeBlocked=true;}try{Lease independent({{"lease-B",Mode::Write}});other=true;}catch(...){}});task.join();Check(read&&writeBlocked&&other,"reader/writer conflict or atomic multi-volume acquisition");}
  {Lease write({{"lease-A",Mode::Read},{"lease-A",Mode::Write}});bool blocked=false;try{Lease read({{"lease-A",Mode::Read}});}catch(const Busy&){blocked=true;}Check(blocked&&!Readable("lease-A")&&Readable("lease-B"),"writer exclusion or unrelated volume availability");}
  std::wstring path=VolumeCoord::Wide(std::string((LPCSTR)(root+"\\shared.dsk")));auto volume=Image(path);auto same=MacBrowser::OpenImage(path);Check(volume==same&&volume->Writable(),"same-process sessions did not share writable native backend");
  std::wstring upper=path;CharUpperBuffW(&upper[0],DWORD(upper.size()));Check(ImageKey(path)==ImageKey(upper)&&MacBrowser::OpenImage(upper)==volume,"case alias has a separate resource/backend");
  auto alias=VolumeCoord::Wide(std::string((LPCSTR)(root+"\\alias.dsk")));if(CreateHardLinkW(alias.c_str(),path.c_str(),nullptr))Check(ImageKey(alias)==ImageKey(path)&&MacBrowser::OpenImage(alias)==volume,"hard-link alias has a separate resource/backend");else fprintf(report,"NOTE: hard-link creation unavailable; case-alias identity verified.\n");
  auto key=volume->ResourceKey();uint32_t one=volume->MakeDirectory(2,u"One"),two=volume->MakeDirectory(2,u"Two"),keep=File(*volume,one,u"Keep"),change=File(*volume,one,u"Change"),other=File(*volume,two,u"Other");
  auto ap=std::make_unique<Panes>(),bp=std::make_unique<Panes>(),up=std::make_unique<Panes>();auto& a=*ap;auto& b=*bp;auto& unrelated=*up;a.Native(volume);a.Enter(L"One");b.Native(same);b.Enter(L"Two");auto separate=Image(VolumeCoord::Wide(std::string((LPCSTR)(root+"\\unrelated.dsk"))));unrelated.Native(separate);app.m_pMainWnd=&unrelated.owner;app.m_doc=&unrelated.doc;app.m_tree=&unrelated.tree;app.m_list=&unrelated.list;
  Select(a.list,L"Keep");Select(b.list,L"Other");a.list.m_sorted_by_column=FIELD_SIZE;a.list.m_sort_is_ascending=FALSE;
  FileOperations::Request rename;rename.SetKind(FileOperations::Kind::Rename);rename.Sources()={Native(volume,change)};rename.Target()=Native(volume,one);rename.name=u"Changed";auto renamed=FileOperations::Submit(rename);Done(renamed);storageUiTest={};Until([&]{return a.Session().notificationRefreshes>=1&&b.Session().notificationRefreshes>=1;});
  Check(storageUiTest.dialogCreations==0&&a.owner.IsWindowEnabled()&&b.owner.IsWindowEnabled(),"notification refresh opened a modal progress dialog or disabled a peer");
  Check(Find(a.list,L"Changed")>=0&&Find(b.list,L"Other")>=0&&Selected(a.list)==keep&&Selected(b.list)==other&&a.list.m_sorted_by_column==FIELD_SIZE&&!a.list.m_sort_is_ascending&&unrelated.Session().notificationRefreshes==0,"refresh changed another session's folder, selection or sort");
  // An explicit initiating-window refresh acknowledges the event once; other
  // sessions still receive their own notification.
  {unsigned own=a.Session().notificationRefreshes,peer=b.Session().notificationRefreshes;rename.name=u"Changed again";Done(FileOperations::Submit(rename));Check(a.Session().MacBrowser().Refresh(),"originating-session manual refresh");Until([&]{return b.Session().notificationRefreshes>peer;});Check(a.Session().notificationRefreshes==own,"origin refreshed twice for one acknowledged change");}
  // Mutation after a snapshot but before first subscription must not get lost.
  {Panes late;int index=late.Session().MacBrowser().AttachTestVolume(volume,"late");late.tree.enable_selchange(0);late.tree.GetTreeCtrl().SelectItem(late.tree.GetTreeCtrl().GetRootItem());late.tree.enable_selchange(1);auto old=Revision(key);Changed({key});late.Session().Observe(key,old,LIST_MAC,index,2);Until([&]{return late.Session().notificationRefreshes>0;});Check(Find(late.list,L"One")>=0,"subscribe-after-read race lost a change");}
  unsigned beforeA=a.Session().notificationRefreshes,beforeB=b.Session().notificationRefreshes;Changed({key});Changed({key,key});Changed({key});Until([&]{return a.Session().notificationRefreshes>beforeA&&b.Session().notificationRefreshes>beforeB;});BriefPump();Check(a.Session().notificationRefreshes==beforeA+1&&b.Session().notificationRefreshes==beforeB+1,"notifications were not coalesced");
  beforeA=a.Session().notificationRefreshes;{Lease held({{key,Mode::Write}});Changed({key});BriefPump();Check(a.Session().notificationRefreshes==beforeA,"refresh ran while a writer held the volume");}Until([&]{return a.Session().notificationRefreshes>beforeA;});
  beforeA=a.Session().notificationRefreshes;++StorageUi::state().active;Changed({key});BriefPump();Check(a.Session().notificationRefreshes==beforeA,"refresh reentered an active storage dialog");--StorageUi::state().active;Until([&]{return a.Session().notificationRefreshes>beforeA;});
  auto revision=Revision(key);{Lease held({{key,Mode::Read}});rename.name=u"Blocked";auto busy=FileOperations::Submit(rename);busy.Job()->wait();Check(busy.Job()->result().error==ERROR_BUSY&&busy.Snapshot().affected.empty()&&Revision(key)==revision,"conflicting file operation mutated/published changes");}
  uint32_t deleted=File(*volume,one,u"Delete first"),retained=File(*volume,one,u"Retain second");auto failing=std::make_shared<DeleteFailure>(volume,retained);FileOperations::Request remove;remove.SetKind(FileOperations::Kind::Delete);remove.Sources()={Native(failing,deleted),Native(failing,retained)};beforeA=a.Session().notificationRefreshes;auto partial=FileOperations::Submit(remove);partial.Job()->wait();Check(partial.Job()->result().code&&partial.Snapshot().completed==1,"partial operation fixture");Until([&]{return a.Session().notificationRefreshes>beforeA;});Check(Find(a.list,L"Delete first")<0&&Find(a.list,L"Retain second")>=0,"partial failure did not refresh actual contents");
  rename.Sources()={Native(volume,one)};rename.Target()=Native(volume,2);rename.name=u"Renamed One";beforeA=a.Session().notificationRefreshes;beforeB=b.Session().notificationRefreshes;Done(FileOperations::Submit(rename));Until([&]{return a.Session().notificationRefreshes>beforeA&&b.Session().notificationRefreshes>beforeB;});
  bool sibling=false;for(auto item=b.tree.GetTreeCtrl().GetChildItem(b.tree.GetTreeCtrl().GetRootItem());item;item=b.tree.GetTreeCtrl().GetNextSiblingItem(item))sibling|=b.tree.GetTreeCtrl().GetItemText(item)=="Renamed One";
  Check(sibling&&Find(a.list,L"Keep")>=0&&Find(b.list,L"Other")>=0,"native sibling tree cache remained stale after a peer folder rename");
  rename.Sources()={Native(volume,two)};rename.Target()=Native(volume,2);rename.name=u"Renamed folder";beforeB=b.Session().notificationRefreshes;Done(FileOperations::Submit(rename));Until([&]{return b.Session().notificationRefreshes>beforeB;});Check(b.tree.GetTreeCtrl().GetItemText(b.tree.GetTreeCtrl().GetSelectedItem())=="Renamed folder"&&Find(b.list,L"Other")>=0,"renamed current folder was not preserved/reflected");
  remove.Sources()={Native(volume,two)};beforeB=b.Session().notificationRefreshes;Done(FileOperations::Submit(remove));Until([&]{return b.Session().notificationRefreshes>beforeB;});Check(Find(b.list,L"Renamed One")>=0&&Find(b.list,L"Renamed folder")<0,"deleted current folder did not fall back safely");
  {auto closing=std::make_unique<Panes>();closing->Native(volume);Changed({key});closing.reset();auto replacement=std::make_unique<Panes>();replacement->Native(volume);Pump();Check(replacement->Session().notificationErrors==0,"closed-session notification hit a later session");}
  {auto ep=std::make_unique<Panes>();auto& errors=*ep;auto transient=std::make_shared<DeleteFailure>(volume,0);errors.Native(transient);transient->failList=true;storageUiTest={};Changed({key});Until([&]{return errors.Session().notificationErrors==1;});Check(errors.list.GetListCtrl().GetItemCount()==0&&!errors.list.BrowserDirectoryWritable()&&storageUiTest.dialogCreations==0,"failed notification read left stale/writable rows or a popup");transient->failList=false;Changed({key});Until([&]{return errors.Session().notificationRefreshes>0;});Check(Find(errors.list,L"Renamed One")>=0,"a subsequent change did not retry a failed notification read");}
  fprintf(report,"PASS: atomic per-volume reader/writer leases, unrelated resources, shared writable mounts through case/hard-link aliases, coalesced UI-thread refreshes, independent folders/selection/sort, busy deferral, failed/partial-operation notifications, current-folder rename/deletion and session closure with queued delivery.\n");fflush(report);
  // Legacy Windows and classic HFS readers receive the same resource events.
  CString windows=root+"\\windows";Check(CreateDirectoryA(windows,nullptr)!=FALSE,"create Windows fixture");for(const char* leaf:{"Keep.txt","Change.txt"}){CFile file;Check(file.Open(windows+"\\"+leaf,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create Windows file");}
  auto wap=std::make_unique<Panes>(),wbp=std::make_unique<Panes>();auto& wa=*wap;auto& wb=*wbp;wa.Windows(windows);wb.Windows(windows);FileOperations::Location host;host.SetWindows(VolumeCoord::Wide(std::string((LPCSTR)(windows+"\\Change.txt"))));rename.Sources()={host};rename.name=u"Changed.txt";Done(FileOperations::Submit(rename));Until([&]{return wa.Session().notificationRefreshes>0&&wb.Session().notificationRefreshes>0;});Check(Find(wa.list,L"Changed.txt")>=0&&Find(wb.list,L"Changed.txt")>=0,"Windows sessions did not refresh");
  {Check(CreateDirectoryA(windows+"\\Branch",nullptr)!=FALSE,"create inactive branch");auto ip=std::make_unique<Panes>();auto& inactive=*ip;inactive.Windows(windows);inactive.Native(separate);auto selected=inactive.tree.GetTreeCtrl().GetSelectedItem();auto nativeRefreshes=inactive.Session().notificationRefreshes;
   host.SetWindows(VolumeCoord::Wide(std::string((LPCSTR)(windows+"\\Branch"))));host.directory=true;rename.Sources()={host};rename.name=u"Renamed branch";unsigned baseline=wa.Session().notificationRefreshes;Done(FileOperations::Submit(rename));Until([&]{return wa.Session().notificationRefreshes>baseline&&inactive.tree.GetTreeCtrl().GetChildItem(inactive.doc.m_fats[0].m_rootitem)==nullptr;});
   Check(inactive.tree.GetTreeCtrl().GetChildItem(inactive.doc.m_fats[0].m_rootitem)==nullptr&&inactive.tree.GetTreeCtrl().GetSelectedItem()==selected&&inactive.Session().notificationRefreshes==nativeRefreshes,"inactive legacy cache invalidation changed the active native view");
   inactive.tree.GetTreeCtrl().SelectItem(inactive.doc.m_fats[0].m_rootitem);Check(Find(inactive.list,L"Renamed branch")>=0&&Find(inactive.list,L"Branch")<0,"inactive Windows root did not refill with fresh paths");
  }
  CString classic=root+"\\classic.dsk";{CFile file;Check(file.Open(classic,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create classic fixture");file.SetLength(2*1024*1024);}char label[]="Classic",type[]="TEXT",creator[]="TEST",folder[]="Classic:Folder",child[]="Classic:Folder:Change";Check(hfs_format(classic.GetBuffer(),0,label)==0,"format classic fixture");classic.ReleaseBuffer();auto v=hfs_mount(classic.GetBuffer(),0,O_RDWR);classic.ReleaseBuffer();Check(v&&hfs_mkdir(v,folder)==0&&hfs_create(v,child,type,creator)==0&&hfs_umount(v)==0,"populate classic fixture");
  auto cap=std::make_unique<Panes>(),cbp=std::make_unique<Panes>();auto& ca=*cap;auto& cb=*cbp;ca.Classic(classic);cb.Classic(classic);Check(Find(ca.list,L"Change")>=0&&Find(cb.list,L"Change")>=0,"classic folder fixture");FileOperations::Location legacy;legacy.SetClassic((LPCSTR)classic,child);rename.Sources()={legacy};rename.name=u"Changed";Done(FileOperations::Submit(rename));Until([&]{return ca.Session().notificationRefreshes>0&&cb.Session().notificationRefreshes>0;});Check(Find(ca.list,L"Changed")>=0&&Find(cb.list,L"Changed")>=0&&ca.Session().notificationErrors==0&&cb.Session().notificationErrors==0,"classic cached readers/folders were not refreshed");
  Check(a.owner.IsWindowEnabled()&&b.owner.IsWindowEnabled()&&wa.owner.IsWindowEnabled()&&wb.owner.IsWindowEnabled(),"notification refresh left an owner disabled");
  fprintf(report,"PASS: Windows folder refresh and classic HFS cached-reader invalidation across two real hidden browser sessions; inactive legacy paths invalidate lazily without changing the active native view. No physical devices or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());FileOperations::CancelPending();result=1;}
 app.m_doc=nullptr;app.m_tree=nullptr;app.m_list=nullptr;app.m_pMainWnd=nullptr;fclose(report);return result;
}
#endif
