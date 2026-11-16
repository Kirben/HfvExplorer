// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "MainMacBrowser.h"
#include "BrowserPresentation.h"
#include "FindFilesDialog.h"
#include "HfsPlusBrowser.h"
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
void TreeText(HTREEITEM item,const std::wstring& text) {
    TVITEMW value{};value.mask=TVIF_TEXT;value.hItem=item;value.pszText=const_cast<wchar_t*>(text.c_str());
    SendMessageW(App().m_tree->m_ct->m_hWnd,TVM_SETITEMW,0,reinterpret_cast<LPARAM>(&value));
}
void Cell(int row,int column,const std::wstring& text){BrowserPresentation::Cell(App().m_list->GetListCtrl().m_hWnd,row,column,text);}
bool FileDialog(std::wstring& path,bool save) {
    wchar_t buffer[32768]{};wcsncpy_s(buffer,path.c_str(),_TRUNCATE);OPENFILENAMEW ofn{};
    ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=AfxGetMainWnd()->m_hWnd;ofn.lpstrFile=buffer;ofn.nMaxFile=ARRAYSIZE(buffer);
    ofn.lpstrFilter=L"All files\0*.*\0\0";ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?0:OFN_FILEMUSTEXIST);
    if(!(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn)))return false;path=buffer;return true;
}
bool Hidden() {
#ifdef HFV_TESTING
    return !AfxGetMainWnd()->IsWindowVisible();
#else
    return false;
#endif
}
bool Run(std::function<void(Storage::Job&)> work,bool copy=false,bool single=false) {
    auto body=[&](Storage::Job& job){work(job);return Storage::Result{};};
    auto result=copy?ShowStorageJob(body,"Copying files",Hidden(),single,true):ShowStorageWaitJob(body,Hidden());
    if(result.code&&result.error!=ERROR_CANCELLED&&!Hidden())AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);
    return result.code==0;
}
struct Location {
    int type=0,index=-1;uint32_t id=2;bool cut=false,directory=false;
    std::shared_ptr<Volume> volume;std::wstring windowsPath;std::string classicImage,classicPath;
};
std::pair<std::shared_ptr<Volume>,uint32_t> Source(const Location& value,bool mutate=false) {
    if(value.volume)return {value.volume,value.id};
    std::shared_ptr<Volume> volume;std::u16string name;
    if(value.type==LIST_FAT){auto pos=value.windowsPath.find_last_of(L"\\/");
        auto parent=value.windowsPath.substr(0,pos);if(parent.size()==2&&parent[1]==L':')parent+=L'\\';
        volume=MacBrowser::OpenWindows(parent);name=U16(value.windowsPath.substr(pos+1));
    }else{auto pos=value.classicPath.find_last_of(':');volume=OpenClassic(value.classicImage,value.classicPath.substr(0,pos+1),mutate);
        std::string text=value.classicPath.substr(pos+1);name=U16(FromAnsi(text));}
    for(auto& entry:volume->List())if(HfsPlus::CompareNames(entry.name,name,volume->CaseSensitive())==0)return {volume,entry.id};
    throw HfsPlus::Error("The copied source no longer exists");
}
std::pair<std::shared_ptr<Volume>,uint32_t> Destination(const Location& value) {
    if(value.volume)return {value.volume,value.id};
    if(value.type==LIST_FAT)return {MacBrowser::OpenWindows(value.windowsPath),2};
    return {OpenClassic(value.classicImage,value.classicPath,true),2};
}
}
struct MainMacBrowser::State {
    struct Mount {std::shared_ptr<Volume> volume;std::wstring key;HTREEITEM root=nullptr;int drive=-1;};
    std::vector<Mount> mounts;
    std::vector<Location> clipboard;
    std::vector<Entry> shown;
    int current=-1;uint32_t folder=2;bool showing=false,folderMutable=true;
    std::vector<uint32_t> restore;
    bool Selected(int& mount,uint32_t& id,HTREEITEM item=nullptr)const {
        if(!App().m_tree)return false;if(!item)item=App().m_tree->m_ct->GetSelectedItem();if(!item)return false;
        auto p=reinterpret_cast<lparam_struct*>(App().m_tree->m_ct->GetItemData(item));
        if(!p||p->type!=LIST_MAC)return false;mount=p->volinx;id=uint32_t(p->id);
        return mount>=0&&size_t(mount)<mounts.size()&&bool(mounts[mount].volume);
    }
    Location Target(HTREEITEM item=nullptr) {
        if(!item)item=App().m_tree->m_ct->GetSelectedItem();Location out;auto tv=App().m_tree;
        auto p=reinterpret_cast<lparam_struct*>(tv->m_ct->GetItemData(item));
        if(!p)throw HfsPlus::Error("Select a destination folder");out.type=p->type;out.index=p->volinx;
        if(p->type==LIST_MAC){out.volume=mounts.at(p->volinx).volume;out.id=uint32_t(p->id);}
        else if(p->type==LIST_FAT)out.windowsPath=FromAnsi(p->path);
        else if(p->type==LIST_HFS){int mac=0;out.classicImage=App().m_doc->m_volumes[p->volinx].m_file_name;
            out.classicPath=App().m_doc->get_directory(item,&mac);}
        else throw HfsPlus::Error("This item is not a destination folder");return out;
    }
    Location Item(int row,bool cut) {
        auto lv=App().m_list;auto p=reinterpret_cast<list_lparam_struct*>(lv->GetListCtrl().GetItemData(row));
        if(!p||p->type==LIST_SPEC)throw HfsPlus::Error("Select a file or folder");Location out;out.type=p->type;out.index=p->volinx;out.cut=cut;out.directory=p->isdir!=0;
        if(p->type==LIST_MAC){out.volume=mounts.at(p->volinx).volume;out.id=uint32_t(p->id);}
        else {CString path;lv->get_path(row,path);
            if(p->type==LIST_FAT)out.windowsPath=FromAnsi((LPCSTR)path);
            else{out.classicImage=App().m_doc->m_volumes[p->volinx].m_file_name;out.classicPath=(LPCSTR)path;}}
        return out;
    }
    HTREEITEM Insert(int index,uint32_t id,HTREEITEM parent,const std::wstring& name,bool root=false) {
        auto tree=App().m_tree;TVINSERTSTRUCTW ins{};ins.hParent=parent;ins.hInsertAfter=TVI_LAST;
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
    bool Show(int index,uint32_t id,HTREEITEM item) {
        if(showing)return true;showing=true;struct Done{bool& flag;~Done(){flag=false;}}done{showing};
        auto volume=mounts.at(index).volume;std::vector<Entry> entries;uint64_t free=0;bool mutableFolder=true;
        std::vector<uint32_t> ancestors;for(auto a=item;a;a=App().m_tree->m_ct->GetParentItem(a)){auto p=reinterpret_cast<lparam_struct*>(App().m_tree->m_ct->GetItemData(a));ancestors.push_back(uint32_t(p->id));}
        if(!Run([&](Storage::Job&){
            std::set<uint32_t> visited;
            for(auto ancestor:ancestors){
                auto e=volume->Stat(ancestor);auto key=e.resolvedId?e.resolvedId:e.id;
                if(!visited.insert(key).second)throw HfsPlus::Error("Cyclic directory hard link");
            }
            auto currentFolder=volume->Stat(id);mutableFolder=(currentFolder.id==2||!currentFolder.protectedFile)&&!currentFolder.hardLink;
            entries=volume->List(id);free=volume->FreeBytes();})){
            auto tree=App().m_tree;bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
            auto root=mounts[index].root;for(auto child=tree->m_ct->GetChildItem(root);child;){auto next=tree->m_ct->GetNextSiblingItem(child);tree->m_ct->DeleteItem(child);child=next;}
            tree->m_ct->SelectItem(root);tree->enable_selchange(enabled);
            App().m_list->initialize_volume("",0);shown.clear();current=index;folder=2;return false;}
        entries.erase(std::remove_if(entries.begin(),entries.end(),[](const Entry& e){return !App().m_show_invisible_mac&&((uint16_t(e.finder[8])<<8|e.finder[9])&0x4000);}),entries.end());
        auto tree=App().m_tree;auto& control=*tree->m_ct;BrowserPresentation::Batch treeBatch(control.m_hWnd),listBatch(App().m_list->GetListCtrl().m_hWnd,true);bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
        // Replace only this directory's child nodes, never unrelated volumes.
        for(HTREEITEM child=control.GetChildItem(item);child;){auto next=control.GetNextSiblingItem(child);control.DeleteItem(child);child=next;}
        std::map<uint32_t,HTREEITEM> children;
        for(auto& e:entries)if(e.directory)children[e.id]=Insert(index,e.id,item,Wide(e.name));
        auto list=App().m_list;auto& lc=list->GetListCtrl();list->list_clear();list->destroy_pointers();list->initialize_volume("",id==2?0:ID_GOTO_PARENT);
        for(const auto& e:entries){int row=-1;char type[5]{},creator[5]{};memcpy(type,e.finder.data(),4);memcpy(creator,e.finder.data()+4,4);POINT point{-1,-1};
            int color=((unsigned(e.finder[8])<<8|e.finder[9])>>1)&7;
            list->list_insert("",e.directory,e.directory?0:e.forks[0].size+e.forks[1].size,e.created,e.modified,type,creator,e.id,nullptr,nullptr,
                e.directory?children[e.id]:nullptr,index,&point,FALSE,0,color,LIST_MAC,&row);
            if(row>=0){Cell(row,FIELD_NAME,Wide(e.name)+(e.version?L" [version "+std::to_wstring(e.version)+L"]":L""));if(e.directory)Cell(row,FIELD_SIZE,L"Folder");
                if(!e.readError.empty())Cell(row,FIELD_SIZE,L"Unreadable");}
        }
        shown=std::move(entries);current=index;folder=id;folderMutable=mutableFolder;control.SelectItem(item);control.EnsureVisible(item);tree->enable_selchange(enabled);MainMacBrowser::Get().Sort(FIELD_NAME,true);
        if(App().m_pStatusBar){CString status;status.Format("%s%s - %I64u bytes free",volume->Kind(),volume->Writable()?"":" (read-only)",free);
            App().m_pStatusBar->SetPaneText(0,status);}
        return true;
    }
    bool Transfer(const std::vector<Location>& sources,const Location& target) {
        // Existing classic GUI readers must relinquish stale cached sectors before a write.
        if(target.type==LIST_HFS)App().m_doc->m_volumes[target.index].close();
        bool complete=true;
        bool ok=Run([&](Storage::Job& job){
            HFSIFACE_get_dir_flush();
            std::unique_ptr<DirectFloppy> disk;
            int drive=target.type==LIST_HFS?physical_floppy_path(target.classicImage.c_str()):-1;
            if(drive>=0){physical_floppy_suspend(drive);
#ifdef HFV_FLOPPY_TESTING
                auto api=dynamic_cast<FloppyWriteApi*>(physical_floppy_test_api(drive));
                if(!api)throw HfsPlus::Error("No simulated writable floppy is attached");
                disk.reset(new DirectFloppy(api));
#else
                disk.reset(new DirectFloppy);
#endif
                if(!disk->Open(drive))throw HfsPlus::Error("Cannot open and lock the destination floppy");
            }
            auto destination=Destination(target);
            std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> opened;std::vector<std::string> snapshots;
            for(auto& item:sources){auto source=Source(item);if(item.cut&&item.volume&&!source.first->Writable())throw HfsPlus::Error("A cut source is read-only");snapshots.push_back(item.cut?MacBrowser::Snapshot(*source.first,source.second,job,bool(item.volume)):std::string());opened.push_back(source);}
            complete=MacBrowser::CopyMany(opened,*destination.first,destination.second,job);
            if(!target.volume)destination.first->Close();
            if(disk&&!disk->Finish())throw HfsPlus::Error("Floppy flush failed; source items were retained");
            for(size_t i=0;i<sources.size();++i)if(!sources[i].volume)opened[i].first->Close();
            // Cut is applied only after all destination writes and finalization succeed.
            if(complete)for(size_t i=0;i<sources.size();++i)if(sources[i].cut){
                auto fresh=sources[i].volume?opened[i]:Source(sources[i],true);
                if(MacBrowser::Snapshot(*fresh.first,fresh.second,job)!=snapshots[i])throw HfsPlus::Error("Source changed during the copy; source items were retained");
                RemoveTree(*fresh.first,fresh.second,job);if(!sources[i].volume)fresh.first->Close();
            }
        },true,sources.size()==1&&!sources[0].directory);
        if(target.type==LIST_HFS)App().m_doc->OnOpenDocument("");
        else if(target.type==LIST_MAC)MainMacBrowser::Get().Refresh();else App().m_list->update_current_view();
        return ok&&complete;
    }
};
MainMacBrowser::MainMacBrowser():state_(new State){}
MainMacBrowser::~MainMacBrowser()=default;
MainMacBrowser& MainMacBrowser::Get(){static MainMacBrowser browser;return browser;}
bool MainMacBrowser::Active()const{int index;uint32_t id;return state_->Selected(index,id);}
bool MainMacBrowser::Writable()const{int i;uint32_t id;return state_->Selected(i,id)&&state_->folderMutable&&state_->mounts[i].volume->Supports().create;}
bool MainMacBrowser::SelectionWritable()const{
    if(!Writable())return false;auto& list=App().m_list->GetListCtrl();bool selected=false;
    for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));
        if(!p||p->type!=LIST_MAC)continue;selected=true;for(auto& e:state_->shown)if(e.id==uint32_t(p->id)&&(e.protectedFile||e.compressed||e.hardLink||!e.readError.empty()))return false;}
    return selected;
}
bool MainMacBrowser::CanExport()const{
    if(!Active())return false;auto& list=App().m_list->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return false;
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
    std::shared_ptr<Volume> volume;bool ok=Run([&](Storage::Job&){volume=MacBrowser::OpenImage(wide);});
    if(!ok)return -1;if(!volume)return 0;
    state_->mounts.push_back({volume,wide,nullptr,-1});int i=int(state_->mounts.size()-1);state_->Root(i);state_->Show(i,2,state_->mounts[i].root);
    int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string ansi(n,0);BOOL lost=FALSE;if(n)WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,&ansi[0],n,nullptr,&lost);
    if(!lost&&n)App().AddToRecentFileList(ansi.c_str());return 1;
}
int MainMacBrowser::OpenCd(const char* root) {
    int drive=HfsPlus::CdDriveFromRoot(root);if(drive<0)return 0;char driveRoot[]={char('A'+drive),':',char(92),0};if(GetDriveTypeA(driveRoot)!=DRIVE_CDROM)return 0;
    for(auto& mount:state_->mounts)if(mount.drive==drive&&mount.root)return 0; // Windows side stays selectable.
    bool detected=false;std::shared_ptr<Volume> volume;auto result=ShowStorageWaitJob([&](Storage::Job&){try{volume=MacBrowser::OpenCd(drive);}catch(const MacBrowser::DetectedVolumeError&){detected=true;throw;}return Storage::Result{};},Hidden());
    if(result.code){if(detected){if(!Hidden())AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);return -1;}return 0;}if(!volume)return 0;state_->mounts.push_back({volume,FromAnsi(root),nullptr,drive});int i=int(state_->mounts.size()-1);
    state_->Root(i);state_->Show(i,2,state_->mounts[i].root);return 1;
}
bool MainMacBrowser::Select(LPARAM data){auto p=reinterpret_cast<lparam_struct*>(data);if(!p||p->type!=LIST_MAC)return false;
    state_->Show(p->volinx,uint32_t(p->id),App().m_tree->m_ct->GetSelectedItem());return true;}
