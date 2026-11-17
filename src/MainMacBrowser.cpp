// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MetadataDialogs.h"
#include "HFVExplorer.h"
#include "MainMacBrowser.h"
#include "FileOperations.h"
#include "BackgroundCopies.h"
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
#ifdef HFV_TESTING
std::string browserTransferError;
#endif
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
bool readBusy=false;
bool Run(BrowserSession& session_,std::function<void(Storage::Job&)> work,bool copy=false,bool single=false) {
    auto body=[&](Storage::Job& job){try{work(job);return Storage::Result{};}catch(const VolumeCoord::Busy& e){return Storage::Result{1,ERROR_BUSY,e.what()};}};
    auto result=copy?ShowStorageJob(body,"Copying files",Hidden(session_),single,true,session_.Owner()):ShowStorageWaitJob(body,Hidden(session_),session_.Owner());
    readBusy=result.error==ERROR_BUSY;
    if(result.code&&result.error!=ERROR_BUSY&&result.error!=ERROR_CANCELLED&&!Hidden(session_)&&!session_.Refreshing())AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);
    return result.code==0;
}
bool RunOperation(BrowserSession& session,FileOperations::Request request,bool copy=false,bool single=false){
 if(BackgroundCopies::Enabled()&&(request.kind==FileOperations::Kind::CreateHardLink||request.kind==FileOperations::Kind::Rename||request.kind==FileOperations::Kind::Delete||request.kind==FileOperations::Kind::CreateFolder||request.kind==FileOperations::Kind::SetProperties||request.kind==FileOperations::Kind::ReplaceResource||(copy&&request.kind==FileOperations::Kind::ExportFork)))return BackgroundCopies::Start({std::move(request)},session.Owner(),single);
 auto operation=FileOperations::Submit(std::move(request));auto result=ShowSubmittedStorageJob(operation.Job(),copy?"Copying files":"Updating files",Hidden(session),single,true,session.Owner());
#ifdef HFV_TESTING
 browserTransferError=result.message;
#endif
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
        else if(p->type==LIST_FAT){out.windowsPath=FromAnsi(p->path);auto list=session_.List();if(!_wcsicmp(out.windowsPath.c_str(),list->m_browser_folder_path.c_str()))out.expectedIdentity=list->m_browser_folder_identity;}
        else if(p->type==LIST_HFS){int mac=0;out.classicImage=session_.Document()->m_volumes[p->volinx].m_file_name;
            out.expectedCatalogId=uint32_t(p->id);out.expectedResource=VolumeCoord::ImageKey(out.classicImage);out.classicPath=session_.Document()->get_directory(item,&mac);auto& v=session_.Document()->m_volumes[p->volinx];out.mediaGenerationKnown=v.m_mediaGenerationKnown;out.mediaGeneration=v.m_mediaGeneration;}
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
        if(p->type==LIST_HFS){auto& v=session_.Document()->m_volumes[p->volinx];out.mediaGenerationKnown=v.m_mediaGenerationKnown;out.mediaGeneration=v.m_mediaGeneration;out.expectedCatalogId=uint32_t(p->id);out.expectedResource=VolumeCoord::ImageKey(out.classicImage);}
        if(p->type==LIST_FAT){auto rowValue=lv->BrowserRowAt(row);if(rowValue)out.expectedIdentity=rowValue->entry.identity;}
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
        if(showing)return true;
        auto title=Wide(mounts.at(index).volume->Name());if(id!=2&&item){wchar_t leaf[1024]{};TVITEMW value{};value.mask=TVIF_TEXT;value.hItem=item;value.pszText=leaf;value.cchTextMax=ARRAYSIZE(leaf);if(SendMessageW(session_.Tree()->m_ct->m_hWnd,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&value)))title+=L":"+std::wstring(leaf);}
        session_.SetLocationTitle(std::move(title));
        if(session_.Defer(mounts.at(index).volume->ResourceKey(),mounts.at(index).drive>=0,[this,index,id,item,rebuild]{Show(index,id,item,rebuild);}))return true;
        session_.ClearCurrent();showing=true;struct Done{bool& flag;~Done(){flag=false;}}done{showing};
        auto volume=mounts.at(index).volume;uint64_t revision=0;std::wstring folderTitle;std::vector<std::pair<uint32_t,std::vector<Entry>>> levels;std::vector<Entry> entries;uint64_t free=0;bool mutableFolder=true;
        std::vector<uint32_t> ancestors;std::vector<std::pair<uint32_t,std::u16string>> savedPath;
        for(auto a=item;a;a=session_.Tree()->m_ct->GetParentItem(a)){auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(a));ancestors.push_back(uint32_t(p->id));wchar_t text[1024]{};TVITEMW value{};value.mask=TVIF_TEXT;value.hItem=a;value.pszText=text;value.cchTextMax=ARRAYSIZE(text);SendMessageW(session_.Tree()->m_ct->m_hWnd,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&value));savedPath.push_back({uint32_t(p->id),U16(text)});}
        if(!Run(session_,[&](Storage::Job&){
            VolumeCoord::Lease read({{volume->ResourceKey(),VolumeCoord::Mode::Read}});revision=VolumeCoord::Revision(volume->ResourceKey());
            if(rebuild){
                // Follow the visible path rather than backing-inode parent IDs.
                // Converting a folder to hard links retains its inode CNID but
                // gives the original visible name a new catalog CNID.
                id=2;std::vector<uint32_t> chain{2};
                for(auto old=savedPath.rbegin();old!=savedPath.rend();++old){if(old->first==2)continue;auto children=volume->List(id);auto next=std::find_if(children.begin(),children.end(),[&](const Entry& e){return e.directory&&(e.id==old->first||(e.hardLink&&e.resolvedId==old->first&&e.name==old->second));});if(next==children.end())break;auto nextId=next->id;levels.push_back({id,std::move(children)});id=nextId;chain.push_back(id);}
                ancestors.assign(chain.rbegin(),chain.rend());
            }
            std::set<uint32_t> visited;
            for(auto ancestor:ancestors){
                auto e=volume->Stat(ancestor);auto key=e.resolvedId?e.resolvedId:e.id;
                if(!visited.insert(key).second)throw HfsPlus::Error("Cyclic directory hard link");
            }
            auto currentFolder=volume->Stat(id);folderTitle=Wide(currentFolder.name);mutableFolder=(currentFolder.id==2||!currentFolder.protectedFile);
            entries=volume->List(id);free=volume->FreeBytes();})){
            if(readBusy){session_.Defer(volume->ResourceKey(),mounts.at(index).drive>=0,[this,index,id,item,rebuild]{Show(index,id,item,rebuild);},true);return true;}
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
        session_.SetLocationTitle(id==2?Wide(volume->Name()):Wide(volume->Name())+L":"+folderTitle);
        session_.Observe(volume->ResourceKey(),revision,LIST_MAC,index,id);shown=std::move(entries);current=index;folder=id;folderMutable=mutableFolder;control.SelectItem(item);control.EnsureVisible(item);tree->enable_selchange(enabled);session_.MacBrowser().Sort(FIELD_NAME,true);
        if(session_.Status()){CString status;status.Format("%s%s - %I64u bytes free",volume->Kind(),volume->Writable()?"":" (read-only)",free);
            session_.Status()->SetPaneText(0,status);}
        return true;
    }
    bool Transfer(const std::vector<Location>& sources,const Location& target,std::function<void(bool)> completed={}) {
        if(target.type==LIST_HFS)session_.Document()->m_volumes[target.index].close();
        auto convertible=[](const Location& p){if(p.type!=LIST_HFS)return true;auto n=p.classicImage.size();return n>=4&&p.classicImage[n-4]=='.'&&(!_stricmp(p.classicImage.c_str()+n-3,"DSK")||!_strnicmp(p.classicImage.c_str()+n-3,"HF",2));};
        bool legacy=!target.volume&&convertible(target)&&std::all_of(sources.begin(),sources.end(),[&](const Location& p){return !p.volume&&convertible(p);})&&std::any_of(sources.begin(),sources.end(),[&](const Location& p){return p.type!=target.type;});
        bool complete=true;std::vector<FileOperations::Request> background;
        if(legacy){
         session_.List()->m_shutup_for_now=FALSE;
         for(auto& source:sources){
          CString sourcePath=source.type==LIST_HFS?CString(source.classicPath.c_str()):CString(source.windowsPath.c_str());
          if(!BackgroundCopies::Enabled()&&source.type!=target.type&&!Hidden(session_)&&!session_.List()->ask_set_copy_mode(source.type==LIST_HFS,sourcePath)){complete=false;break;}
          FileOperations::Request request;request.kind=source.type==target.type?FileOperations::Kind::Copy:FileOperations::Kind::LegacyCopy;request.sources={source};request.target=target;
          request.sourceVolume=source.classicImage;request.sourcePath=(LPCSTR)sourcePath;
          request.targetVolume=target.classicImage;request.targetPath=target.type==LIST_HFS?target.classicPath:(LPCSTR)CString(target.windowsPath.c_str());
          request.askMode=request.kind==FileOperations::Kind::LegacyCopy&&(source.type==LIST_HFS?App().m_copyout_mode:App().m_copyin_mode)==6;
          request.directory=source.directory;request.move=source.cut;request.conversion=CaptureLegacyCopyOptions(HFSIFACE_copy_mode());
          if(BackgroundCopies::Enabled())background.push_back(std::move(request));else if(!RunOperation(session_,std::move(request),true,!source.directory)){complete=false;break;}
         }
        }else{
         FileOperations::Request request;request.kind=FileOperations::Kind::Copy;request.sources=sources;request.target=target;
         if(BackgroundCopies::Enabled())background.push_back(std::move(request));else complete=RunOperation(session_,std::move(request),true,sources.size()==1&&!sources[0].directory);
        }
        if(BackgroundCopies::Enabled()){if(!complete)return false;return BackgroundCopies::Start(std::move(background),session_.Owner(),sources.size()==1&&!sources[0].directory,std::move(completed));}
        if(completed)completed(complete);
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
std::string MainMacBrowser::SelectedResourceKey()const{int index;uint32_t id;if(state_->Selected(index,id))return state_->mounts[index].volume->ResourceKey();auto tree=session_.Tree();auto doc=session_.Document();if(!tree||!doc)return {};auto item=tree->m_ct->GetSelectedItem();auto p=item?reinterpret_cast<lparam_struct*>(tree->m_ct->GetItemData(item)):nullptr;if(!p)return {};if(p->type==LIST_HFS&&p->volinx>=0&&p->volinx<doc->m_hfs_count)return doc->m_volumes[p->volinx].m_file_name.IsEmpty()?std::string():VolumeCoord::ImageKey(std::string((LPCSTR)doc->m_volumes[p->volinx].m_file_name));if(p->type==LIST_FAT)return session_.ObservedWindowsKey(p->path);return {};}
bool MainMacBrowser::Active()const{int index;uint32_t id;return state_->Selected(index,id);}
bool MainMacBrowser::Writable()const{int i;uint32_t id;return state_->Selected(i,id)&&state_->folderMutable&&state_->mounts[i].volume->Supports().create;}
bool MainMacBrowser::CanCreateFolder()const{int i;uint32_t id;return Writable()&&state_->Selected(i,id)&&state_->mounts[i].volume->Supports().createFolders;}
bool MainMacBrowser::SelectionWritable()const{
    if(!Writable())return false;auto& list=session_.List()->GetListCtrl();bool selected=false;
    for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));
        if(!p||p->type!=LIST_MAC)continue;selected=true;for(auto& e:state_->shown)if(e.id==uint32_t(p->id)&&(e.protectedFile||!e.readError.empty()))return false;}
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
int MainMacBrowser::OpenImage(const std::wstring& path,const std::wstring& externalJournal) {
    auto wide=path;wchar_t full[32768];if(GetFullPathNameW(wide.c_str(),ARRAYSIZE(full),full,nullptr))wide=full;
    for(size_t i=0;i<state_->mounts.size();++i)if(!_wcsicmp(wide.c_str(),state_->mounts[i].key.c_str())){
        if(!state_->mounts[i].root)state_->Root(int(i));state_->Show(int(i),2,state_->mounts[i].root);return 1;}
    auto key=VolumeCoord::ImageKey(wide);if(session_.Defer(key,false,[this,wide,externalJournal]{OpenImage(wide,externalJournal);}))return 1;
    std::shared_ptr<Volume> volume;std::string journalPrompt;bool ok=Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{key,VolumeCoord::Mode::Read}});try{volume=MacBrowser::OpenImage(wide,externalJournal);}catch(const HfsPlus::ExternalJournalRequired& e){journalPrompt=e.what();}});
    if(ok&&!journalPrompt.empty()){
        if(Hidden(session_))return -1;
        std::wstring external;if(!SelectHfsPlusExternalJournal(session_.Owner(),external,journalPrompt))return -1;
        ok=Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{key,VolumeCoord::Mode::Read},{VolumeCoord::ImageKey(external),VolumeCoord::Mode::Read}});volume=MacBrowser::OpenImage(wide,external);});
    }
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
    bool detected=false,busy=false;std::shared_ptr<Volume> volume;auto result=ShowStorageWaitJob([&](Storage::Job&){try{VolumeCoord::Lease read({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}});volume=MacBrowser::OpenCdRoot(root);}catch(const VolumeCoord::Busy&){busy=true;}catch(const MacBrowser::DetectedVolumeError&){detected=true;throw;}return Storage::Result{};},Hidden(session_),session_.Owner());
    if(busy){std::string saved=root;auto doc=session_.Document();int index=-1;for(int n=0;n<doc->m_fat_count;++n)if(toupper((unsigned char)doc->m_fats[n].m_letter)-'A'==drive){index=n;break;}session_.Defer(VolumeCoord::DeviceKey(drive),true,[this,index,saved]{if(index>=0)session_.Document()->open_fat_directory(index,const_cast<char*>(saved.c_str()),1);else OpenCd(saved.c_str());},true);return 1;}
    if(result.code){if(detected){if(!Hidden(session_))AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);return -1;}return 0;}if(!volume)return 0;state_->mounts.push_back({volume,FromAnsi(root),nullptr,drive});int i=int(state_->mounts.size()-1);
    state_->Root(i);state_->Show(i,2,state_->mounts[i].root);return 1;
}
bool MainMacBrowser::Select(LPARAM data){auto p=reinterpret_cast<lparam_struct*>(data);if(!p||p->type!=LIST_MAC)return false;
    state_->Show(p->volinx,uint32_t(p->id),session_.Tree()->m_ct->GetSelectedItem());return true;}
