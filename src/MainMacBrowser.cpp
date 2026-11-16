// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "MainMacBrowser.h"
#include "FileOperations.h"
#include "VolumeCoordinator.h"
#include "BrowserSession.h"
#include "BrowserPresentation.h"
#include "BrowserDirectory.h"
#include "FindFilesDialog.h"
#include "HfsPlusDialogs.h"
#include "HfsPlusCd.h"
#include "StorageJobDialog.h"
#include "own_ids.h"
#include "DirectFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "charset.h"
#include <commdlg.h>
#include <set>
#include <algorithm>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
}
using namespace MacBrowser;
namespace {
CHFVExplorerApp &App() { return *static_cast<CHFVExplorerApp*>(AfxGetApp()); }
std::wstring FromAnsi(const std::string &s) {
    int n=MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),nullptr,0);std::wstring out(n,0);
    if(n)MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),&out[0],n);return out;
}
void TreeText(BrowserSession& session_,HTREEITEM item,const std::wstring& text) {
    TVITEMW value{};value.mask=TVIF_TEXT;value.hItem=item;value.pszText=const_cast<wchar_t*>(text.c_str());
    SendMessageW(session_.Tree()->m_ct->m_hWnd,TVM_SETITEMW,0,reinterpret_cast<LPARAM>(&value));
}