bool MainMacBrowser::Refresh(){int i;uint32_t id;if(!state_->Selected(i,id))return false;
    auto& mount=state_->mounts[i];
    bool ready=Run([&](Storage::Job&){if(mount.drive>=0){auto fresh=MacBrowser::OpenCd(mount.drive);if(!fresh)throw HfsPlus::Error("No HFS+ / HFSX disc is present");mount.volume=std::move(fresh);}else mount.volume->Reload();});
    if(!ready){if(mount.drive>=0){ReleaseCd(mount.drive);return false;}App().m_list->initialize_volume("",0);return true;}
    if(mount.drive>=0){id=2;state_->Show(i,2,mount.root);}
    else if(!state_->Show(i,id,App().m_tree->m_ct->GetSelectedItem())&&id!=2)state_->Show(i,2,mount.root);
    return true;
}
void MainMacBrowser::ReleaseCd(int drive){
    std::vector<std::shared_ptr<Volume>> released;
    auto tree=App().m_tree;bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
    for(auto& mount:state_->mounts)if(mount.drive==drive&&mount.volume){
        auto volume=mount.volume;
        state_->clipboard.erase(std::remove_if(state_->clipboard.begin(),state_->clipboard.end(),[&](const Location& e){return e.volume==volume;}),state_->clipboard.end());
        if(mount.root)tree->m_ct->DeleteItem(mount.root);mount.root=nullptr;released.push_back(std::move(mount.volume));
    }
    tree->enable_selchange(enabled);App().m_list->initialize_volume("",0);
    Run([&](Storage::Job&){released.clear();});
}
void MainMacBrowser::DetachTree(){state_->restore.clear();int i;uint32_t id;if(state_->Selected(i,id)){
    state_->current=i;for(auto item=App().m_tree->m_ct->GetSelectedItem();item;item=App().m_tree->m_ct->GetParentItem(item)){
        auto p=reinterpret_cast<lparam_struct*>(App().m_tree->m_ct->GetItemData(item));if(p&&p->type==LIST_MAC)state_->restore.push_back(uint32_t(p->id));}}
    for(auto& mount:state_->mounts)mount.root=nullptr;}