bool MainMacBrowser::Refresh(){int i;uint32_t id;if(!state_->Selected(i,id))return false;
    auto& mount=state_->mounts[i];
    if(session_.Defer(mount.volume->ResourceKey(),mount.drive>=0,[this]{Refresh();}))return true;
    bool ready=Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{mount.volume->ResourceKey(),VolumeCoord::Mode::Read}});if(mount.drive>=0){auto fresh=MacBrowser::OpenCd(mount.drive);if(!fresh)throw HfsPlus::Error("No HFS+ / HFSX disc is present");mount.volume=std::move(fresh);}else mount.volume->Reload();});
    if(!ready){if(readBusy){session_.Defer(mount.volume->ResourceKey(),mount.drive>=0,[this]{Refresh();},true);return true;}if(mount.drive>=0){ReleaseCd(mount.drive);return false;}session_.List()->initialize_volume("",0);return true;}
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
    bool present=false;for(const auto& mount:state_->mounts)present|=mount.drive==drive&&bool(mount.volume);if(!present)return;
    std::unique_ptr<VolumeCoord::OperationLease> access;try{access.reset(new VolumeCoord::OperationLease({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Write}}));}catch(const VolumeCoord::Busy&){return;}
    bool selected=SelectedCdDrive()==drive;std::vector<std::shared_ptr<Volume>> released;
    auto tree=session_.Tree();bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
    for(auto& mount:state_->mounts)if(mount.drive==drive&&mount.volume){if(mount.root)tree->m_ct->DeleteItem(mount.root);mount.root=nullptr;released.push_back(std::move(mount.volume));}
    tree->enable_selchange(enabled);if(selected){session_.ClearCurrent();session_.SetLocationTitle({});session_.List()->initialize_volume("",0);}
    Run(session_,[&](Storage::Job&){for(auto& volume:released)volume->ReleaseMedia();released.clear();});
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
void MainMacBrowser::Clear(){state_->mounts.clear();state_->shown.clear();state_->current=-1;}
bool MainMacBrowser::Activate(int row){auto p=reinterpret_cast<list_lparam_struct*>(session_.List()->GetListCtrl().GetItemData(row));
    if(!p||p->type!=LIST_MAC)return false;if(p->isdir){auto item=p->htree;session_.Tree()->m_ct->SelectItem(item);Select(session_.Tree()->m_ct->GetItemData(item));}
    else{auto& list=session_.List()->GetListCtrl();list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);Export(0);}return true;}