bool FileDialog(BrowserSession& session_,std::wstring& path,bool save) {
    wchar_t buffer[32768]{};wcsncpy_s(buffer,path.c_str(),_TRUNCATE);OPENFILENAMEW ofn{};
    ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=session_.Owner()->m_hWnd;ofn.lpstrFile=buffer;ofn.nMaxFile=ARRAYSIZE(buffer);
    ofn.lpstrFilter=L"All files\0*.*\0\0";ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?0:OFN_FILEMUSTEXIST);
    if(!(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn)))return false;path=buffer;return true;
}
bool Hidden(BrowserSession& session_) {
#ifdef HFV_TESTING
    return !session_.Owner()->IsWindowVisible();
#else
    return false;
#endif
}
bool Run(BrowserSession& session_,std::function<void(Storage::Job&)> work,bool copy=false,bool single=false) {
    auto body=[&](Storage::Job& job){work(job);return Storage::Result{};};
    auto result=copy?ShowStorageJob(body,"Copying files",Hidden(session_),single,true,session_.Owner()):ShowStorageWaitJob(body,Hidden(session_),session_.Owner());
    if(result.code&&result.error!=ERROR_CANCELLED&&!Hidden(session_)&&!session_.Refreshing())AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);
    return result.code==0;
}
bool RunOperation(BrowserSession& session,FileOperations::Request request,bool copy=false,bool single=false){
 auto operation=FileOperations::Submit(std::move(request));auto result=ShowSubmittedStorageJob(operation.Job(),copy?"Copying files":"Updating files",Hidden(session),single,true,session.Owner());
 if(result.code&&result.error!=ERROR_CANCELLED&&!Hidden(session))session.Owner()->MessageBox(result.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);
 return !result.code&&operation.Snapshot().complete;
}
using Location=FileOperations::Location;

}
struct MainMacBrowser::State {
    BrowserSession& session_;
    explicit State(BrowserSession& session):session_(session){}
    struct Mount {std::shared_ptr<Volume> volume;std::wstring key;HTREEITEM root=nullptr;int drive=-1;};
    std::vector<Mount> mounts;
    std::vector<Location> clipboard;
    std::vector<Entry> shown;
    int current=-1;uint32_t folder=2;bool showing=false,folderMutable=true;
    std::vector<uint32_t> restore;
    bool Selected(int& mount,uint32_t& id,HTREEITEM item=nullptr)const {
        if(!session_.Tree()||!session_.List()||!IsWindow(session_.Tree()->GetSafeHwnd())||!IsWindow(session_.List()->GetSafeHwnd()))return false;if(!item)item=session_.Tree()->m_ct->GetSelectedItem();if(!item)return false;
        auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(item));
        if(!p||p->type!=LIST_MAC)return false;mount=p->volinx;id=uint32_t(p->id);
        return mount>=0&&size_t(mount)<mounts.size()&&bool(mounts[mount].volume);
    }
    Location Target(HTREEITEM item=nullptr) {
        if(!item)item=session_.Tree()->m_ct->GetSelectedItem();Location out;auto tv=session_.Tree();
        auto p=reinterpret_cast<lparam_struct*>(tv->m_ct->GetItemData(item));
        if(!p)throw HfsPlus::Error("Select a destination folder");out.type=p->type;out.index=p->volinx;
        if(p->type==LIST_MAC){out.volume=mounts.at(p->volinx).volume;out.id=uint32_t(p->id);}
        else if(p->type==LIST_FAT)out.windowsPath=FromAnsi(p->path);
        else if(p->type==LIST_HFS){int mac=0;out.classicImage=session_.Document()->m_volumes[p->volinx].m_file_name;
            out.classicPath=session_.Document()->get_directory(item,&mac);}
        else throw HfsPlus::Error("This item is not a destination folder");return out;
    }
    Location Item(int row,bool cut) {
        auto lv=session_.List();auto p=reinterpret_cast<list_lparam_struct*>(lv->GetListCtrl().GetItemData(row));
        if(!p||p->type==LIST_SPEC)throw HfsPlus::Error("Select a file or folder");Location out;out.type=p->type;out.index=p->volinx;out.cut=cut;out.directory=p->isdir!=0;
        if(p->type==LIST_MAC){out.volume=mounts.at(p->volinx).volume;out.id=uint32_t(p->id);}
        else {CString path;lv->get_path(row,path);
            if(p->type==LIST_FAT)out.windowsPath=FromAnsi((LPCSTR)path);
            else{out.classicImage=session_.Document()->m_volumes[p->volinx].m_file_name;out.classicPath=(LPCSTR)path;}}
        if(p->type==LIST_MAC)for(const auto& e:shown)if(e.id==out.id){out.expectedIdentity=e.identity;out.expectedName=e.name;out.expectedParent=e.parent;break;}
        if(p->type==LIST_HFS)out.expectedCatalogId=uint32_t(p->id);
        return out;
    }
    HTREEITEM Insert(int index,uint32_t id,HTREEITEM parent,const std::wstring& name,bool root=false) {
        auto tree=session_.Tree();TVINSERTSTRUCTW ins{};ins.hParent=parent;ins.hInsertAfter=TVI_LAST;
        ins.item.mask=TVIF_TEXT|TVIF_IMAGE|TVIF_SELECTEDIMAGE|TVIF_PARAM;
        ins.item.pszText=const_cast<wchar_t*>(name.c_str());ins.item.iImage=ins.item.iSelectedImage=root?(mounts[index].drive>=0?9:3):0;
        ins.item.lParam=tree->make_lparam(LIST_MAC,index,long(id),nullptr);
        return reinterpret_cast<HTREEITEM>(SendMessageW(tree->m_ct->m_hWnd,TVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&ins)));
    }
    void Root(int index) {
        auto& m=mounts[index];if(!m.volume)return;auto label=Wide(m.volume->Name())+L" ("+FromAnsi(m.volume->Description())+L")";
        if(m.drive>=0)label+=std::wstring(L" - ")+wchar_t('A'+m.drive)+L":";
        if(!m.volume->Writable())label+=L" [read-only]";
        m.root=Insert(index,2,nullptr,label,true);
    }
    bool Show(int index,uint32_t id,HTREEITEM item,bool rebuild=false) {
        if(showing)return true;session_.ClearCurrent();showing=true;struct Done{bool& flag;~Done(){flag=false;}}done{showing};
        auto volume=mounts.at(index).volume;uint64_t revision=0;std::wstring folderTitle;std::vector<std::pair<uint32_t,std::vector<Entry>>> levels;std::vector<Entry> entries;uint64_t free=0;bool mutableFolder=true;
        std::vector<uint32_t> ancestors;for(auto a=item;a;a=session_.Tree()->m_ct->GetParentItem(a)){auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(a));ancestors.push_back(uint32_t(p->id));}
        if(!Run(session_,[&](Storage::Job&){
            VolumeCoord::Lease read({{volume->ResourceKey(),VolumeCoord::Mode::Read}});revision=VolumeCoord::Revision(volume->ResourceKey());
            if(rebuild){
                id=2;for(auto old:ancestors){try{if(volume->Stat(old).directory){id=old;break;}}catch(const std::exception&){}}
                std::vector<uint32_t> chain;std::set<uint32_t> parents;uint32_t at=id;
                while(at!=2){if(chain.size()>128||!parents.insert(at).second)throw HfsPlus::Error("Cyclic folder ancestry");chain.push_back(at);at=volume->Stat(at).parent;}
                chain.push_back(2);ancestors=chain;
                for(auto parent=chain.rbegin();parent!=chain.rend();++parent)if(*parent!=id)levels.push_back({*parent,volume->List(*parent)});
            }
            std::set<uint32_t> visited;
            for(auto ancestor:ancestors){
                auto e=volume->Stat(ancestor);auto key=e.resolvedId?e.resolvedId:e.id;
                if(!visited.insert(key).second)throw HfsPlus::Error("Cyclic directory hard link");
            }
            auto currentFolder=volume->Stat(id);folderTitle=Wide(currentFolder.name);mutableFolder=(currentFolder.id==2||!currentFolder.protectedFile)&&!currentFolder.hardLink;
            entries=volume->List(id);free=volume->FreeBytes();})){
            auto tree=session_.Tree();bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
            auto root=mounts[index].root;for(auto child=tree->m_ct->GetChildItem(root);child;){auto next=tree->m_ct->GetNextSiblingItem(child);tree->m_ct->DeleteItem(child);child=next;}
            tree->m_ct->SelectItem(root);tree->enable_selchange(enabled);
            session_.List()->initialize_volume("",0);session_.List()->SetBrowserCapabilities({});shown.clear();current=index;folder=2;folderMutable=false;return false;}
        entries.erase(std::remove_if(entries.begin(),entries.end(),[](const Entry& e){return !App().m_show_invisible_mac&&((uint16_t(e.finder[8])<<8|e.finder[9])&0x4000);}),entries.end());
        auto tree=session_.Tree();auto& control=*tree->m_ct;BrowserPresentation::Batch treeBatch(control.m_hWnd),listBatch(session_.List()->GetListCtrl().m_hWnd,true);bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
        if(rebuild){
            auto root=mounts[index].root;tree->delete_subdir_contents(root);std::map<uint32_t,HTREEITEM> pathItems;pathItems[2]=root;
            for(auto& level:levels)for(auto& entry:level.second)if(entry.directory&&(App().m_show_invisible_mac||!((uint16_t(entry.finder[8])<<8|entry.finder[9])&0x4000)||std::find(ancestors.begin(),ancestors.end(),entry.id)!=ancestors.end()))pathItems[entry.id]=Insert(index,entry.id,pathItems.at(level.first),Wide(entry.name));
            item=pathItems.at(id);
        }
        if(id!=2)TreeText(session_,item,folderTitle);
        // Replace only this directory's child nodes, never unrelated volumes.
        for(HTREEITEM child=control.GetChildItem(item);child;){auto next=control.GetNextSiblingItem(child);control.DeleteItem(child);child=next;}
        std::map<uint32_t,HTREEITEM> children;
        for(auto& e:entries)if(e.directory)children[e.id]=Insert(index,e.id,item,Wide(e.name));
        auto list=session_.List();auto& lc=list->GetListCtrl();list->list_clear();list->destroy_pointers();list->initialize_volume("",id==2?0:ID_GOTO_PARENT);
        auto capabilities=volume->Supports();if(!mutableFolder)capabilities.create=capabilities.rename=capabilities.remove=false;
        list->SetBrowserCapabilities(capabilities);
        for(const auto& e:entries){
            auto row=BrowserDirectory::FromMac(e,index,volume->Supports());
            list->InsertBrowserRow(row,e.directory?children[e.id]:nullptr);
        }
        session_.Observe(volume->ResourceKey(),revision,LIST_MAC,index,id);shown=std::move(entries);current=index;folder=id;folderMutable=mutableFolder;control.SelectItem(item);control.EnsureVisible(item);tree->enable_selchange(enabled);session_.MacBrowser().Sort(FIELD_NAME,true);
        if(session_.Status()){CString status;status.Format("%s%s - %I64u bytes free",volume->Kind(),volume->Writable()?"":" (read-only)",free);
            session_.Status()->SetPaneText(0,status);}
        return true;
    }
    bool Transfer(const std::vector<Location>& sources,const Location& target) {
        if(target.type==LIST_HFS)session_.Document()->m_volumes[target.index].close();
        FileOperations::Request request;request.kind=FileOperations::Kind::Copy;request.sources=sources;request.target=target;
        bool complete=RunOperation(session_,std::move(request),true,sources.size()==1&&!sources[0].directory);
        if(session_.Tree()&&session_.List()){
            if(target.type==LIST_HFS)session_.Document()->OnOpenDocument("");
            else if(target.type==LIST_MAC)session_.MacBrowser().Refresh();else session_.List()->update_current_view();
        }
        return complete;
    }
};
MainMacBrowser::MainMacBrowser(BrowserSession& session):session_(session),state_(new State(session)){}
MainMacBrowser::~MainMacBrowser()=default;
void MainMacBrowser::ForgetTree(){state_->restore.clear();for(auto& mount:state_->mounts)mount.root=nullptr;}
bool MainMacBrowser::Active()const{int index;uint32_t id;return state_->Selected(index,id);}
bool MainMacBrowser::Writable()const{int i;uint32_t id;return state_->Selected(i,id)&&state_->folderMutable&&state_->mounts[i].volume->Supports().create;}
bool MainMacBrowser::SelectionWritable()const{
    if(!Writable())return false;auto& list=session_.List()->GetListCtrl();bool selected=false;
    for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));
        if(!p||p->type!=LIST_MAC)continue;selected=true;for(auto& e:state_->shown)if(e.id==uint32_t(p->id)&&(e.protectedFile||e.compressed||e.hardLink||!e.readError.empty()))return false;}
    return selected;
}
bool MainMacBrowser::CanExport()const{
    if(!Active())return false;auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return false;
    auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));return p&&p->type==LIST_MAC&&!p->isdir;
}
int MainMacBrowser::SelectedCdDrive()const{int i;uint32_t id;return state_->Selected(i,id)?state_->mounts[i].drive:-1;}
int MainMacBrowser::OpenImage(const char* path) {
    if(!path||!*path)return 0;return OpenImage(FromAnsi(path));
}
int MainMacBrowser::OpenImage(const std::wstring& path) {
    auto wide=path;wchar_t full[32768];if(GetFullPathNameW(wide.c_str(),ARRAYSIZE(full),full,nullptr))wide=full;
    for(size_t i=0;i<state_->mounts.size();++i)if(!_wcsicmp(wide.c_str(),state_->mounts[i].key.c_str())){
        if(!state_->mounts[i].root)state_->Root(int(i));state_->Show(int(i),2,state_->mounts[i].root);return 1;}
    std::shared_ptr<Volume> volume;bool ok=Run(session_,[&](Storage::Job&){volume=MacBrowser::OpenImage(wide);});
    if(!ok)return -1;if(!volume)return 0;
    for(size_t n=0;n<state_->mounts.size();++n)if(state_->mounts[n].volume&&state_->mounts[n].volume->ResourceKey()==volume->ResourceKey()){state_->Show(int(n),2,state_->mounts[n].root);return 1;}
    state_->mounts.push_back({volume,wide,nullptr,-1});int i=int(state_->mounts.size()-1);state_->Root(i);state_->Show(i,2,state_->mounts[i].root);
    int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string ansi(n,0);BOOL lost=FALSE;if(n)WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,&ansi[0],n,nullptr,&lost);
    if(!lost&&n)App().AddToRecentFileList(ansi.c_str());return 1;
}
int MainMacBrowser::OpenCd(const char* root) {
    int drive=HfsPlus::CdDriveFromRoot(root);if(drive<0)return 0;
    for(auto& mount:state_->mounts)if(mount.drive==drive&&mount.root)return 0; // Windows side stays selectable.
    bool detected=false;std::shared_ptr<Volume> volume;auto result=ShowStorageWaitJob([&](Storage::Job&){try{volume=MacBrowser::OpenCdRoot(root);}catch(const MacBrowser::DetectedVolumeError&){detected=true;throw;}return Storage::Result{};},Hidden(session_),session_.Owner());
    if(result.code){if(detected){if(!Hidden(session_))AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);return -1;}return 0;}if(!volume)return 0;state_->mounts.push_back({volume,FromAnsi(root),nullptr,drive});int i=int(state_->mounts.size()-1);
    state_->Root(i);state_->Show(i,2,state_->mounts[i].root);return 1;
}
bool MainMacBrowser::Select(LPARAM data){auto p=reinterpret_cast<lparam_struct*>(data);if(!p||p->type!=LIST_MAC)return false;
    state_->Show(p->volinx,uint32_t(p->id),session_.Tree()->m_ct->GetSelectedItem());return true;}