void MainMacBrowser::RebuildTree(){for(size_t i=0;i<state_->mounts.size();++i)state_->Root(int(i));
    if(!state_->restore.empty()&&state_->current>=0){int i=state_->current;auto item=state_->mounts[i].root;
        for(auto at=state_->restore.rbegin();at!=state_->restore.rend();++at){if(*at==2){state_->Show(i,2,item);continue;}
            for(auto child=App().m_tree->m_ct->GetChildItem(item);child;child=App().m_tree->m_ct->GetNextSiblingItem(child)){
                auto p=reinterpret_cast<lparam_struct*>(App().m_tree->m_ct->GetItemData(child));if(uint32_t(p->id)==*at){item=child;state_->Show(i,*at,item);break;}}}}
}
void MainMacBrowser::Clear(){state_->mounts.clear();state_->clipboard.clear();state_->shown.clear();state_->current=-1;}
bool MainMacBrowser::Activate(int row){auto p=reinterpret_cast<list_lparam_struct*>(App().m_list->GetListCtrl().GetItemData(row));
    if(!p||p->type!=LIST_MAC)return false;if(p->isdir){auto item=p->htree;App().m_tree->m_ct->SelectItem(item);Select(App().m_tree->m_ct->GetItemData(item));}
    else{auto& list=App().m_list->GetListCtrl();list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);Export(0);}return true;}
