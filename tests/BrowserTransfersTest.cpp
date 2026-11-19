// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "WorkerEndpoint.h"
#include "HFVExplorer.h"
#include "BackgroundCopies.h"
#include "BrowserSession.h"
#include "BrowserPresentation.h"
#include "BrowserTransfers.h"
#include "MainMacBrowser.h"
#include "HfsPlusDisk.h"
#include "StorageJobDialog.h"
#include "CopyProcess.h"
#include "VolumeSession.h"
#include "utils.h"
#include <cstdio>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
}

namespace {
using Choice=Storage::ConflictChoice;
CString host;

void Check(bool ok,const std::string& message)
{
 if(!ok)throw HfsPlus::Error(message.c_str());
}

class TransferDoc:public CHFVExplorerDoc {
 DECLARE_DYNCREATE(TransferDoc)
public:
 using CHFVExplorerDoc::NavigateWindowsFolder;
 TransferDoc():CHFVExplorerDoc(false)
 {
  Mounts().AddWindows(host[0],DRIVE_FIXED,host);
 }
};
IMPLEMENT_DYNCREATE(TransferDoc,CHFVExplorerDoc)

class TransferFrame:public CMainFrame {
 DECLARE_DYNCREATE(TransferFrame)
public:
 void LoadWindowPosition()override{}
 void SaveWindowPosition()override{}
 void ActivateFrame(int)override{}
};
IMPLEMENT_DYNCREATE(TransferFrame,CMainFrame)

std::wstring Wide(const CString& path){return VolumeCoord::Wide((LPCSTR)path);}

int Row(BrowserSession& session,const wchar_t* name)
{
 auto& list=session.List()->GetListCtrl();
 for(int i=0;i<list.GetItemCount();++i){
  const auto row=session.List()->BrowserRowAt(i);
  if(!row)continue;
  const auto leaf=MacBrowser::Wide(row->entry.name);
  if(row->location.IsWindows()?!_wcsicmp(leaf.c_str(),name):leaf==name)return i;
 }
 return -1;
}

void Select(BrowserSession& session,const wchar_t* name)
{
 Check(session.AwaitNavigationForTest()&&session.ListingAvailable(),"selection listing is still loading");
 const auto index=Row(session,name);
 auto& list=session.List()->GetListCtrl();
 if(index<0){
  std::string visible;
  for(int i=0;i<list.GetItemCount();++i)
   if(const auto row=session.List()->BrowserRowAt(i))
    visible+=" ["+BrowserPresentation::Utf8(row->entry.name)+"]";
  throw HfsPlus::Error(("selection fixture is absent: "+BrowserPresentation::Utf8(MacBrowser::U16(name))+
       "; visible rows:"+visible).c_str());
 }
 list.SetItemState(-1,0,LVIS_SELECTED);
 list.SetItemState(index,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
}

uint32_t Find(MacBrowser::Volume& volume,uint32_t parent,const std::u16string& name)
{
 for(const auto& e:volume.List(parent))if(e.name==name)return e.id;
 return 0;
}

uint32_t Put(MacBrowser::Volume& volume,uint32_t parent,const std::u16string& name,unsigned seed=17)
{
 MacBrowser::Entry e;
 e.name=name;
 e.forks[0].size=29;
 e.forks[1].size=13;
 e.finder[0]='T';
 e.finder[4]='X';
 return volume.Put(parent,e,[seed](unsigned fork,uint64_t at,void* p,size_t n){
  for(size_t i=0;i<n;++i)static_cast<unsigned char*>(p)[i]=BYTE(seed+fork+at+i);
 },false);
}

void Verify(MacBrowser::Volume& volume,uint32_t parent,const std::u16string& name,unsigned seed=17)
{
 const auto label=BrowserPresentation::Utf8(name);
 const auto id=Find(volume,parent,name);
 Check(id!=0,"copied entry is missing: "+label);
 const auto e=volume.Stat(id);
 Check(e.forks[0].size==29&&e.forks[1].size==13&&e.finder[0]=='T'&&e.finder[4]=='X',
       "fork sizes or Finder metadata were lost: "+label);
 for(unsigned fork=0;fork<2;++fork){
  unsigned char bytes[29]{};
  volume.Read(id,fork,0,bytes,size_t(e.forks[fork].size));
  for(unsigned i=0;i<e.forks[fork].size;++i)
   Check(bytes[i]==BYTE(seed+fork+i),"copied fork bytes differ: "+label);
 }
}

void FormatImage(const CString& path)
{
 const auto wide=Wide(path);
 auto disk=HfsPlus::CreateImage(wide.c_str(),8*1024*1024);
 HfsPlus::Volume::Format(*disk,u"Transfer",true,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
}

void FormatClassic(const CString& path)
{
 CFile file;
 Check(file.Open(path,CFile::modeCreate|CFile::modeWrite),"create classic fixture");
 file.SetLength(2*1024*1024);
 file.Close();
 char label[]="Legacy";
 Check(hfs_format(const_cast<char*>((LPCSTR)path),0,label)==0,"format classic fixture");
}

std::shared_ptr<MacBrowser::Volume> Mount(BrowserSession& session,const CString& path)
{
 Check(session.MacBrowser().OpenImage(Wide(path))==1&&session.AwaitNavigationForTest(),"open browser image fixture");
 auto volume=session.MacBrowser().TargetFolder().Volume();
 Check(volume&&MacBrowser::ExecutionFor(*volume).Persistent(),"browser image is not owned by a persistent worker");
 return volume;
}

void Refresh(BrowserSession& session)
{
 Check(session.MacBrowser().Refresh()&&session.AwaitNavigationForTest(),"refresh mounted browser fixture");
}

void Host(BrowserSession& session,const CString& path)
{
 auto doc=session.Document();
 Check(doc->OnOpenDocument(""),"rebuild browser host tree");
 session.Tree()->GetTreeCtrl().SelectItem(doc->Mounts().WindowsAt(0).m_rootitem);
 Check(static_cast<TransferDoc*>(doc)->NavigateWindowsFolder(0,std::string(path.GetString()))&&
       session.AwaitNavigationForTest(),"open host folder through browser navigation");
}

template<class Predicate>
void Until(Predicate ready,const char* failure)
{
 const DWORD start=GetTickCount();
 while(!ready()){
  Check(GetTickCount()-start<10000,failure);
  MSG message;
  while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){
   if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessage(&message);}
  }
  Sleep(5);
 }
}

template<class Start>
Storage::Result Transfer(Start start,Choice choice=Choice::None)
{
 Check(!BackgroundCopies::Active(),"previous transfer still active");
 storageUiTest={};
 Check(start()&&BackgroundCopies::Active(),"browser did not queue the transfer");
 if(choice!=Choice::None){
  std::shared_ptr<Storage::Job> conflict;
  Until([&]{
   for(const auto& pending:FileOperations::Pending())
    if(!pending.Job()->conflict().empty()){conflict=pending.Job();return true;}
   return !BackgroundCopies::Active();
  },"timed out waiting for an overwrite conflict");
  Check(bool(conflict),"transfer finished before presenting an overwrite conflict");
  storageUiTest.answer=choice;
 }
 Until([]{return !BackgroundCopies::Active();},"background transfer did not finish");
 return BackgroundCopies::LastResult();
}

Storage::Result Paste(CMainFrame& frame,Choice choice=Choice::None)
{
 auto& session=frame.GetBrowserSession();
 Check(session.AwaitNavigationForTest()&&session.ListingAvailable(),"paste destination is still loading");
 return Transfer([&]{frame.SendMessage(WM_COMMAND,ID_EDIT_PASTE);return true;},choice);
}

void Success(const Storage::Result& result)
{
 Check(!result.code,result.message.empty()?"transfer failed":result.message);
}

class ReadOnly:public MacBrowser::Volume {
 std::shared_ptr<MacBrowser::Volume> base_;
public:
 explicit ReadOnly(std::shared_ptr<MacBrowser::Volume> base):base_(std::move(base)){}
 std::string ResourceKey()const override{return base_->ResourceKey();}
 std::u16string Name()const override{return u"Read-only fixture";}
 const char* Kind()const override{return "Read-only fixture";}
 bool Writable()const override{return false;}
 bool CaseSensitive()const override{return true;}
 uint64_t FreeBytes()const override{return 0;}
 MacBrowser::Entry Stat(uint32_t id)override{return base_->Stat(id);}
 std::vector<MacBrowser::Entry> List(uint32_t id)override{return base_->List(id);}
 void Read(uint32_t id,unsigned f,uint64_t at,void* p,size_t n)override{base_->Read(id,f,at,p,n);}
 uint32_t MakeDirectory(uint32_t,const std::u16string&)override{throw HfsPlus::Error("Read-only mutation");}
 uint32_t Put(uint32_t,const MacBrowser::Entry&,MacBrowser::Reader,bool)override{throw HfsPlus::Error("Read-only mutation");}
 void Rename(uint32_t,uint32_t,const std::u16string&)override{throw HfsPlus::Error("Read-only mutation");}
 void Remove(uint32_t)override{throw HfsPlus::Error("Read-only mutation");}
};
}