bool MainMacBrowser::Refresh(){int i;uint32_t id;if(!state_->Selected(i,id))return false;
    auto& mount=state_->mounts[i];
    bool ready=Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{mount.volume->ResourceKey(),VolumeCoord::Mode::Read}});if(mount.drive>=0){auto fresh=MacBrowser::OpenCd(mount.drive);if(!fresh)throw HfsPlus::Error("No HFS+ / HFSX disc is present");mount.volume=std::move(fresh);}else mount.volume->Reload();});
    if(!ready){if(mount.drive>=0){ReleaseCd(mount.drive);return false;}session_.List()->initialize_volume("",0);return true;}
    if(mount.drive>=0){id=2;state_->Show(i,2,mount.root);}
    else if(!state_->Show(i,id,session_.Tree()->m_ct->GetSelectedItem())&&id!=2)state_->Show(i,2,mount.root);
    return true;
}
bool MainMacBrowser::RefreshForChange(const std::string& key){
 int index;uint32_t id;if(!state_->Selected(index,id)||state_->mounts[index].volume->ResourceKey()!=key)return false;
 // Read the ancestry, ancestor directories and current listing under one read
 // lease, then rebuild the affected tree path and list in a single UI batch.
 if(!state_->Show(index,id,session_.Tree()->m_ct->GetSelectedItem(),true))throw HfsPlus::Error("Cannot refresh the changed volume");return true;
}
void MainMacBrowser::ReleaseCd(int drive){
    std::vector<std::shared_ptr<Volume>> released;
    auto tree=session_.Tree();bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
    for(auto& mount:state_->mounts)if(mount.drive==drive&&mount.volume){
        auto volume=mount.volume;
        state_->clipboard.erase(std::remove_if(state_->clipboard.begin(),state_->clipboard.end(),[&](const Location& e){return e.volume==volume;}),state_->clipboard.end());
        if(mount.root)tree->m_ct->DeleteItem(mount.root);mount.root=nullptr;released.push_back(std::move(mount.volume));
    }
    tree->enable_selchange(enabled);session_.List()->initialize_volume("",0);
    Run(session_,[&](Storage::Job&){released.clear();});
}
void MainMacBrowser::DetachTree(){state_->restore.clear();int i;uint32_t id;if(state_->Selected(i,id)){
    state_->current=i;for(auto item=session_.Tree()->m_ct->GetSelectedItem();item;item=session_.Tree()->m_ct->GetParentItem(item)){
        auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(item));if(p&&p->type==LIST_MAC)state_->restore.push_back(uint32_t(p->id));}}
    for(auto& mount:state_->mounts)mount.root=nullptr;}