bool MainMacBrowser::NewFolder(){if(!Active())return false;if(!Writable())return true;
    int i;uint32_t id;state_->Selected(i,id);std::wstring name=L"New Folder";
    if(EditHfsPlusName(AfxGetMainWnd(),name,state_->mounts[i].volume->CaseSensitive())){
        Run([&](Storage::Job&){state_->mounts[i].volume->MakeDirectory(id,U16(name));});Refresh();}return true;}
bool MainMacBrowser::Rename(){if(!Active())return false;if(!SelectionWritable())return true;
    auto& list=App().m_list->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return true;
    auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(p->type!=LIST_MAC)return true;auto volume=state_->mounts[p->volinx].volume;uint32_t id=uint32_t(p->id);
    Entry e;if(!Run([&](Storage::Job&){e=volume->Stat(id);}))return true;auto name=Wide(e.name);
    if(EditHfsPlusName(AfxGetMainWnd(),name,volume->CaseSensitive())){Run([&](Storage::Job&){volume->Rename(id,state_->folder,U16(name));});Refresh();}return true;}
bool MainMacBrowser::Delete(bool confirm){if(!Active())return false;if(!SelectionWritable())return true;
    std::vector<Location> selected;auto& list=App().m_list->GetListCtrl();for(int i=-1;(i=list.GetNextItem(i,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(i));if(p->type==LIST_MAC)selected.push_back(state_->Item(i,false));}
    if(selected.empty()||(confirm&&AfxMessageBox("Delete the selected files and folders?",MB_YESNO|MB_ICONQUESTION)!=IDYES))return true;
    Run([&](Storage::Job& job){for(auto& item:selected)RemoveTree(*item.volume,item.id,job);});Refresh();return true;}
bool MainMacBrowser::Properties(int row){auto p=reinterpret_cast<list_lparam_struct*>(App().m_list->GetListCtrl().GetItemData(row));if(!p||p->type!=LIST_MAC)return false;
    auto volume=state_->mounts[p->volinx].volume;Entry e;if(!Run([&](Storage::Job&){e=volume->Stat(uint32_t(p->id));}))return true;
    std::wstring text=Wide(e.name)+L"\n"+FromAnsi(volume->Description())+(volume->Writable()?L"":L" (read-only)")+L"\nData fork: "+std::to_wstring(e.forks[0].size)+L" bytes\nResource fork: "+std::to_wstring(e.forks[1].size)+L" bytes";
    MessageBoxW(AfxGetMainWnd()->m_hWnd,text.c_str(),L"Properties",MB_OK);return true;}
bool MainMacBrowser::Capture(bool cut,bool append){if(cut&&Active()&&!SelectionWritable()){if(!Hidden())AfxMessageBox("The selected entries cannot be moved.");return true;}std::vector<Location> selected;auto& list=App().m_list->GetListCtrl();
    for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(p->type!=LIST_SPEC)selected.push_back(state_->Item(row,cut));}
    if(cut)for(auto& item:selected)if(item.volume&&!item.volume->Writable()){AfxMessageBox("This volume is read-only.");return true;}
    if(!selected.empty()){if(!append)state_->clipboard.clear();state_->clipboard.insert(state_->clipboard.end(),selected.begin(),selected.end());}
    return Active();}