bool MainMacBrowser::CreateFolder(const FileOperations::Location& target,const std::u16string& name){
 FileOperations::Request request;request.kind=FileOperations::Kind::CreateFolder;request.target=target;request.name=name;return RunOperation(session_,std::move(request),false,true);
}
bool MainMacBrowser::CanPasteHardLink()const{
 if(!Writable())return false;int index;uint32_t folder;if(!state_->Selected(index,folder))return false;auto target=state_->mounts[index].volume;auto selection=BrowserTransfers::Snapshot();return target->SupportsHardLinks()&&selection.items.size()==1&&selection.items[0].volume&&selection.items[0].volume->SupportsHardLinks()&&selection.items[0].volume->ResourceKey()==target->ResourceKey();
}
bool MainMacBrowser::PasteHardLink(){
 if(!CanPasteHardLink())return true;auto selection=BrowserTransfers::Snapshot();auto target=state_->Target();std::wstring name=Wide(selection.items[0].expectedName)+L" link";
 if(!EditHfsPlusName(session_.Owner(),name,target.volume->CaseSensitive()))return true;
 FileOperations::Request request;request.kind=FileOperations::Kind::CreateHardLink;request.sources={selection.items[0]};request.sources[0].cut=false;request.target=target;request.name=U16(name);RunOperation(session_,std::move(request));if(!BackgroundCopies::Enabled())Refresh();return true;
}
bool MainMacBrowser::NewFolder(){if(!Active())return false;if(!CanCreateFolder())return true;
 auto target=state_->Target();std::wstring name=L"New Folder";
 if(EditHfsPlusName(session_.Owner(),name,target.volume->CaseSensitive())){CreateFolder(target,U16(name));if(!BackgroundCopies::Enabled())Refresh();}return true;}