void MainMacBrowser::RebuildTree(){for(size_t i=0;i<state_->mounts.size();++i)state_->Root(int(i));
    if(!state_->restore.empty()&&state_->current>=0){int i=state_->current;auto item=state_->mounts[i].root;
        for(auto at=state_->restore.rbegin();at!=state_->restore.rend();++at){if(*at==2){state_->Show(i,2,item);continue;}
            for(auto child=session_.Tree()->m_ct->GetChildItem(item);child;child=session_.Tree()->m_ct->GetNextSiblingItem(child)){
                auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(child));if(uint32_t(p->id)==*at){item=child;state_->Show(i,*at,item);break;}}}}
}
void MainMacBrowser::Clear(){state_->mounts.clear();state_->clipboard.clear();state_->shown.clear();state_->current=-1;}
bool MainMacBrowser::Activate(int row){auto p=reinterpret_cast<list_lparam_struct*>(session_.List()->GetListCtrl().GetItemData(row));
    if(!p||p->type!=LIST_MAC)return false;if(p->isdir){auto item=p->htree;session_.Tree()->m_ct->SelectItem(item);Select(session_.Tree()->m_ct->GetItemData(item));}
    else{auto& list=session_.List()->GetListCtrl();list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);Export(0);}return true;}
bool MainMacBrowser::NewFolder(){if(!Active())return false;if(!Writable())return true;
    int i;uint32_t id;state_->Selected(i,id);std::wstring name=L"New Folder";
    if(EditHfsPlusName(session_.Owner(),name,state_->mounts[i].volume->CaseSensitive())){
        Run(session_,[&](Storage::Job&){auto volume=state_->mounts[i].volume;{VolumeCoord::Lease write({{volume->ResourceKey(),VolumeCoord::Mode::Write}});volume->MakeDirectory(id,U16(name));}VolumeCoord::Changed({volume->ResourceKey()});});Refresh();}return true;}
