// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "BrowserTransfers.h"
#include "MainMacBrowser.h"
#include "HfsPlusDisk.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "utils.h"
#include <cstdio>
#include <fcntl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
}
extern std::string browserTransferError;
namespace {
CString host;
void Check(bool ok,const char* text){if(!ok)throw HfsPlus::Error(text);}
class TransferDoc:public CHFVExplorerDoc{
 DECLARE_DYNCREATE(TransferDoc)
public:TransferDoc():CHFVExplorerDoc(false){m_fat_count=1;m_fats[0].m_letter=host[0];m_fats[0].m_type=DRIVE_FIXED;m_fats[0].m_volume_name=host;}
};
IMPLEMENT_DYNCREATE(TransferDoc,CHFVExplorerDoc)
class TransferFrame:public CMainFrame{
 DECLARE_DYNCREATE(TransferFrame)
public:void LoadWindowPosition()override{}void SaveWindowPosition()override{}void ActivateFrame(int)override{}
};
IMPLEMENT_DYNCREATE(TransferFrame,CMainFrame)
std::wstring Wide(const CString& path){return MacBrowser::Wide(BrowserDirectory::FromAnsi(path));}
int Row(BrowserSession& session,const wchar_t* name){auto& list=session.List()->GetListCtrl();for(int i=0;i<list.GetItemCount();++i){wchar_t text[512]{};LVITEMW item{};item.pszText=text;item.cchTextMax=ARRAYSIZE(text);SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));if(!wcscmp(text,name))return i;}return -1;}
void Select(BrowserSession& session,const wchar_t* name){int row=Row(session,name);Check(row>=0,"selection fixture is absent");auto& list=session.List()->GetListCtrl();list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);}
uint32_t Find(MacBrowser::Volume& volume,uint32_t parent,const std::u16string& name){for(auto& e:volume.List(parent))if(e.name==name)return e.id;return 0;}
uint32_t Put(MacBrowser::Volume& volume,uint32_t parent,const std::u16string& name,unsigned seed=17){MacBrowser::Entry e;e.name=name;e.forks[0].size=29;e.forks[1].size=13;e.finder[0]='T';e.finder[4]='X';return volume.Put(parent,e,[seed](unsigned fork,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<unsigned char*>(p)[i]=BYTE(seed+fork+at+i);},false);}
void Verify(MacBrowser::Volume& volume,uint32_t parent,const std::u16string& name,unsigned seed=17){auto id=Find(volume,parent,name);Check(id!=0,("copied entry is missing: "+std::string(name.begin(),name.end())+" at "+std::to_string(parent)).c_str());auto e=volume.Stat(id);Check(e.forks[0].size==29&&e.forks[1].size==13&&e.finder[0]=='T'&&e.finder[4]=='X',"fork sizes or Finder metadata were lost");for(unsigned f=0;f<2;++f){unsigned char bytes[29]{};volume.Read(id,f,0,bytes,size_t(e.forks[f].size));for(size_t i=0;i<e.forks[f].size;++i)Check(bytes[i]==BYTE(seed+f+i),("copied fork bytes differ: "+std::string(name.begin(),name.end())+" fork "+std::to_string(f)+" byte "+std::to_string(i)+" expected "+std::to_string(BYTE(seed+f+i))+" actual "+std::to_string(bytes[i])).c_str());}}
std::shared_ptr<MacBrowser::Volume> Image(const CString& path){auto w=Wide(path);auto disk=HfsPlus::CreateImage(w.c_str(),8*1024*1024);HfsPlus::Volume::Format(*disk,u"Transfer",true,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);disk.reset();return MacBrowser::OpenImage(w);}
void Classic(const CString& path,unsigned seed){CFile file;Check(file.Open(path,CFile::modeCreate|CFile::modeWrite),"create classic fixture");file.SetLength(2*1024*1024);file.Close();char label[]="Legacy";Check(hfs_format(const_cast<char*>((LPCSTR)path),0,label)==0,"format classic fixture");auto volume=MacBrowser::OpenClassic((LPCSTR)path,"Legacy:",true);Put(*volume,2,u"LegacyFile",seed);volume->Close();}
class ReadOnly:public MacBrowser::Volume{
 std::shared_ptr<MacBrowser::Volume> base_;
public:explicit ReadOnly(std::shared_ptr<MacBrowser::Volume> base):base_(base){}
 std::string ResourceKey()const override{return base_->ResourceKey();}std::u16string Name()const override{return u"Read-only fixture";}const char* Kind()const override{return "Read-only fixture";}bool Writable()const override{return false;}bool CaseSensitive()const override{return true;}uint64_t FreeBytes()const override{return 0;}
 MacBrowser::Entry Stat(uint32_t id)override{return base_->Stat(id);}std::vector<MacBrowser::Entry> List(uint32_t id)override{return base_->List(id);}void Read(uint32_t id,unsigned f,uint64_t at,void* p,size_t n)override{base_->Read(id,f,at,p,n);}
 uint32_t MakeDirectory(uint32_t,const std::u16string&)override{throw HfsPlus::Error("Read-only mutation");}uint32_t Put(uint32_t,const MacBrowser::Entry&,MacBrowser::Reader,bool)override{throw HfsPlus::Error("Read-only mutation");}void Rename(uint32_t,uint32_t,const std::u16string&)override{throw HfsPlus::Error("Read-only mutation");}void Remove(uint32_t)override{throw HfsPlus::Error("Read-only mutation");}
};
void Refresh(CMainFrame* frame){frame->GetBrowserSession().MacBrowser().Refresh();}
void Host(BrowserSession& session,const CString& path){session.Document()->m_fats[0].m_volume_name=path;session.Document()->OnOpenDocument("");session.Tree()->GetTreeCtrl().SelectItem(session.Document()->m_fats[0].m_rootitem);session.Document()->open_fat_directory(0,const_cast<char*>((LPCSTR)path),1);}
void Pump(){DWORD end=GetTickCount()+180;while(LONG(end-GetTickCount())>0){MSG m;while(PeekMessage(&m,nullptr,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessage(&m);}Sleep(5);}}
}
int RunBrowserTransfersTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\browser-transfers-report.txt","w");if(!report)return 2;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;app.m_copyin_mode=app.m_copyout_mode=0;
 BrowserTransfers::Clear();int result=0;
 try{
  auto ap=host+"\\a.dsk",bp=host+"\\b.dsk";auto av=Image(ap),bv=Image(bp);Put(*av,2,u"Payload");auto folder=bv->MakeDirectory(2,u"Destination");
  app.InstallBrowserTemplate(RUNTIME_CLASS(TransferDoc),RUNTIME_CLASS(TransferFrame));auto a=app.CreateBrowser(FALSE),b=app.CreateBrowser(FALSE);Check(a&&b,"create transfer frames");auto& sa=a->GetBrowserSession();auto& sb=b->GetBrowserSession();
  Check(sa.MacBrowser().OpenImage((LPCSTR)ap)>0&&sb.MacBrowser().OpenImage((LPCSTR)bp)>0,"open transfer images");
  Select(sa,L"Payload");a->SendMessage(WM_COMMAND,ID_EDIT_COPY);Check(BrowserTransfers::Snapshot().items.size()==1,"Copy did not publish a shared selection");
  b->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Verify(*bv,2,u"Payload");Check(Find(*av,2,u"Payload")&&BrowserTransfers::Any(),"copy removed its source or clipboard");
  storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Skip;b->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Verify(*bv,2,u"Payload");Check(BrowserTransfers::Any(),"Skip consumed copied clipboard");
  Put(*av,2,u"Move",33);Put(*bv,2,u"Move",99);Refresh(a);Select(sa,L"Move");a->SendMessage(WM_COMMAND,ID_EDIT_CUT);
  storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Cancel;b->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Verify(*av,2,u"Move",33);Verify(*bv,2,u"Move",99);Check(BrowserTransfers::Snapshot().items.front().cut,"cancelled paste consumed Cut");
  storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Skip;b->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Check(Find(*av,2,u"Move")&&BrowserTransfers::Any(),"skipped Cut deleted its source");
  storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Replace;b->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Verify(*bv,2,u"Move",33);Check(!Find(*av,2,u"Move")&&!BrowserTransfers::Any(),"completed Cut was not removed from source/clipboard");
  Refresh(a);Select(sa,L"Payload");a->SendMessage(WM_COMMAND,ID_EDIT_COPY);a->SendMessage(WM_COMMAND,ID_EDIT_COPY_APPEND);Check(BrowserTransfers::Snapshot().items.size()==1,"Copy append duplicated the same entry");
  // Drag snapshots must not overwrite the copy/cut clipboard.
  Put(*av,2,u"Dragged",45);Refresh(a);Select(sa,L"Dragged");BrowserTransfers::Drag drag(sa);
  auto saved=BrowserTransfers::Snapshot().items.front().id;
  Check(drag.TestDrop(sb,false,nullptr,Row(sb,L"Destination"),MK_CONTROL),"folder-row drop failed");Verify(*bv,folder,u"Dragged",45);Check(Find(*av,2,u"Dragged")&&BrowserTransfers::Snapshot().items.front().id==saved,"copy drag moved source or changed clipboard");
  Put(*av,2,u"Tree move",50);Refresh(a);Select(sa,L"Tree move");BrowserTransfers::Drag treeDrag(sa);auto binding=reinterpret_cast<list_lparam_struct*>(sb.List()->GetListCtrl().GetItemData(Row(sb,L"Destination")));Check(binding&&treeDrag.TestDrop(sb,true,binding->htree,-1,MK_SHIFT),"tree-folder move drop failed");Verify(*bv,folder,u"Tree move",50);Check(!Find(*av,2,u"Tree move"),"Shift drop did not move source");
  Refresh(a);Select(sa,L"Payload");BrowserTransfers::Drag policy(sa);auto target=sb.MacBrowser().TargetFolder();auto selection=sa.MacBrowser().SelectedItems(false);
  Check(BrowserTransfers::Choose(selection,target,true,0)==BrowserTransfers::Effect::Copy,"different images with equal mount indexes defaulted to move");
  Check(BrowserTransfers::Choose(selection,sa.MacBrowser().TargetFolder(),true,0)==BrowserTransfers::Effect::Move&&BrowserTransfers::Choose(selection,target,true,MK_CONTROL)==BrowserTransfers::Effect::Copy,"same-volume/modifier policy");
  selection.canMove=false;Check(BrowserTransfers::Choose(selection,target,true,MK_SHIFT)==BrowserTransfers::Effect::None&&BrowserTransfers::Choose(selection,target,true,0)==BrowserTransfers::Effect::Copy&&BrowserTransfers::Choose(selection,target,false,MK_CONTROL)==BrowserTransfers::Effect::None,"read-only move/target policy");
  Check(!policy.TestDrop(sb,false,nullptr,Row(sb,L"Payload"),MK_CONTROL),"regular file accepted a folder drop");
  Put(*av,2,u"Stale",51);Refresh(a);Select(sa,L"Stale");BrowserTransfers::Drag stale(sa);av->Rename(Find(*av,2,u"Stale"),2,u"Renamed");Check(!stale.TestDrop(sb,false,nullptr,-1,MK_CONTROL)&&!Find(*bv,2,u"Stale"),"stale drag copied a renamed/replaced source");
  auto outer=av->MakeDirectory(2,u"Outer"),inner=av->MakeDirectory(outer,u"Inner");Refresh(a);Select(sa,L"Outer");BrowserTransfers::Drag descendant(sa);
  Check(sb.MacBrowser().OpenImage((LPCSTR)ap)>0&&sb.MacBrowser().Activate(Row(sb,L"Outer")),"open same image in another browser");auto innerRow=Row(sb,L"Inner");Check(innerRow>=0&&!descendant.TestDrop(sb,false,nullptr,innerRow,MK_CONTROL),"cross-window descendant copy was accepted");
  Put(*av,2,u"Within",71);Refresh(a);Select(sa,L"Within");BrowserTransfers::Drag within(sa);Check(within.TestDrop(sb,false,nullptr,-1,0),"default same-volume drop failed");Verify(*av,outer,u"Within",71);Check(!Find(*av,2,u"Within"),"default same-volume drop copied instead of moving");
  Check(sb.MacBrowser().ShowTestFolder(0,2),"restore destination browser");
  // Window closure leaves the shared clipboard's owned source available, but
  // a live drag is cancelled when its source HWND disappears.
  Refresh(a);Select(sa,L"Payload");a->SendMessage(WM_COMMAND,ID_EDIT_COPY);BrowserTransfers::Drag closing(sa);a->SendMessage(WM_CLOSE);Check(!closing.TestDrop(sb,false,nullptr,-1,MK_CONTROL),"drag survived destruction of its source window");
  auto after=bv->MakeDirectory(2,u"After close");Refresh(b);Check(sb.MacBrowser().Activate(Row(sb,L"After close")),"show post-close folder");b->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Verify(*bv,after,u"Payload");
  // Host paths are captured independently of the source window's drive index.
  auto c=app.CreateBrowser(FALSE);auto& sc=c->GetBrowserSession();Check(BrowserTransfers::Any()&&BrowserTransfers::Snapshot().items.front().volume==av,"new browser erased the shared clipboard");CString source=host+"\\host-source",dest=host+"\\host-dest";Check(CreateDirectoryA(source,nullptr)&&CreateDirectoryA(dest,nullptr),"create host folders");{CFile f;Check(f.Open(source+"\\sample.txt",CFile::modeCreate|CFile::modeWrite),"create host file");f.Write("hello",5);}
  Host(sc,source);Host(sb,dest);Select(sc,L"Sample.txt");c->SendMessage(WM_COMMAND,ID_EDIT_COPY_APPEND);Check(BrowserTransfers::Snapshot().items.size()==2,"mixed-format Copy append failed");b->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Check(GetFileAttributesA(dest+"\\sample.txt")!=INVALID_FILE_ATTRIBUTES,"host cross-window paste failed");auto hostVolume=MacBrowser::OpenWindows(Wide(dest));Verify(*hostVolume,2,u"Payload");hostVolume.reset();
  Check(sb.MacBrowser().ShowTestFolder(0,2),"show native import target");storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Replace;b->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Check(Find(*bv,2,u"sample.txt")!=0,"Windows-to-native shared paste failed");
  Put(*av,2,u"ReadOnly",72);int ro=sc.MacBrowser().AttachTestVolume(std::make_shared<ReadOnly>(av),"read-only");Check(sc.MacBrowser().ShowTestFolder(ro,2),"show read-only fixture");Select(sc,L"ReadOnly");auto count=BrowserTransfers::Snapshot().items.size();c->SendMessage(WM_COMMAND,ID_EDIT_CUT);Check(BrowserTransfers::Snapshot().items.size()==count,"read-only Cut changed the clipboard");c->SendMessage(WM_COMMAND,ID_EDIT_COPY);BrowserTransfers::Drag readOnly(sc);Check(!readOnly.TestDrop(sb,false,nullptr,-1,MK_SHIFT)&&readOnly.TestDrop(sb,false,nullptr,-1,0),"read-only source Shift/default policy");Verify(*bv,2,u"ReadOnly",72);Check(!readOnly.TestDrop(sc,false,nullptr,-1,MK_CONTROL),"read-only destination accepted a drop");
  auto ca=host+"\\classic-a.dsk",cb=host+"\\classic-b.dsk";Classic(ca,61);Classic(cb,99);auto d=app.CreateBrowser(FALSE),e=app.CreateBrowser(FALSE);auto& sd=d->GetBrowserSession();auto& se=e->GetBrowserSession();Check(sd.Document()->OnOpenDocument(ca)&&se.Document()->OnOpenDocument(cb),"open classic browser fixtures");Select(sd,L"LegacyFile");d->SendMessage(WM_COMMAND,ID_EDIT_COPY);storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Replace;e->SendMessage(WM_COMMAND,ID_EDIT_PASTE);auto cv=MacBrowser::OpenClassic((LPCSTR)cb,"Legacy:",false);Verify(*cv,2,u"LegacyFile",61);cv->Close();cv.reset();
  d->SendMessage(WM_CLOSE);HFSIFACE_get_dir_flush();Check(MoveFileA(ca,ca+".retired"),"retire closed classic source image");Classic(ca,88);e->SendMessage(WM_COMMAND,ID_EDIT_PASTE);cv=MacBrowser::OpenClassic((LPCSTR)cb,"Legacy:",false);Verify(*cv,2,u"LegacyFile",61);cv->Close();Check(BrowserTransfers::Any(),"replaced-image error consumed the clipboard");
  // Explicit raw conversion settings must still be honored for old HFS/PC
  // transfers, rather than silently changing them into AppleDouble exports.
  char oldOut,oldIn;int oldAd;HFSIFACE_copy_settings(&oldOut,&oldIn,&oldAd);HFSIFACE_restore_copy_settings('r','r',0);
  Select(se,L"LegacyFile");e->SendMessage(WM_COMMAND,ID_EDIT_COPY);Host(sc,source);c->SendMessage(WM_COMMAND,ID_EDIT_PASTE);
  {CFile exported;Check(exported.Open(source+"\\LegacyFile",CFile::modeRead),"classic raw export was not created");unsigned char bytes[29]{};Check(exported.GetLength()==29&&exported.Read(bytes,29)==29&&bytes[0]==61&&GetFileAttributesA(source+"\\._LegacyFile")==INVALID_FILE_ATTRIBUTES,"classic raw export settings/data were lost");}
  Select(sc,L"Sample.txt");c->SendMessage(WM_COMMAND,ID_EDIT_COPY);e->SendMessage(WM_COMMAND,ID_EDIT_PASTE);cv=MacBrowser::OpenClassic((LPCSTR)cb,"Legacy:",false);uint32_t imported=0;for(auto& entry:cv->List())if(HfsPlus::CompareNames(entry.name,u"sample.txt",false)==0)imported=entry.id;std::string importedNames;for(auto& entry:cv->List())importedNames+=std::string(entry.name.begin(),entry.name.end())+":"+std::to_string(entry.forks[0].size)+";";Check(imported&&cv->Stat(imported).forks[0].size==5,("Windows raw import was not created: "+importedNames).c_str());char importedBytes[5]{};cv->Read(imported,0,0,importedBytes,5);Check(!memcmp(importedBytes,"hello",5),"Windows raw import bytes differ");cv->Close();HFSIFACE_restore_copy_settings(oldOut,oldIn,oldAd);
  storageUiTest={};BrowserTransfers::Clear();Pump();Check(app.m_browsers.size()==3,"unexpected browser collection");
  fprintf(report,"PASS: real hidden browser Copy/Cut/Paste routing; shared/mixed append and deduplication; Cancel/Skip/Replace with source retention; native data/resource forks and Finder metadata; folder-row/tree/background drops; same-volume moves and read-only policies; stale/descendant rejection; independent drag snapshots; source-window closure and new-window clipboard lifetime; Windows/native imports; same-label classic volumes, replaced-image rejection and legacy raw conversion settings. No physical media or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s; last operation: %s\n",e.what(),browserTransferError.c_str());result=1;}
 catch(CException* e){char text[512]{};e->GetErrorMessage(text,ARRAYSIZE(text));fprintf(report,"FAIL: MFC exception: %s\n",text);e->Delete();result=1;}
 BrowserTransfers::Clear();fclose(report);return result;
}
#endif