void MainMacBrowser::LegacyClipboardChanged(){
    auto list=App().m_list;state_->clipboard.clear();
    for(int i=0;i<list->m_clipboard.GetSize();++i){Location item;item.type=list->m_clipboard_type.GetAt(i);item.index=list->m_clipboard_volinx.GetAt(i);item.cut=list->m_clipboard_cutting.GetAt(i)!=0;item.directory=list->m_clipboard_isdir.GetAt(i)!=0;
        if(item.type==LIST_FAT)item.windowsPath=FromAnsi((LPCSTR)list->m_clipboard.GetAt(i));else{item.classicPath=list->m_clipboard.GetAt(i);item.classicImage=list->m_clipboard_volpath.GetAt(i);}state_->clipboard.push_back(std::move(item));}
}
bool MainMacBrowser::Paste(){bool relevant=Active();for(auto& item:state_->clipboard)relevant=relevant||bool(item.volume);if(!relevant)return false;
    if(state_->clipboard.empty())return true;try{if(state_->Transfer(state_->clipboard,state_->Target()))state_->clipboard.erase(std::remove_if(state_->clipboard.begin(),state_->clipboard.end(),[](const Location& e){return e.cut;}),state_->clipboard.end());}
    catch(const std::exception& e){AfxMessageBox(e.what());}
    auto list=App().m_list;list->empty_clipboard();
    for(auto& item:state_->clipboard)if(!item.volume){
        CString path=item.type==LIST_FAT?CString(item.windowsPath.c_str()):CString(item.classicPath.c_str());
        list->m_clipboard.Add(path);list->m_clipboard_type.Add(item.type);list->m_clipboard_isdir.Add(item.directory);
        list->m_clipboard_volpath.Add(item.classicImage.c_str());list->m_clipboard_cutting.Add(item.cut);list->m_clipboard_volinx.Add(item.index);
    }
    return true;
}
int MainMacBrowser::Drop(int row,HTREEITEM target,int action){
    if(row<0||!target)return -1;auto p=reinterpret_cast<list_lparam_struct*>(App().m_list->GetListCtrl().GetItemData(row));
    auto q=reinterpret_cast<lparam_struct*>(App().m_tree->m_ct->GetItemData(target));
    if(!p||!q||(p->type!=LIST_MAC&&q->type!=LIST_MAC))return -1;
    if(p->type==LIST_SPEC)return 0;
    try{return state_->Transfer({state_->Item(row,action==2)},state_->Target(target))?1:0;}
    catch(const std::exception& e){AfxMessageBox(e.what());return 0;}
}
bool MainMacBrowser::DropSelection(HTREEITEM target,int action){
    if(!target)return false;auto p=reinterpret_cast<lparam_struct*>(App().m_tree->m_ct->GetItemData(target));if(!p)return false;
    bool relevant=p->type==LIST_MAC;std::vector<Location> sources;auto& list=App().m_list->GetListCtrl();
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
    for(auto item=App().m_tree->m_ct->GetSelectedItem();item;item=App().m_tree->m_ct->GetParentItem(item)){
        auto p=reinterpret_cast<lparam_struct*>(App().m_tree->m_ct->GetItemData(item));if(p&&p->type==LIST_MAC)location.nativePath.insert(location.nativePath.begin(),uint32_t(p->id));}
    Run([&](Storage::Job&){for(size_t n=1;n<location.nativePath.size();++n)location.directory+="/"+BrowserPresentation::Utf8(location.native->Stat(location.nativePath[n]).name);});
    FindHit found;if(!ShowFindFilesDialog(AfxGetMainWnd(),location,found))return true;
    auto item=state_->mounts[index].root;state_->Show(index,2,item);
    size_t count=found.nativePath.size()-(found.folder?0:1);
    for(size_t n=1;n<count;++n)for(auto child=App().m_tree->m_ct->GetChildItem(item);child;child=App().m_tree->m_ct->GetNextSiblingItem(child)){
        auto p=reinterpret_cast<lparam_struct*>(App().m_tree->m_ct->GetItemData(child));if(uint32_t(p->id)==found.nativePath[n]){item=child;state_->Show(index,uint32_t(p->id),item);break;}}
    if(!found.folder&&!found.nativePath.empty()){auto& list=App().m_list->GetListCtrl();for(int row=0;row<list.GetItemCount();++row){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(p->type==LIST_MAC&&uint32_t(p->id)==found.nativePath.back()){list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);list.EnsureVisible(row,FALSE);break;}}}
    return true;
}
void MainMacBrowser::Import(){if(!Active()||!Writable())return;std::wstring path;if(!FileDialog(path,false))return;
    Location source;source.type=LIST_FAT;source.windowsPath=path;try{state_->Transfer({source},state_->Target());}catch(const std::exception& e){AfxMessageBox(e.what());}}