bool MainMacBrowser::Rename(){if(!Active())return false;if(!SelectionWritable())return true;
    auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return true;
    auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(p->type!=LIST_MAC)return true;auto volume=state_->mounts[p->volinx].volume;uint32_t id=uint32_t(p->id);
    Entry e;if(!Run(session_,[&](Storage::Job&){e=volume->Stat(id);}))return true;auto name=Wide(e.name);
    if(EditHfsPlusName(session_.Owner(),name,volume->CaseSensitive())){
        FileOperations::Request request;request.kind=FileOperations::Kind::Rename;request.sources={state_->Item(row,false)};request.target.volume=volume;request.target.id=state_->folder;request.name=U16(name);
        RunOperation(session_,std::move(request));Refresh();}return true;}
bool MainMacBrowser::Delete(bool confirm){if(!Active())return false;if(!SelectionWritable())return true;
    std::vector<Location> selected;auto& list=session_.List()->GetListCtrl();for(int i=-1;(i=list.GetNextItem(i,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(i));if(p->type==LIST_MAC)selected.push_back(state_->Item(i,false));}
    if(selected.empty()||(confirm&&session_.Owner()->MessageBox("Delete the selected files and folders?","HFVExplorer",MB_YESNO|MB_ICONQUESTION)!=IDYES))return true;
    FileOperations::Request request;request.kind=FileOperations::Kind::Delete;request.sources=std::move(selected);RunOperation(session_,std::move(request));Refresh();return true;}
bool MainMacBrowser::Properties(int row){auto p=reinterpret_cast<list_lparam_struct*>(session_.List()->GetListCtrl().GetItemData(row));if(!p||p->type!=LIST_MAC)return false;
    auto volume=state_->mounts[p->volinx].volume;Entry e;if(!Run(session_,[&](Storage::Job&){e=volume->Stat(uint32_t(p->id));}))return true;
    std::wstring text=Wide(e.name)+L"\n"+FromAnsi(volume->Description())+(volume->Writable()?L"":L" (read-only)")+L"\nData fork: "+std::to_wstring(e.forks[0].size)+L" bytes\nResource fork: "+std::to_wstring(e.forks[1].size)+L" bytes";
    MessageBoxW(session_.Owner()->m_hWnd,text.c_str(),L"Properties",MB_OK);return true;}
bool MainMacBrowser::Capture(bool cut,bool append){if(cut&&Active()&&!SelectionWritable()){if(!Hidden(session_))AfxMessageBox("The selected entries cannot be moved.");return true;}std::vector<Location> selected;auto& list=session_.List()->GetListCtrl();
    for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(p->type!=LIST_SPEC)selected.push_back(state_->Item(row,cut));}
    if(cut)for(auto& item:selected)if(item.volume&&!item.volume->Writable()){AfxMessageBox("This volume is read-only.");return true;}
    if(!selected.empty()){if(!append)state_->clipboard.clear();state_->clipboard.insert(state_->clipboard.end(),selected.begin(),selected.end());}
    return Active();}