FileOperations::Location MainMacBrowser::CaptureItem(int row){
 auto item=state_->Item(row,false);
 if(item.type==LIST_FAT&&item.expectedIdentity.empty()){
  if(!Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{VolumeCoord::PathKey(item.windowsPath),VolumeCoord::Mode::Read}});item.expectedIdentity=DescribeWindowsFile(item.windowsPath).identity;}))throw HfsPlus::Error("Cannot identify the selected Windows entry");
  if(item.expectedIdentity.empty())throw HfsPlus::Error("Cannot safely identify the selected Windows entry");
 }
 return item;
}
bool MainMacBrowser::RenameItem(int row,const std::u16string& name){
 auto item=CaptureItem(row);FileOperations::Request request;request.kind=FileOperations::Kind::Rename;request.sources={item};request.name=name;request.target.volume=item.volume;request.target.id=item.expectedParent?item.expectedParent:state_->folder;
 bool result=RunOperation(session_,std::move(request),false,true);if(!BackgroundCopies::Enabled()&&Active())Refresh();return result;
}
bool MainMacBrowser::Rename(){if(!Active())return false;if(!SelectionWritable())return true;
 auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return true;
 auto item=state_->Item(row,false);auto name=Wide(item.expectedName);
 if(EditHfsPlusName(session_.Owner(),name,item.volume->CaseSensitive())){FileOperations::Request request;request.kind=FileOperations::Kind::Rename;request.sources={item};request.target.volume=item.volume;request.target.id=item.expectedParent;request.name=U16(name);RunOperation(session_,std::move(request),false,true);if(!BackgroundCopies::Enabled())Refresh();}return true;
}
bool MainMacBrowser::Delete(bool confirm){if(!Active())return false;if(!SelectionWritable())return true;
    std::vector<Location> selected;auto& list=session_.List()->GetListCtrl();for(int i=-1;(i=list.GetNextItem(i,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(i));if(p->type==LIST_MAC)selected.push_back(state_->Item(i,false));}
    if(selected.empty()||(confirm&&session_.Owner()->MessageBox("Delete the selected files and folders?","HFVExplorer",MB_YESNO|MB_ICONQUESTION)!=IDYES))return true;
    FileOperations::Request request;request.kind=FileOperations::Kind::Delete;request.sources=std::move(selected);RunOperation(session_,std::move(request),false,true);if(!BackgroundCopies::Enabled())Refresh();return true;}
bool MainMacBrowser::Properties(int row){
 if(BackgroundCopies::Enabled()){auto item=state_->Item(row,false);if(item.type==LIST_FAT)return false;return MetadataDialogs::Show(item,session_.Owner());}
auto p=reinterpret_cast<list_lparam_struct*>(session_.List()->GetListCtrl().GetItemData(row));if(!p||p->type!=LIST_MAC)return false;
    auto volume=state_->mounts[p->volinx].volume;Entry e;if(!Run(session_,[&](Storage::Job&){e=volume->Stat(uint32_t(p->id));}))return true;
    std::wstring text=Wide(e.name)+L"\n"+FromAnsi(volume->Description())+(volume->Writable()?L"":L" (read-only)")+L"\nData fork: "+std::to_wstring(e.forks[0].size)+L" bytes\nResource fork: "+std::to_wstring(e.forks[1].size)+L" bytes";
    if(e.hardLink)text+=L"\nHard links: "+std::to_wstring(e.linkCount);MessageBoxW(session_.Owner()->m_hWnd,text.c_str(),L"Properties",MB_OK);return true;}
BrowserTransfers::Selection MainMacBrowser::SelectedItems(bool cut){
 BrowserTransfers::Selection out;out.canMove=session_.List()->BrowserSelectionAllows(false);
 if(cut&&!out.canMove)return out;
 auto& list=session_.List()->GetListCtrl();
 for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){auto p=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(p&&p->type!=LIST_SPEC)out.items.push_back(state_->Item(row,cut));}
 bool missing=false;for(auto& item:out.items)missing|=item.type==LIST_FAT&&item.expectedIdentity.empty();
 if(missing&&!Run(session_,[&](Storage::Job&){for(auto& item:out.items)if(item.type==LIST_FAT&&item.expectedIdentity.empty()){VolumeCoord::Lease read({{VolumeCoord::PathKey(item.windowsPath),VolumeCoord::Mode::Read}});item.expectedIdentity=MacBrowser::DescribeWindowsFile(item.windowsPath).identity;}}))out.items.clear();
 return out;
}
FileOperations::Location MainMacBrowser::TargetFolder(HTREEITEM item){return state_->Target(item);}
bool MainMacBrowser::TransferSelection(const std::vector<Location>& sources,const Location& target,std::function<void(bool)> completed){
 try{return !sources.empty()&&state_->Transfer(sources,target,std::move(completed));}catch(const std::exception& e){if(!Hidden(session_))session_.Owner()->MessageBox(e.what(),"HFVExplorer",MB_OK|MB_ICONERROR);return false;}
}
bool MainMacBrowser::Capture(bool cut,bool append){BrowserTransfers::Publish(SelectedItems(cut),append);return true;}
bool MainMacBrowser::Paste(){try{return BrowserTransfers::Paste(session_);}catch(const std::exception& e){if(!Hidden(session_))session_.Owner()->MessageBox(e.what(),"HFVExplorer",MB_OK|MB_ICONERROR);return true;}}
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
    bool relevant=BackgroundCopies::Enabled()||p->type==LIST_MAC;std::vector<Location> sources;auto& list=session_.List()->GetListCtrl();
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
    auto selected=state_->Item(row,false);
    std::wstring path;if(!FileDialog(session_,path,false))return;Location input;input.type=LIST_FAT;input.windowsPath=path;
    FileOperations::Request request;request.kind=FileOperations::Kind::ReplaceResource;request.sources={input};request.target=selected;
    RunOperation(session_,std::move(request),true,true);if(!BackgroundCopies::Enabled())Refresh();
}
void MainMacBrowser::Export(unsigned fork){if(!Active())return;auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return;
 auto item=state_->Item(row,false);if(item.directory)return;auto path=Wide(item.expectedName)+(fork?L".rsrc":L"");if(!FileDialog(session_,path,true))return;path=BrowserPresentation::WindowsPath(path);
 FileOperations::Request request;request.kind=FileOperations::Kind::ExportFork;request.sources={item};request.target.type=LIST_FAT;request.target.windowsPath=path;request.fork=fork;RunOperation(session_,std::move(request),true,true);}

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
    menu.AppendMenu(MF_STRING|(Writable()&&BrowserTransfers::Any()?MF_ENABLED:MF_GRAYED),ID_EDIT_PASTE,"&Paste");menu.AppendMenu(MF_STRING|(CanPasteHardLink()?MF_ENABLED:MF_GRAYED),ID_EDIT_PASTE_HARD_LINK,"Paste hard &link...");menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING|(CanCreateFolder()?MF_ENABLED:MF_GRAYED),ID_OWN_NEW_FOLDER,"New &folder...");menu.AppendMenu(MF_STRING|(selected?selectionWrite:MF_GRAYED),ID_EDIT_LABEL,"&Rename...");
    menu.AppendMenu(MF_STRING|(selected?selectionWrite:MF_GRAYED),ID_OWN_DELETE,"&Delete");
    menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_OWN_FILE_PROPERTIES,"P&roperties...");
    UINT command=menu.TrackPopupMenu(TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,session_.Owner());
    int row=list.GetNextItem(-1,LVNI_SELECTED);
    switch(command){case ID_EDIT_COPY:Capture(false,false);break;case ID_EDIT_CUT:Capture(true,false);break;case ID_EDIT_PASTE:Paste();break;case ID_EDIT_PASTE_HARD_LINK:PasteHardLink();break;
        case ID_OWN_NEW_FOLDER:NewFolder();break;case ID_EDIT_LABEL:Rename();break;case ID_OWN_DELETE:Delete(true);break;
        case ID_OWN_FILE_OPEN:if(row>=0)Activate(row);break;case ID_OWN_FILE_PROPERTIES:if(row>=0)Properties(row);break;}
    return true;
}

#ifdef HFV_TESTING
size_t MainMacBrowser::TestClipboardCount()const{return BrowserTransfers::Snapshot().items.size();}
int MainMacBrowser::AttachTestVolume(std::shared_ptr<Volume> volume,const char* key,int drive){state_->mounts.push_back({volume,FromAnsi(key),nullptr,drive});int i=int(state_->mounts.size()-1);state_->Root(i);return i;}
bool MainMacBrowser::ShowTestFolder(int index,uint32_t id){return state_->Show(index,id,state_->mounts.at(index).root);}
#endif