int RunBrowserTransfersTest(const char* directory)
{
 if(!CreateDirectoryA(directory,nullptr))return 2;
 host=directory;
 FILE* report=fopen(host+"\\browser-transfers-report.txt","w");
 if(!report)return 2;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 InitializeStorageUi();
 app.m_dosicons=DI_NO_ICONS;
 app.m_mac_window_mode=FALSE;
 app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 app.m_copyin_mode=app.m_copyout_mode=0;
 BrowserTransfers::Clear();
 BackgroundCopies::enabled=true;
 int result=0;
 const char* stage="mount browser fixtures";
 try{
  const auto ap=host+"\\a.dsk",bp=host+"\\b.dsk";
  FormatImage(ap);
  FormatImage(bp);
  app.InstallBrowserTemplate(RUNTIME_CLASS(TransferDoc),RUNTIME_CLASS(TransferFrame));
  auto a=app.CreateBrowser(FALSE),b=app.CreateBrowser(FALSE);
  Check(a&&b&&!a->IsWindowVisible()&&!b->IsWindowVisible(),"create hidden transfer frames");
  auto& sa=a->GetBrowserSession();
  auto& sb=b->GetBrowserSession();
  auto av=Mount(sa,ap),bv=Mount(sb,bp);
  const auto aPid=VolumeSession::VolumeSessionPidForTest(av);
  const auto bPid=VolumeSession::VolumeSessionPidForTest(bv);
  Check(aPid&&bPid&&aPid!=GetCurrentProcessId()&&bPid!=aPid,"fixtures must use two browser worker processes");
  Put(*av,2,u"Payload");
  const auto folder=bv->MakeDirectory(2,u"Destination");
  Refresh(sa);
  Refresh(sb);

  stage="shared copy and cut";
  Select(sa,L"Payload");
  a->SendMessage(WM_COMMAND,ID_EDIT_COPY);
  auto clipboard=BrowserTransfers::Snapshot();
  Check(clipboard.items.size()==1&&clipboard.items.front().Volume()==av,"Copy did not capture the browser mount");
  Success(Paste(*b));
  Verify(*bv,2,u"Payload");
  Check(Find(*av,2,u"Payload")&&BrowserTransfers::Any(),"copy removed its source or clipboard");

  Put(*av,2,u"Move",33);
  Put(*bv,2,u"Move",99);
  Refresh(sa);
  Select(sa,L"Move");
  a->SendMessage(WM_COMMAND,ID_EDIT_CUT);
  const auto cancelled=Paste(*b,Choice::Cancel);
  Check(cancelled.error==ERROR_CANCELLED,"overwrite cancel was not reported as cancellation");
  Verify(*av,2,u"Move",33);
  Verify(*bv,2,u"Move",99);
  clipboard=BrowserTransfers::Snapshot();
  Check(clipboard.items.size()==1&&clipboard.items.front().cut,"cancelled paste consumed Cut");
  Paste(*b,Choice::Skip);
  Verify(*av,2,u"Move",33);
  Verify(*bv,2,u"Move",99);
  Check(BrowserTransfers::Any(),"skipped Cut consumed the clipboard");
  Success(Paste(*b,Choice::Replace));
  Verify(*bv,2,u"Move",33);
  Check(!Find(*av,2,u"Move")&&!BrowserTransfers::Any(),"completed Cut was not removed from source and clipboard");

  stage="cross-window folder-row and tree drops";
  Refresh(sa);
  Select(sa,L"Payload");
  a->SendMessage(WM_COMMAND,ID_EDIT_COPY);
  a->SendMessage(WM_COMMAND,ID_EDIT_COPY_APPEND);
  clipboard=BrowserTransfers::Snapshot();
  Check(clipboard.items.size()==1,"Copy append duplicated an entry");
  const auto saved=clipboard.items.front().Id();
  Put(*av,2,u"Dragged",45);
  Put(*bv,folder,u"Dragged",98);
  Refresh(sa);
  Select(sa,L"Dragged");
  BrowserTransfers::Drag drag(sa);
  auto dropOnRow=[&]{return drag.TestDrop(sb,false,nullptr,Row(sb,L"Destination"),MK_CONTROL);};
  Check(Transfer(dropOnRow,Choice::Cancel).error==ERROR_CANCELLED,"drag overwrite cancel was not reported");
  Verify(*bv,folder,u"Dragged",98);
  Transfer(dropOnRow,Choice::Skip);
  Verify(*bv,folder,u"Dragged",98);
  Success(Transfer(dropOnRow,Choice::Replace));
  Verify(*bv,folder,u"Dragged",45);
  Verify(*av,2,u"Dragged",45);
  clipboard=BrowserTransfers::Snapshot();
  Check(clipboard.items.size()==1&&clipboard.items.front().Id()==saved,"drag changed the shared clipboard");

  Put(*av,2,u"Tree move",50);
  Refresh(sa);
  Select(sa,L"Tree move");
  BrowserTransfers::Drag treeDrag(sa);
  const auto destinationRow=sb.List()->BrowserRowAt(Row(sb,L"Destination"));
  Check(destinationRow&&destinationRow->treeItem,"missing typed tree-folder binding");
  const auto treeItem=destinationRow->treeItem;
  Success(Transfer([&]{return treeDrag.TestDrop(sb,true,treeItem,-1,MK_SHIFT);}));
  Verify(*bv,folder,u"Tree move",50);
  Check(!Find(*av,2,u"Tree move"),"Shift drop did not move source");

  stage="stale selections and drop policy";
  Refresh(sa);
  Select(sa,L"Payload");
  BrowserTransfers::Drag policy(sa);
  Check(!policy.TestDrop(sb,false,nullptr,Row(sb,L"Payload"),MK_CONTROL)&&!BackgroundCopies::Active(),
        "regular file accepted a folder drop");
  auto selection=sa.MacBrowser().SelectedItems(false);
  const auto target=sb.MacBrowser().TargetFolder();
  Check(BrowserTransfers::Choose(selection,target,true,0)==BrowserTransfers::Effect::Copy&&
        BrowserTransfers::Choose(selection,sa.MacBrowser().TargetFolder(),true,0)==BrowserTransfers::Effect::Move,
        "incorrect default effect across distinct or shared mounts");
  selection.canMove=false;
  Check(BrowserTransfers::Choose(selection,target,true,MK_SHIFT)==BrowserTransfers::Effect::None&&
        BrowserTransfers::Choose(selection,target,true,0)==BrowserTransfers::Effect::Copy&&
        BrowserTransfers::Choose(selection,target,false,MK_CONTROL)==BrowserTransfers::Effect::None,
        "read-only move or target policy");

  const auto original=Put(*av,2,u"Stale",51);
  Refresh(sa);
  Select(sa,L"Stale");
  BrowserTransfers::Drag stale(sa);
  a->SendMessage(WM_COMMAND,ID_EDIT_CUT);
  av->Rename(original,2,u"Renamed");
  const auto replacement=Put(*av,2,u"Stale",87);
  Check(original!=replacement,"replacement fixture reused the original catalog ID");
  Check(Transfer([&]{return stale.TestDrop(sb,false,nullptr,-1,MK_CONTROL);}).code!=0,
        "a stale drag was reported as successful");
  Check(Paste(*b).code!=0,"a stale Cut was reported as successful");
  Check(!Find(*bv,2,u"Stale")&&!Find(*bv,2,u"Renamed"),"stale transfer copied an entry");
  Verify(*av,2,u"Renamed",51);
  Verify(*av,2,u"Stale",87);
  clipboard=BrowserTransfers::Snapshot();
  Check(clipboard.items.size()==1&&clipboard.items.front().cut&&clipboard.items.front().Id()==original,
        "rejected stale Cut consumed or rebound the clipboard");
  fprintf(report,"PASS: cross-window folder-row and tree drops, Cancel/Skip/Replace, both forks and metadata, and stale drag and Cut after replacement.\n");

  stage="shared mounted image and window lifetime";
  const auto outer=av->MakeDirectory(2,u"Outer"),inner=av->MakeDirectory(outer,u"Inner");
  Refresh(sa);
  Select(sa,L"Outer");
  BrowserTransfers::Drag descendant(sa);
  Check(Mount(sb,ap)==av&&sb.MacBrowser().Activate(Row(sb,L"Outer"))&&sb.AwaitNavigationForTest(),"two browsers did not share the same image mount");
  const auto innerRow=Row(sb,L"Inner");
  Check(innerRow>=0&&Transfer([&]{return descendant.TestDrop(sb,false,nullptr,innerRow,MK_CONTROL);}).code,
        "cross-window descendant copy was accepted");
  Check(!Find(*av,inner,u"Outer"),"descendant rejection left an entry");
  const auto withinId=Put(*av,2,u"Within",71);
  Refresh(sa);
  Select(sa,L"Within");
  BrowserTransfers::Drag within(sa);
  Success(Transfer([&]{return within.TestDrop(sb,false,nullptr,-1,0);}));
  Verify(*av,outer,u"Within",71);
  Check(!Find(*av,2,u"Within")&&Find(*av,outer,u"Within")==withinId,
        "default same-volume drop did not move the original catalog entry");

  Until([&]{return Row(sb,L"Within")>=0;},"same-image move did not refresh the nested listing");
  const auto parentRow=[&]{
   for(int i=0;i<sb.List()->GetListCtrl().GetItemCount();++i)
    if(sb.List()->IsParentRow(i))return i;
   return -1;
  }();
  Check(parentRow>=0,"nested folder has no typed parent drop row");
  Select(sb,L"Within");
  BrowserTransfers::Drag toParent(sb);
  Success(Transfer([&]{return toParent.TestDrop(sb,false,nullptr,parentRow,0);}));
  Verify(*av,2,u"Within",71);
  Check(!Find(*av,outer,u"Within")&&Find(*av,2,u"Within")==withinId,
        "same-window parent drop did not move the captured file back to its parent");

  stage="same-image overwrite move";
  Put(*av,2,u"Collision",44);
  Put(*av,outer,u"Collision",92);
  Refresh(sa);
  Select(sa,L"Collision");
  BrowserTransfers::Drag collision(sa);
  auto dropWithin=[&]{return collision.TestDrop(sb,false,nullptr,-1,0);};
  Check(Transfer(dropWithin,Choice::Cancel).error==ERROR_CANCELLED,"same-image overwrite move did not cancel");
  Verify(*av,2,u"Collision",44);
  Verify(*av,outer,u"Collision",92);
  Success(Transfer(dropWithin,Choice::Replace));
  Verify(*av,outer,u"Collision",44);
  Check(!Find(*av,2,u"Collision"),"same-image overwrite move retained the source after success");

  stage="source window closure";
  Check(Mount(sb,bp)==bv&&VolumeSession::VolumeSessionPidForTest(av)==aPid&&
        VolumeSession::VolumeSessionPidForTest(bv)==bPid,"transfer replaced a mounted worker");

  Select(sa,L"Payload");
  a->SendMessage(WM_COMMAND,ID_EDIT_COPY);
  BrowserTransfers::Drag closing(sa);
  a->SendMessage(WM_CLOSE);
  Check(!closing.TestDrop(sb,false,nullptr,-1,MK_CONTROL)&&!BackgroundCopies::Active(),
        "drag survived destruction of its source window");
  const auto after=bv->MakeDirectory(2,u"After close");
  Refresh(sb);
  Check(sb.MacBrowser().Activate(Row(sb,L"After close"))&&sb.AwaitNavigationForTest(),"show post-close folder");
  Success(Paste(*b));
  Verify(*bv,after,u"Payload");

  stage="Windows targets and read-only source";
  auto c=app.CreateBrowser(FALSE);
  Check(c!=nullptr,"create third browser");
  auto& sc=c->GetBrowserSession();
  clipboard=BrowserTransfers::Snapshot();
  Check(clipboard.items.size()==1&&clipboard.items.front().Volume()==av,"new browser erased the shared clipboard");
  CString source=host+"\\host-source",dest=host+"\\host-dest";
  Check(CreateDirectoryA(source,nullptr)&&CreateDirectoryA(dest,nullptr),"create host folders");
  {
   CFile file;
   Check(file.Open(source+"\\sample.txt",CFile::modeCreate|CFile::modeWrite),"create host file");
   file.Write("hello",5);
  }
  Host(sc,source);
  Host(sb,dest);
  Select(sc,L"Sample.txt");
  c->SendMessage(WM_COMMAND,ID_EDIT_COPY_APPEND);
  Check(BrowserTransfers::Snapshot().items.size()==2,"mixed-format Copy append failed");
  Success(Paste(*b));
  Check(GetFileAttributesA(dest+"\\sample.txt")!=INVALID_FILE_ATTRIBUTES,"host cross-window paste failed");
  Verify(*MacBrowser::OpenWindows(Wide(dest)),2,u"Payload");
  Check(Mount(sb,bp)==bv,"reselect native import target");
  Success(Paste(*b,Choice::Replace));
  Check(Find(*bv,2,u"sample.txt")!=0,"Windows-to-native shared paste failed");

  stage="Windows selection with unavailable identity";
  {
   const auto path=source+"\\locked.txt";
   const auto wide=Wide(path);
   const auto handle=CreateFileW(wide.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
   Check(handle!=INVALID_HANDLE_VALUE,"create locked-file fixture");
   struct Lock {HANDLE value;~Lock(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} locked{handle};
   Host(sc,source);
   const auto row=sc.List()->BrowserRowAt(Row(sc,L"locked.txt"));
   Check(row&&row->location.IsWindows()&&row->location.requiresWindowsIdentity,"Windows row had no capture guard");
   BrowserDirectory::Snapshot snapshot;
   snapshot.folder=sc.MacBrowser().TargetFolder();
   snapshot.resourceKey=VolumeCoord::PathKey(snapshot.folder.WindowsPath());
   snapshot.revision=VolumeCoord::Revision(snapshot.resourceKey);
   snapshot.capabilitiesKnown=true;
   snapshot.capabilities.create=snapshot.capabilities.remove=snapshot.capabilities.rename=true;
   snapshot.rows.push_back(*row);
   // File-attribute opens can succeed even during a sharing denial. Exercise
   // the enumeration fallback explicitly while the data handle is locked.
   snapshot.rows.front().entry.identity.clear();
   snapshot.rows.front().location.expectedIdentity.clear();
   sc.PublishFolder(snapshot,sc.Tree()->GetTreeCtrl().GetSelectedItem(),{},Wide(source),"",
       [](const BrowserDirectory::Row&){return HTREEITEM{};});
   Select(sc,L"locked.txt");
   storageUiTest={};
   c->SendMessage(WM_COMMAND,ID_EDIT_CUT);
   const auto captured=BrowserTransfers::Snapshot();
   Check(captured.items.size()==1&&captured.items.front().cut&&captured.items.front().expectedIdentity.empty()&&
         storageUiTest.dialogCreations==0,"capturing a locked Windows row opened storage UI or rebound its identity");
   Host(sc,dest);
   CloseHandle(locked.value);locked.value=INVALID_HANDLE_VALUE;
   const auto replacement=source+"\\replacement.txt";
   {CFile file;Check(file.Open(replacement,CFile::modeWrite|CFile::modeCreate),"create replacement Windows entry");file.Write("new",3);}
   Check(MoveFileExA(replacement,path,MOVEFILE_REPLACE_EXISTING)!=FALSE,"replace the unverified selection");
   Check(Paste(*b).code!=0&&GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES&&
         !Find(*bv,2,u"locked.txt"),"an unverified Cut rebound to a same-name replacement after navigation");
   auto request=FileOperations::RenameEntry{captured.items.front(),u"renamed.txt"};
   auto rename=FileOperations::Submit(request);
   Check(WaitForSingleObject(rename.Job()->event(),10000)==WAIT_OBJECT_0&&rename.Job()->result().code&&
         GetFileAttributesA(source+"\\renamed.txt")==INVALID_FILE_ATTRIBUTES,
         "worker rename accepted the unverified entry");
  }
  fprintf(report,"PASS: selecting a locked Windows file stays responsive and unverified selections cannot bind to a replacement.\n");

  Put(*av,2,u"ReadOnly",72);
  const auto readOnlyIndex=sc.MacBrowser().AttachTestVolume(std::make_shared<ReadOnly>(av),"read-only");
  Check(sc.MacBrowser().ShowTestFolder(readOnlyIndex,2),"show read-only fixture");
  Select(sc,L"ReadOnly");
  const auto count=BrowserTransfers::Snapshot().items.size();
  c->SendMessage(WM_COMMAND,ID_EDIT_CUT);
  Check(BrowserTransfers::Snapshot().items.size()==count,"read-only Cut changed the clipboard");
  BrowserTransfers::Drag readOnly(sc);
  Check(!readOnly.TestDrop(sb,false,nullptr,-1,MK_SHIFT)&&!BackgroundCopies::Active(),"read-only source accepted Shift drop");
  Success(Transfer([&]{return readOnly.TestDrop(sb,false,nullptr,-1,0);}));
  Verify(*bv,2,u"ReadOnly",72);
  Check(!readOnly.TestDrop(sc,false,nullptr,-1,MK_CONTROL)&&!BackgroundCopies::Active(),"read-only destination accepted a drop");

  stage="classic HFS mounts and legacy conversion";
  const auto ca=host+"\\classic-a.dsk",cb=host+"\\classic-b";
  FormatClassic(ca);
  FormatClassic(cb);
  auto d=app.CreateBrowser(FALSE),e=app.CreateBrowser(FALSE);
  Check(d&&e,"create classic browser frames");
  auto& sd=d->GetBrowserSession();
  auto& se=e->GetBrowserSession();
  const auto unicodeClassic=Wide(host)+L"\\classic-\u65e5\u672c";
  Check(MoveFileW(Wide(cb).c_str(),unicodeClassic.c_str())!=FALSE,"rename classic image to a Unicode path");
  auto classicSource=Mount(sd,ca);
  Check(se.MacBrowser().OpenImage(unicodeClassic)==1&&se.AwaitNavigationForTest(),"open Unicode classic image");
  auto classicTarget=se.MacBrowser().TargetFolder().Volume();
  Check(classicTarget&&MacBrowser::ExecutionFor(*classicTarget).Persistent(),"Unicode classic image did not use a persistent worker");
  const auto classicId=Put(*classicSource,2,u"LegacyFile",61);
  Put(*classicTarget,2,u"LegacyFile",99);
  Refresh(sd);
  Select(sd,L"LegacyFile");
  d->SendMessage(WM_COMMAND,ID_EDIT_COPY);
  Success(Paste(*e,Choice::Replace));
  Verify(*classicTarget,2,u"LegacyFile",61);
  classicSource->Remove(classicId);
  Check(Put(*classicSource,2,u"LegacyFile",88)!=classicId,"classic replacement fixture reused the ID");
  Check(Paste(*e).code!=0,"same-name classic replacement was copied using a stale selection");
  Verify(*classicTarget,2,u"LegacyFile",61);
  d->SendMessage(WM_CLOSE);

  stage="mounted classic raw export";
  char oldOut,oldIn;
  int oldAd;
  HFSIFACE_copy_settings(&oldOut,&oldIn,&oldAd);
  struct RestoreMode {char out,in;int ad;~RestoreMode(){HFSIFACE_restore_copy_settings(out,in,ad);}} restore{oldOut,oldIn,oldAd};
  HFSIFACE_restore_copy_settings('r','r',0);
  Refresh(se);
  Select(se,L"LegacyFile");
  e->SendMessage(WM_COMMAND,ID_EDIT_COPY);
  Host(sc,source);
  Success(Paste(*c));
  {
   CFile exported;
   Check(exported.Open(source+"\\LegacyFile",CFile::modeRead),"classic raw export was not created");
   unsigned char bytes[29]{};
   Check(exported.GetLength()==29&&exported.Read(bytes,29)==29&&bytes[0]==61&&
         GetFileAttributesA(source+"\\._LegacyFile")==INVALID_FILE_ATTRIBUTES,"classic raw export settings or data were lost");
  }
  stage="mounted classic raw import";
  Select(sc,L"sample.txt");
  c->SendMessage(WM_COMMAND,ID_EDIT_COPY);
  Success(Paste(*e));
  const auto imported=Find(*classicTarget,2,u"sample.txt");
  Check(imported&&classicTarget->Stat(imported).forks[0].size==5,"Windows raw import was not created");
  char bytes[5]{};
  classicTarget->Read(imported,0,0,bytes,5);
  Check(!memcmp(bytes,"hello",5),"Windows raw import bytes differ");

  stage="mixed conversion pairs and extensionless classic image";
  const auto classicMixed=Put(*classicTarget,2,u"Mixed Classic",47);
  const auto nativeMixed=Put(*av,2,u"Mixed Native",52);
  auto classicLocation=FileOperations::Location::Mounted(classicTarget,classicMixed);
  const auto nativeLocation=FileOperations::Location::Mounted(av,nativeMixed);
  const auto destination=sc.MacBrowser().TargetFolder();
  const auto legacyLocation=FileOperations::Location::Classic((LPCSTR)cb,"Legacy:Mixed Classic");
  const auto plan=FileOperations::PlanTransfers({{legacyLocation,nativeLocation,classicLocation},destination},
      {CaptureLegacyCopyOptions('r'),false,true});
  Check(plan.size()==3&&plan[0].Conversion()&&plan[0].Conversion()->askMode&&
        plan[1].Get<FileOperations::OrdinaryCopy>()&&plan[2].Conversion()&&
        plan[2].Conversion()->source.Volume()==classicTarget,
        "planner did not classify each typed endpoint without relying on an image suffix");
  Success(Transfer([&]{return sc.MacBrowser().TransferSelection({classicLocation,nativeLocation},destination);}));
  {
   auto hostVolume=MacBrowser::OpenWindows(destination.WindowsPath());
   const auto converted=Find(*hostVolume,2,u"Mixed Classic");
   Check(converted&&hostVolume->Stat(converted).forks[1].size==0,"mixed batch dropped the raw classic export policy");
   Verify(*hostVolume,2,u"Mixed Native",52);
  }

  stage="Unicode conversion through a temporary worker";
  {
   const auto exported=Wide(dest)+L"\\worker-export";
   Check(CreateDirectoryW(exported.c_str(),nullptr)!=FALSE,"create worker export destination");
   const FileOperations::Request conversion=FileOperations::LegacyConversion{
       classicLocation,FileOperations::Location::Windows(exported),false,false,false,
       CaptureLegacyCopyOptions('r'),FileOperations::ConversionTarget::Folder};
   const auto sourceKey=classicLocation.ResourceKey();
   const auto outputKey=VolumeCoord::LegacyOutputKey(exported);
   auto job=Storage::copyWorker().submit([conversion,sourceKey,outputKey](Storage::Job& job){
    VolumeCoord::Lease lease({{sourceKey,VolumeCoord::Mode::Write},{outputKey,VolumeCoord::Mode::Write}});
    FileOperations::Completion completion;
    return CopyProcess::Run(conversion,job,completion,{sourceKey,outputKey},{outputKey});
   });
   Check(WaitForSingleObject(job->event(),15000)==WAIT_OBJECT_0,"temporary conversion worker timed out");
   Success(job->result());
   Check(GetFileAttributesW((exported+L"\\Mixed Classic").c_str())!=INVALID_FILE_ATTRIBUTES,
         "worker did not export the typed Unicode-image selection");
  }

  stage="stale converted source";
  Refresh(se);
  auto staleConversion=se.MacBrowser().CaptureItem(Row(se,L"Mixed Classic"));
  classicTarget->Rename(classicMixed,2,u"Renamed before copy");
  Check(Transfer([&]{return sc.MacBrowser().TransferSelection({staleConversion},destination);}).code!=0,
        "typed conversion accepted a stale source name");
  Verify(*classicTarget,2,u"Renamed before copy",47);

  stage="cancel planned conversion";
  const auto cancelId=Put(*classicTarget,2,u"Do Not Export",53);
  auto cancelSource=FileOperations::Location::Mounted(classicTarget,cancelId);
  app.m_copyout_mode=6;
  bool completed=true;
  Check(sc.MacBrowser().TransferSelection({cancelSource},destination,[&](bool ok){completed=ok;}),
        "could not enqueue Ask-mode classic export");
  Until([]{return BackgroundCopies::ModeWindow()!=nullptr;},"Ask-mode export did not prompt");
  Check(FileOperations::Pending().empty(),"conversion started before choosing a mode");
  SendMessage(BackgroundCopies::ModeWindow(),WM_COMMAND,IDCANCEL,0);
  Until([]{return !BackgroundCopies::Active();},"cancelled conversion did not finish");
  app.m_copyout_mode=0;
  Check(!completed&&GetFileAttributesA(source+"\\Do Not Export")==INVALID_FILE_ATTRIBUTES,
        "conversion cancellation appeared to succeed or created a destination file");
  Verify(*classicTarget,2,u"Do Not Export",53);
  fprintf(report,"PASS: per-pair mixed policy, Unicode classic image through both worker routes, stale source rejection, and conversion cancellation.\n");

  Check(app.m_browsers.size()==3,"unexpected browser collection");
  fprintf(report,"PASS: shared mounted workers, same-volume move and descendant rejection, clipboard lifetime, mixed paths, read-only policy, and same-label classic HFS with raw conversion. No physical media or desktop automation.\n");
 }catch(const std::exception& error){
  fprintf(report,"FAIL (%s): %s; last background result: %s\n",stage,error.what(),BackgroundCopies::LastResult().message.c_str());
  result=1;
 }catch(CException* error){
  char message[512]{};
  error->GetErrorMessage(message,ARRAYSIZE(message));
  fprintf(report,"FAIL (%s): MFC exception: %s\n",stage,message);
  error->Delete();
  result=1;
 }
 BackgroundCopies::Shutdown();
 BackgroundCopies::enabled=false;
 storageUiTest={};
 BrowserTransfers::Clear();
 fclose(report);
 return result;
}
#endif