void MainMacBrowser::LegacyClipboardChanged(){
    auto list=session_.List();state_->clipboard.clear();
    for(int i=0;i<list->m_clipboard.GetSize();++i){Location item;item.type=list->m_clipboard_type.GetAt(i);item.index=list->m_clipboard_volinx.GetAt(i);item.cut=list->m_clipboard_cutting.GetAt(i)!=0;item.directory=list->m_clipboard_isdir.GetAt(i)!=0;
        if(item.type==LIST_FAT)item.windowsPath=FromAnsi((LPCSTR)list->m_clipboard.GetAt(i));else{item.classicPath=list->m_clipboard.GetAt(i);item.classicImage=list->m_clipboard_volpath.GetAt(i);}state_->clipboard.push_back(std::move(item));}
}
bool MainMacBrowser::Paste(){bool relevant=Active();for(auto& item:state_->clipboard)relevant=relevant||bool(item.volume);if(!relevant)return false;
    if(state_->clipboard.empty())return true;try{if(state_->Transfer(state_->clipboard,state_->Target()))state_->clipboard.erase(std::remove_if(state_->clipboard.begin(),state_->clipboard.end(),[](const Location& e){return e.cut;}),state_->clipboard.end());}
    catch(const std::exception& e){AfxMessageBox(e.what());}
    auto list=session_.List();list->empty_clipboard();
    for(auto& item:state_->clipboard)if(!item.volume){
        CString path=item.type==LIST_FAT?CString(item.windowsPath.c_str()):CString(item.classicPath.c_str());
        list->m_clipboard.Add(path);list->m_clipboard_type.Add(item.type);list->m_clipboard_isdir.Add(item.directory);
        list->m_clipboard_volpath.Add(item.classicImage.c_str());list->m_clipboard_cutting.Add(item.cut);list->m_clipboard_volinx.Add(item.index);
    }
    return true;
}
int MainMacBrowser::Drop(int row,HTREEITEM target,int action){
    if(row<0||!target)return -1;auto p=reinterpret_cast<list_lparam_struct*>(session_.List()->GetListCtrl().GetItemData(row));
    auto q=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(target));
    if(!p||!q||(p->type!=LIST_MAC&&q->type!=LIST_MAC))return -1;
    if(p->type==LIST_SPEC)return 0;
    try{return state_->Transfer({state_->Item(row,action==2)},state_->Target(target))?1:0;}
    catch(const std::exception& e){AfxMessageBox(e.what());return 0;}
}
bool MainMacBrowser::DropSelection(HTREEITEM target,int action){
    if(!target)return false;auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(target));if(!p)return false;
    bool relevant=p->type==LIST_MAC;std::vector<Location> sources;auto& list=session_.List()->GetListCtrl();
    for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){auto item=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));
        if(!item||item->type==LIST_SPEC)continue;relevant=relevant||item->type==LIST_MAC;sources.push_back(state_->Item(row,action==2));}
    if(!relevant)return false;try{if(!sources.empty())state_->Transfer(sources,state_->Target(target));}catch(const std::exception& e){AfxMessageBox(e.what());}return true;
}
bool MainMacBrowser::Command(UINT id){if(!Active())return false;
    if(id>=ID_OWN_WITH_MAC_FIRST&&id<ID_OWN_FILE_PROPERTIES)return true; // Classic emulator/file-type actions do not apply.
    if(id>=ID_OWN_MAP_THIS&&id<=ID_OWN_SHORTCUT_WINDOWS)return true;return false;}
