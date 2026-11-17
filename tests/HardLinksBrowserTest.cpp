// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "MacBrowserModel.h"
#include "FileOperations.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "StorageJobDialog.h"
#include "HfsPlusDisk.h"
#include "VolumeCoordinator.h"
#include "utils.h"
#include <cstdio>
namespace {
using namespace MacBrowser;
void Check(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
uint32_t Find(Volume& v,uint32_t parent,const std::u16string& name){Entry e;Check(v.FindChild(parent,name,e),"Missing hard-link browser fixture");return e.id;}
void Payload(unsigned f,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<uint8_t*>(p)[i]=uint8_t((at+i)*17+f*71);}
void Verify(Volume& v,uint32_t id){auto e=v.Stat(id);Check(e.forks[0].size==5003&&e.forks[1].size==723,"Hard-link copy fork sizes");for(unsigned f=0;f<2;++f){HfsPlus::Bytes got(size_t(e.forks[f].size)),wanted(got.size());Payload(f,0,wanted.data(),wanted.size());v.Read(id,f,0,got.data(),got.size());Check(got==wanted,"Hard-link copy fork data");}}
std::shared_ptr<Volume> Image(const std::string& path,HfsPlus::Volume::FormatKind kind){auto wide=VolumeCoord::Wide(path);{auto disk=HfsPlus::CreateImage(wide.c_str(),24*1024*1024);HfsPlus::Volume::Format(*disk,u"Hard links",true,kind);}return MacBrowser::OpenImage(wide);}
FileOperations::Location Location(std::shared_ptr<Volume> v,uint32_t id){FileOperations::Location out;out.type=LIST_MAC;out.volume=v;out.id=id;auto e=v->Stat(id);out.directory=e.directory;out.expectedName=e.name;out.expectedParent=e.parent;out.expectedIdentity=e.identity;return out;}
void Pump(){MSG m;while(PeekMessage(&m,nullptr,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessage(&m);}}
FileOperations::Operation Run(FileOperations::Request r,bool success=true,Storage::ConflictChoice choice=Storage::ConflictChoice::Replace){r.background=true;auto op=FileOperations::Submit(r);auto start=GetTickCount();while(!op.Job()->done()){if(!op.Job()->conflict().empty())op.Job()->resolve(choice);Check(GetTickCount()-start<30000,"Hard-link job timed out");Pump();Sleep(1);}Check((op.Job()->result().code==0)==success,op.Job()->result().message.c_str());return op;}
void Delete(std::shared_ptr<Volume> v,uint32_t id){FileOperations::Request r;r.kind=FileOperations::Kind::Delete;r.sources={Location(v,id)};Run(r);}
CString host;
class LinkDoc:public CHFVExplorerDoc{DECLARE_DYNCREATE(LinkDoc)
public:LinkDoc():CHFVExplorerDoc(false){m_fat_count=1;m_fats[0].m_letter=host[0];m_fats[0].m_type=DRIVE_FIXED;m_fats[0].m_volume_name=host;}};
IMPLEMENT_DYNCREATE(LinkDoc,CHFVExplorerDoc)
class LinkFrame:public CMainFrame{DECLARE_DYNCREATE(LinkFrame)
public:void LoadWindowPosition()override{}void SaveWindowPosition()override{}void ActivateFrame(int)override{}};
IMPLEMENT_DYNCREATE(LinkFrame,CMainFrame)
int Row(BrowserSession& s,const wchar_t* name){auto& list=s.List()->GetListCtrl();for(int row=0;row<list.GetItemCount();++row){wchar_t text[512]{};LVITEMW item{};item.pszText=text;item.cchTextMax=ARRAYSIZE(text);SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));if(!wcscmp(text,name))return row;}throw HfsPlus::Error("Missing hard-link browser row");}
struct Menu:CCmdUI{bool enabled=false;void Enable(BOOL value=TRUE)override{enabled=value!=FALSE;}};
}
int RunHardLinksBrowserTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory;host=directory;FILE* report=fopen((root+"\\hard-links-report.txt").c_str(),"w");if(!report)return 2;int result=0;InitializeStorageUi();auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 try{
  unsigned variant=0;for(auto kind:{HfsPlus::Volume::FormatKind::HfsPlus,HfsPlus::Volume::FormatKind::HfsXCaseSensitive,HfsPlus::Volume::FormatKind::HfsXCaseInsensitive}){
   auto source=Image(root+"\\source"+std::to_string(variant)+".dsk",kind),target=Image(root+"\\target"+std::to_string(variant++)+".dsk",kind);Entry file;file.name=u"A";file.forks[0].size=5003;file.forks[1].size=723;memcpy(file.finder.data(),"TEXTTEST",8);auto a=source->Put(2,file,Payload,false);source->SetAttributes(a,{{u"user.keep",{1,2,3}}});
   FileOperations::Request link;link.kind=FileOperations::Kind::CreateHardLink;link.sources={Location(source,a)};link.target=Location(source,2);link.name=u"B";Run(link);a=Find(*source,2,u"A");auto b=Find(*source,2,u"B");Check(source->Stat(a).linkCount==2&&!source->Stat(a).protectedFile,"Background link is not writable/shared");Verify(*source,b);
   link.sources={Location(source,a)};link.target=Location(target,2);link.name=u"Invalid";Run(link,false);Check(target->List().size()==2,"Cross-volume hard link wrote output");
   auto d=source->MakeDirectory(2,u"D");source->MakeHardLink(a,d,u"Shared child");file.name=u"Independent";source->Put(d,file,Payload,false);source->MakeHardLink(d,2,u"E");d=Find(*source,2,u"D");auto e=Find(*source,2,u"E");
   FileOperations::Request cycle;cycle.kind=FileOperations::Kind::Copy;cycle.sources={Location(source,d)};cycle.target=Location(source,e);Run(cycle,false);Check(source->List(e).size()==2,"Copy into a directory alias wrote partial output");
   FileOperations::Request copy;copy.kind=FileOperations::Kind::Copy;for(auto id:{a,b,d,e})copy.sources.push_back(Location(source,id));copy.target=Location(target,2);Run(copy);
   auto ta=Find(*target,2,u"A"),tb=Find(*target,2,u"B"),td=Find(*target,2,u"D"),te=Find(*target,2,u"E");Check(target->Stat(ta).resolvedId==target->Stat(tb).resolvedId&&target->Stat(ta).linkCount==3,"File-link topology not copied");Check(target->Stat(td).resolvedId==target->Stat(te).resolvedId&&target->Stat(td).linkCount==2,"Directory-link topology not copied");Verify(*target,Find(*target,td,u"Shared child"));
   Run(copy);Check(target->Stat(Find(*target,2,u"A")).linkCount==3&&target->Stat(Find(*target,2,u"D")).linkCount==2,"Overwrite/merge grew link groups");
   FileOperations::Request rename;rename.kind=FileOperations::Kind::Rename;rename.sources={Location(target,te)};rename.target=Location(target,2);rename.name=u"Renamed E";Run(rename);Check(target->Stat(te).name==u"Renamed E","Background link rename");
   Delete(target,td);Verify(*target,Find(*target,te,u"Independent"));Delete(target,te);Check(target->Stat(ta).linkCount==2,"Directory last-unlink deleted an outside shared file");Delete(target,ta);Verify(*target,tb);Delete(target,tb);
   auto container=target->MakeDirectory(2,u"Container"),first=target->MakeDirectory(container,u"First");file.name=u"Content";target->Put(first,file,Payload,false);target->MakeHardLink(first,container,u"Second");Delete(target,container);Entry absent;Check(!target->FindChild(2,u"Container",absent),"Recursive deletion of all aliases failed");
   auto pcPath=root+"\\export"+std::to_string(variant);Check(CreateDirectoryA(pcPath.c_str(),nullptr)!=FALSE,"Create hard-link export fixture");auto pc=OpenWindows(VolumeCoord::Wide(pcPath));copy.sources={Location(source,a),Location(source,b)};copy.target=Location(pc,2);Run(copy);Verify(*pc,Find(*pc,2,u"A"));Verify(*pc,Find(*pc,2,u"B"));Check(!pc->Stat(Find(*pc,2,u"A")).hardLink,"Windows export claimed native Mac topology");
  }
  fprintf(report,"PASS: background hard-link creation, same-volume checks, shared file/directory copy topology, overwrites/merges, rename, non-last and last unlink, recursive deletion of all aliases, outside-link preservation and Windows both-fork materialization in HFS+ and both HFSX modes.\n");fflush(report);
  auto volume=Image(root+"\\windows-shared.dsk",HfsPlus::Volume::FormatKind::HfsPlus);auto fresh=volume->MakeDirectory(2,u"Fresh");Entry file;file.name=u"Inside";file.forks[0].size=5003;file.forks[1].size=723;volume->Put(fresh,file,Payload,false);
  app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;app.InstallBrowserTemplate(RUNTIME_CLASS(LinkDoc),RUNTIME_CLASS(LinkFrame));auto a=app.CreateBrowser(FALSE),b=app.CreateBrowser(FALSE);Check(a&&b&&!a->IsWindowVisible()&&!b->IsWindowVisible(),"Hidden hard-link browser creation");auto& sa=a->GetBrowserSession();auto& sb=b->GetBrowserSession();auto ia=sa.MacBrowser().AttachTestVolume(volume,"same"),ib=sb.MacBrowser().AttachTestVolume(volume,"same");Check(sa.MacBrowser().ShowTestFolder(ia,2)&&sb.MacBrowser().ShowTestFolder(ib,2),"Hard-link root display");auto row=Row(sb,L"Fresh");sb.List()->GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);sb.List()->GetListCtrl().SetItemState(row,LVIS_SELECTED,LVIS_SELECTED);Check(sb.MacBrowser().Capture(false,false),"Hard-link clipboard capture");Menu menu;menu.m_nID=ID_EDIT_PASTE_HARD_LINK;Check(b->OnCmdMsg(menu.m_nID,CN_UPDATE_COMMAND_UI,&menu,nullptr)&&menu.enabled,"Paste hard link command is disabled on same volume");Check(sa.MacBrowser().Activate(Row(sa,L"Fresh")),"Browse folder before link conversion");
  FileOperations::Request request;request.kind=FileOperations::Kind::CreateHardLink;request.sources={Location(volume,fresh)};request.target=Location(volume,2);request.name=u"Fresh alias";Run(request);Check(sa.MacBrowser().RefreshForChange(volume->ResourceKey())&&sb.MacBrowser().RefreshForChange(volume->ResourceKey()),"Refresh folder-to-inode conversion");Check(sa.MacBrowser().CanCreateFolder()&&Row(sa,L"Inside")>=0,"First browser lost visible folder after conversion");Check(sb.MacBrowser().Activate(Row(sb,L"Fresh alias")),"Browse directory alias");request.kind=FileOperations::Kind::CreateFolder;request.sources.clear();request.target=Location(volume,Find(*volume,2,u"Fresh alias"));request.name=u"Added";Run(request);Check(sa.MacBrowser().RefreshForChange(volume->ResourceKey())&&sb.MacBrowser().RefreshForChange(volume->ResourceKey())&&Row(sa,L"Added")>=0&&Row(sb,L"Added")>=0,"Directory aliases did not refresh in both windows");
  BrowserTransfers::Clear();a->DestroyWindow();b->DestroyWindow();
  fprintf(report,"PASS: two independent hidden windows retain their visible folder paths across inode conversion, enable the Paste hard link command, browse/write directory aliases and refresh shared children. No computer control or physical media.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());result=1;}
 BrowserTransfers::Clear();FileOperations::CancelPending();auto frames=app.m_browsers;for(auto frame:frames)if(IsWindow(frame->GetSafeHwnd()))frame->DestroyWindow();ShutdownStorageUi();fclose(report);return result;
}
#endif