void MainMacBrowser::ImportResource(){
    if(!Active()||!Writable())return;auto& list=App().m_list->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return;
    auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(!p||p->type!=LIST_MAC||p->isdir)return;
    auto volume=state_->mounts[p->volinx].volume;uint32_t id=uint32_t(p->id),parent=state_->folder;
    std::wstring path;if(!FileDialog(path,false))return;Location input;input.type=LIST_FAT;input.windowsPath=path;
    Run([&](Storage::Job& job){auto from=Source(input);auto original=volume->Stat(id),replacement=original;
        replacement.forks[1].size=from.first->Stat(from.second).forks[0].size;replacement.modified=uint32_t(time(nullptr)+2082844800LL);
        Storage::Progress progress;progress.fileCopy=progress.byteTransfer=progress.utf8=true;progress.scanning=false;
        progress.path=BrowserPresentation::Utf8(original.name);progress.totalBytes=progress.fileTotal=replacement.forks[0].size+replacement.forks[1].size;
        volume->Put(parent,replacement,[&](unsigned fork,uint64_t at,void* data,size_t n){
            if(job.cancelled())throw Storage::Cancelled();
            if(fork)from.first->Read(from.second,0,at,data,n);else volume->Read(id,0,at,data,n);
            progress.bytes+=n;progress.fileBytes+=n;job.progress(progress);
        },true);
    },true,true);Refresh();
}
void MainMacBrowser::Export(unsigned fork){if(!Active())return;auto& list=App().m_list->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return;
    auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(!p||p->type!=LIST_MAC||p->isdir)return;auto volume=state_->mounts[p->volinx].volume;uint32_t id=uint32_t(p->id);Entry entry;
    if(!Run([&](Storage::Job&){entry=volume->Stat(id);}))return;auto path=Wide(entry.name)+(fork?L".rsrc":L"");if(!FileDialog(path,true))return;path=BrowserPresentation::WindowsPath(path);
    Run([&](Storage::Job& job){volume->Read(id,fork,0,nullptr,0);HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw HfsPlus::Error("Cannot create output; choose a new filename");
        try{std::vector<uint8_t> bytes(1024*1024);Storage::Progress progress;progress.fileCopy=progress.byteTransfer=progress.utf8=true;progress.path=BrowserPresentation::Utf8(entry.name);progress.scanning=false;progress.totalBytes=progress.fileTotal=entry.forks[fork].size;
            for(uint64_t at=0;at<entry.forks[fork].size;){if(job.cancelled())throw Storage::Cancelled();size_t n=size_t((std::min)(uint64_t(bytes.size()),entry.forks[fork].size-at));
                volume->Read(id,fork,at,bytes.data(),n);DWORD written;if(!WriteFile(file,bytes.data(),DWORD(n),&written,nullptr)||written!=n)throw HfsPlus::Error("Export write failed");at+=n;progress.bytes=progress.fileBytes=at;job.progress(progress);}
            if(!FlushFileBuffers(file))throw HfsPlus::Error("Export flush failed");CloseHandle(file);
        }catch(...){CloseHandle(file);DeleteFileW(path.c_str());throw;}},true,true);}