bool MainMacBrowser::Find(){
    int index;uint32_t folder;if(!state_->Selected(index,folder))return false;
    FindLocation location;location.mac=true;location.native=state_->mounts[index].volume;location.nativeFolder=folder;
    location.root=BrowserPresentation::Utf8(location.native->Name());location.directory=location.root;
    for(auto item=session_.Tree()->m_ct->GetSelectedItem();item;item=session_.Tree()->m_ct->GetParentItem(item)){
        auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(item));if(p&&p->type==LIST_MAC)location.nativePath.insert(location.nativePath.begin(),uint32_t(p->id));}
    Run(session_,[&](Storage::Job&){for(size_t n=1;n<location.nativePath.size();++n)location.directory+="/"+BrowserPresentation::Utf8(location.native->Stat(location.nativePath[n]).name);});
    FindHit found;if(!ShowFindFilesDialog(session_.Owner(),location,found))return true;
    auto item=state_->mounts[index].root;state_->Show(index,2,item);
    size_t count=found.nativePath.size()-(found.folder?0:1);
    for(size_t n=1;n<count;++n)for(auto child=session_.Tree()->m_ct->GetChildItem(item);child;child=session_.Tree()->m_ct->GetNextSiblingItem(child)){
        auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(child));if(uint32_t(p->id)==found.nativePath[n]){item=child;state_->Show(index,uint32_t(p->id),item);break;}}
    if(!found.folder&&!found.nativePath.empty()){auto& list=session_.List()->GetListCtrl();for(int row=0;row<list.GetItemCount();++row){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(p->type==LIST_MAC&&uint32_t(p->id)==found.nativePath.back()){list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);list.EnsureVisible(row,FALSE);break;}}}
    return true;
}
void MainMacBrowser::Import(){if(!Active()||!Writable())return;std::wstring path;if(!FileDialog(session_,path,false))return;
    Location source;source.type=LIST_FAT;source.windowsPath=path;try{state_->Transfer({source},state_->Target());}catch(const std::exception& e){AfxMessageBox(e.what());}}
void MainMacBrowser::ImportResource(){
    if(!Active()||!Writable())return;auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return;
    auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(!p||p->type!=LIST_MAC||p->isdir)return;
    auto volume=state_->mounts[p->volinx].volume;uint32_t id=uint32_t(p->id),parent=state_->folder;
    std::wstring path;if(!FileDialog(session_,path,false))return;Location input;input.type=LIST_FAT;input.windowsPath=path;
    FileOperations::Request request;request.kind=FileOperations::Kind::ReplaceResource;request.sources={input};request.target=state_->Item(row,false);
    RunOperation(session_,std::move(request),true,true);Refresh();
}
void MainMacBrowser::Export(unsigned fork){if(!Active())return;auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return;
    auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(!p||p->type!=LIST_MAC||p->isdir)return;auto volume=state_->mounts[p->volinx].volume;uint32_t id=uint32_t(p->id);Entry entry;
    if(!Run(session_,[&](Storage::Job&){entry=volume->Stat(id);}))return;auto path=Wide(entry.name)+(fork?L".rsrc":L"");if(!FileDialog(session_,path,true))return;path=BrowserPresentation::WindowsPath(path);
    FileOperations::Request request;request.kind=FileOperations::Kind::ExportFork;request.sources={state_->Item(row,false)};request.target.type=LIST_FAT;request.target.windowsPath=path;request.fork=fork;
    RunOperation(session_,std::move(request),true,true);}

void MainMacBrowser::Sort(int column,bool ascending){
if(Active())session_.List()->SortBrowserRows(column,ascending);
}
bool MainMacBrowser::ContextMenu(CPoint point){
    if(!Active())return false;auto& list=session_.List()->GetListCtrl();
    if(point.x>=0){CPoint local=point;list.ScreenToClient(&local);int row=list.HitTest(local);if(row>=0&&!list.GetItemState(row,LVIS_SELECTED)){list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);}}
    else{CRect rect;list.GetWindowRect(&rect);point=rect.CenterPoint();}
    bool selected=list.GetNextItem(-1,LVNI_SELECTED)>=0;UINT write=Writable()?MF_ENABLED:MF_GRAYED;UINT selectionWrite=SelectionWritable()?MF_ENABLED:MF_GRAYED;
    CMenu menu;menu.CreatePopupMenu();menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_OWN_FILE_OPEN,"&Open");
    menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_EDIT_COPY,"&Copy");menu.AppendMenu(MF_STRING|(selected?selectionWrite:MF_GRAYED),ID_EDIT_CUT,"Cu&t");
    menu.AppendMenu(MF_STRING|write,ID_EDIT_PASTE,"&Paste");menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING|write,ID_OWN_NEW_FOLDER,"New &folder...");menu.AppendMenu(MF_STRING|(selected?selectionWrite:MF_GRAYED),ID_EDIT_LABEL,"&Rename...");
    menu.AppendMenu(MF_STRING|(selected?selectionWrite:MF_GRAYED),ID_OWN_DELETE,"&Delete");
    menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_OWN_FILE_PROPERTIES,"P&roperties...");
    UINT command=menu.TrackPopupMenu(TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,session_.Owner());
    int row=list.GetNextItem(-1,LVNI_SELECTED);
    switch(command){case ID_EDIT_COPY:Capture(false,false);break;case ID_EDIT_CUT:Capture(true,false);break;case ID_EDIT_PASTE:Paste();break;
        case ID_OWN_NEW_FOLDER:NewFolder();break;case ID_EDIT_LABEL:Rename();break;case ID_OWN_DELETE:Delete(true);break;
        case ID_OWN_FILE_OPEN:if(row>=0)Activate(row);break;case ID_OWN_FILE_PROPERTIES:if(row>=0)Properties(row);break;}
    return true;
}

#ifdef HFV_TESTING
size_t MainMacBrowser::TestClipboardCount()const{return state_->clipboard.size();}
int MainMacBrowser::AttachTestVolume(std::shared_ptr<Volume> volume,const char* key){state_->mounts.push_back({volume,FromAnsi(key),nullptr,-1});int i=int(state_->mounts.size()-1);state_->Root(i);return i;}
bool MainMacBrowser::ShowTestFolder(int index,uint32_t id){return state_->Show(index,id,state_->mounts.at(index).root);}
#endif