void MainMacBrowser::Sort(int column,bool ascending){
    if(!Active())return;
    struct SortContext {int column;bool ascending;std::map<uint32_t,Entry> entries;} context{column,ascending,{}};
    for(const auto& e:state_->shown)context.entries[e.id]=e;
    auto compare=[](LPARAM left,LPARAM right,LPARAM raw)->int {
        auto& c=*reinterpret_cast<SortContext*>(raw);auto a=reinterpret_cast<list_lparam_struct*>(left),b=reinterpret_cast<list_lparam_struct*>(right);
        if(a->type==LIST_SPEC||b->type==LIST_SPEC)return a->type==b->type?0:a->type==LIST_SPEC?-1:1;
        auto ia=c.entries.find(uint32_t(a->id)),ib=c.entries.find(uint32_t(b->id));if(ia==c.entries.end()||ib==c.entries.end())return 0;
        auto& x=ia->second;auto& y=ib->second;if(x.directory!=y.directory)return x.directory?-1:1;int result=0;
        if(c.column==FIELD_SIZE){auto xs=x.forks[0].size+x.forks[1].size,ys=y.forks[0].size+y.forks[1].size;result=xs<ys?-1:xs>ys?1:0;}
        else if(c.column==FIELD_CREATED||c.column==FIELD_MODIFIED){uint32_t xs=c.column==FIELD_CREATED?x.created:x.modified,ys=c.column==FIELD_CREATED?y.created:y.modified;result=xs<ys?-1:xs>ys?1:0;}
        else if(c.column==FIELD_TYPE||c.column==FIELD_CREATOR){unsigned at=c.column==FIELD_TYPE?0:4;result=memcmp(x.finder.data()+at,y.finder.data()+at,4);}
        if(!result)result=CompareStringOrdinal(reinterpret_cast<const wchar_t*>(x.name.data()),int(x.name.size()),reinterpret_cast<const wchar_t*>(y.name.data()),int(y.name.size()),TRUE)-CSTR_EQUAL;
        return c.ascending?result:-result;
    };
    App().m_list->GetListCtrl().SortItems(compare,reinterpret_cast<DWORD_PTR>(&context));
}
bool MainMacBrowser::ContextMenu(CPoint point){
    if(!Active())return false;auto& list=App().m_list->GetListCtrl();
    if(point.x>=0){CPoint local=point;list.ScreenToClient(&local);int row=list.HitTest(local);if(row>=0&&!list.GetItemState(row,LVIS_SELECTED)){list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);}}
    else{CRect rect;list.GetWindowRect(&rect);point=rect.CenterPoint();}
    bool selected=list.GetNextItem(-1,LVNI_SELECTED)>=0;UINT write=Writable()?MF_ENABLED:MF_GRAYED;UINT selectionWrite=SelectionWritable()?MF_ENABLED:MF_GRAYED;
    CMenu menu;menu.CreatePopupMenu();menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_OWN_FILE_OPEN,"&Open");
    menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_EDIT_COPY,"&Copy");menu.AppendMenu(MF_STRING|(selected?selectionWrite:MF_GRAYED),ID_EDIT_CUT,"Cu&t");
    menu.AppendMenu(MF_STRING|write,ID_EDIT_PASTE,"&Paste");menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING|write,ID_OWN_NEW_FOLDER,"New &folder...");menu.AppendMenu(MF_STRING|(selected?selectionWrite:MF_GRAYED),ID_EDIT_LABEL,"&Rename...");
    menu.AppendMenu(MF_STRING|(selected?selectionWrite:MF_GRAYED),ID_OWN_DELETE,"&Delete");
    menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_OWN_FILE_PROPERTIES,"P&roperties...");
    UINT command=menu.TrackPopupMenu(TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,AfxGetMainWnd());
    int row=list.GetNextItem(-1,LVNI_SELECTED);
    switch(command){case ID_EDIT_COPY:Capture(false,false);break;case ID_EDIT_CUT:Capture(true,false);break;case ID_EDIT_PASTE:Paste();break;
        case ID_OWN_NEW_FOLDER:NewFolder();break;case ID_EDIT_LABEL:Rename();break;case ID_OWN_DELETE:Delete(true);break;
        case ID_OWN_FILE_OPEN:if(row>=0)Activate(row);break;case ID_OWN_FILE_PROPERTIES:if(row>=0)Properties(row);break;}
    return true;
}

#ifdef HFV_TESTING
int MainMacBrowser::AttachTestVolume(std::shared_ptr<Volume> volume,const char* key){state_->mounts.push_back({volume,FromAnsi(key),nullptr,-1});int i=int(state_->mounts.size()-1);state_->Root(i);return i;}
bool MainMacBrowser::ShowTestFolder(int index,uint32_t id){return state_->Show(index,id,state_->mounts.at(index).root);}
#endif
