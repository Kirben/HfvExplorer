// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "WorkerEndpoint.h"
#include "MetadataDialogs.h"
#include "PhysicalRemovable.h"
#include "MediaDiscovery.h"
#include "CommandStorage.h"
#include "floppy.h"
#include "HFVExplorer.h"
#include "MainMacBrowser.h"
#include "FileOperations.h"
#include "BackgroundCopies.h"
#include "VolumeCoordinator.h"
#include "BrowserSession.h"
#include "StorageLocation.h"
#include "BrowserPresentation.h"
#include "BrowserDirectory.h"
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
#include <deque>
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
bool Run(BrowserSession& session_,std::function<void(Storage::Job&)> work,bool copy=false,bool single=false) {
    auto body=[&](Storage::Job& job){try{work(job);return Storage::Result{};}catch(const VolumeCoord::Busy& e){return Storage::Result{1,ERROR_BUSY,e.what()};}};
    auto result=copy?ShowStorageJob(body,"Copying files",Hidden(session_),single,true,session_.Owner()):ShowStorageWaitJob(body,Hidden(session_),session_.Owner());
    if(result.code&&result.error!=ERROR_BUSY&&result.error!=ERROR_CANCELLED&&!Hidden(session_)&&!session_.Refreshing())AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);
    return result.code==0;
}
bool RunOperation(BrowserSession& session,FileOperations::Request request,bool copy=false,bool single=false){
 if(BackgroundCopies::Enabled()&&(request.OperationKind()==FileOperations::Kind::CreateHardLink||request.OperationKind()==FileOperations::Kind::Rename||request.OperationKind()==FileOperations::Kind::Delete||request.OperationKind()==FileOperations::Kind::CreateFolder||request.OperationKind()==FileOperations::Kind::SetProperties||request.OperationKind()==FileOperations::Kind::ReplaceResource||(copy&&request.OperationKind()==FileOperations::Kind::ExportFork)))return BackgroundCopies::Start({std::move(request)},session.Owner(),single);
 auto operation=FileOperations::Submit(std::move(request));auto result=ShowSubmittedStorageJob(operation.Job(),copy?"Copying files":"Updating files",Hidden(session),single,true,session.Owner());
#ifdef HFV_TESTING
 browserTransferError=result.message;
#endif
 if(result.code&&result.error!=ERROR_CANCELLED&&!Hidden(session))session.Owner()->MessageBox(result.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);
 return !result.code&&operation.Snapshot().complete;
}
using Location=FileOperations::Location;

// Windows share these snapshots on the browser UI thread. Weak mount
// references keep cached listings from retaining image handles.
struct FolderSnapshot {
    std::weak_ptr<Volume> volume;
    uint64_t revision;
    uint32_t folder;
    std::vector<uint32_t> ancestry;
    std::wstring title;
    std::vector<Entry> entries;
    uint64_t free;
};
std::deque<FolderSnapshot> folderCache;

const FolderSnapshot* CachedFolder(const std::shared_ptr<Volume>& volume,uint64_t revision,
                                   uint32_t folder,const std::vector<uint32_t>& ancestry) {
    for(auto it=folderCache.rbegin();it!=folderCache.rend();++it)
        if(it->volume.lock()==volume && it->revision==revision && it->folder==folder && it->ancestry==ancestry)
            return &*it;
    return nullptr;
}
void RememberFolder(FolderSnapshot snapshot) {
    auto volume=snapshot.volume.lock();
    folderCache.erase(std::remove_if(folderCache.begin(),folderCache.end(),[&](const FolderSnapshot& old){
        return old.volume.expired() || (old.volume.lock()==volume &&
              (old.revision!=snapshot.revision || (old.folder==snapshot.folder && old.ancestry==snapshot.ancestry)));
    }),folderCache.end());
    constexpr size_t cacheLimit=64;
    if(folderCache.size()>=cacheLimit)folderCache.pop_front();
    folderCache.push_back(std::move(snapshot));
}
void ForgetFolders(const std::shared_ptr<Volume>& volume) {
    folderCache.erase(std::remove_if(folderCache.begin(),folderCache.end(),[&](const FolderSnapshot& entry){
        return entry.volume.expired() || entry.volume.lock()==volume;
    }),folderCache.end());
}
}
struct MainMacBrowser::State {
    BrowserSession& session_;
    explicit State(BrowserSession& session)
        :session_(session),mounts(session.Mounts().Mounted()),
         media(session.Mounts(),[&session](const auto& job){session.NotifyMediaCompletion(job);}){}
    using Mount=BrowserMountInventory::Mount;
    const std::vector<Mount>& mounts;
    MediaDiscovery::UpdateController media;

    int current=-1;uint32_t folder=2;bool folderMutable=true;
    std::vector<uint32_t> restore;
    bool Selected(int& mount,uint32_t& id,HTREEITEM item=nullptr)const {
        if(!session_.Tree()||!session_.List()||!IsWindow(session_.Tree()->GetSafeHwnd())||!IsWindow(session_.List()->GetSafeHwnd()))return false;if(!item)item=session_.Tree()->m_ct->GetSelectedItem();if(!item)return false;
        auto p=session_.Tree()->BrowserNodeAt(item);
        if(!p||p->LegacyType()!=LIST_MAC)return false;mount=p->mount.index;id=uint32_t(p->CatalogId());
        return mount>=0&&size_t(mount)<mounts.size()&&bool(mounts[mount].volume);
    }
    BrowserDirectory::Folder Folder(HTREEITEM item) {
        auto tree=session_.Tree();
        if(!item){
            if(!session_.ListingAvailable())throw HfsPlus::Error("Wait for the selected folder to load");
            item=tree->GetTreeCtrl().GetSelectedItem();
        }
        if(!item)throw HfsPlus::Error("Select a destination folder");
        if(const auto* folder=tree->BrowserFolderAt(item))return *folder;
        auto binding=tree->BrowserNodeAt(item);
        if(!binding)throw HfsPlus::Error("Select a destination folder");
        BrowserDirectory::Folder folder;
        auto& out=folder.location;
        out.directory=true;
        if(binding->mount.kind!=BrowserMountInventory::Kind::ClassicVolume)
            throw HfsPlus::Error("This item is not a destination folder");
        int mac=0;
        auto& volume=session_.Document()->m_volumes[binding->mount.index];
        const auto id=binding->CatalogId();
        auto path=id==2?std::string(":"):std::string((LPCSTR)session_.Document()->get_directory(item,&mac));
        out.SetClassic(std::string((LPCSTR)volume.m_file_name),std::move(path));
        out.expectedCatalogId=id;
        out.expectedResource=VolumeCoord::ImageKey(out.ClassicImage());
        if(volume.m_mediaGenerationKnown)out.BindMediaGeneration(volume.m_mediaGeneration);
        folder.writable=!volume.m_is_cd;
        return folder;
    }
    Location Target(HTREEITEM item=nullptr) { return Folder(item).location; }

    Location Item(int row,bool cut) {
        if(!session_.ListingAvailable())throw HfsPlus::Error("Wait for the selected folder to load");
        const auto model=session_.List()->BrowserRowAt(row);
        if(!model||model->location.IsNone())throw HfsPlus::Error("Select an available file or folder");
        auto location=model->location;
        location.cut=cut;
        return location;
    }
    HTREEITEM Insert(int index,uint32_t id,HTREEITEM parent,const std::wstring& name,bool root=false) {
        auto tree=session_.Tree();TVINSERTSTRUCTW ins{};ins.hParent=parent;ins.hInsertAfter=TVI_LAST;
        ins.item.mask=TVIF_TEXT|TVIF_IMAGE|TVIF_SELECTEDIMAGE|TVIF_PARAM;
        ins.item.pszText=const_cast<wchar_t*>(name.c_str());ins.item.iImage=ins.item.iSelectedImage=root?(mounts[index].drive>=0?9:3):0;
        ins.item.lParam=tree->StoreBrowserNode({{BrowserMountInventory::Kind::MountedVolume, index},
            BrowserDirectory::Folder{Location::Mounted(mounts[index].volume, id), mounts[index].volume->Supports().create}});
        return reinterpret_cast<HTREEITEM>(SendMessageW(tree->m_ct->m_hWnd,TVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&ins)));
    }
    void Root(int index) {
        auto& m=mounts[index];if(!m.volume)return;auto label=Wide(m.volume->Name())+L" ("+FromAnsi(m.volume->Description())+L")";
        if(!m.suffix.empty())label+=L" - "+m.suffix;
        if(m.drive>=0)label+=std::wstring(L" - ")+wchar_t('A'+m.drive)+L":";
        if(!m.volume->Writable())label+=L" [read-only]";
        const auto root=Insert(index,2,nullptr,label,true);
        session_.Mounts().SetRoot({BrowserMountInventory::Kind::MountedVolume,index},root);
    }
    bool Show(int index,uint32_t id,HTREEITEM item,bool rebuild=false,bool reload=false,const BrowserNavigation::Request* find=nullptr) {
        session_.CancelNavigation();
        auto volume=mounts.at(index).volume;
        const auto key=volume->ResourceKey();
        uint64_t revision=0;std::wstring folderTitle;std::vector<std::pair<uint32_t,std::vector<Entry>>> levels;std::vector<Entry> entries;uint64_t free=0;bool mutableFolder=true;
        std::vector<uint32_t> ancestors;std::vector<std::pair<uint32_t,std::u16string>> savedPath;
        for(auto a=item;a;a=session_.Tree()->m_ct->GetParentItem(a)){auto p=session_.Tree()->BrowserNodeAt(a);if(!p)return false;ancestors.push_back(uint32_t(p->CatalogId()));wchar_t text[1024]{};TVITEMW value{};value.mask=TVIF_TEXT;value.hItem=a;value.pszText=text;value.cchTextMax=ARRAYSIZE(text);SendMessageW(session_.Tree()->m_ct->m_hWnd,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&value));savedPath.push_back({uint32_t(p->CatalogId()),U16(text)});}
        if(find){
            savedPath.clear();
            for(auto node=find->catalogRoute.rbegin();node!=find->catalogRoute.rend();++node)
                savedPath.push_back({*node,{}});
        }
        const bool canCache=ExecutionFor(*volume).Persistent();
        bool cached=false;
        auto restoreCached=[&]{
            if(!canCache || rebuild)return false;
            revision=VolumeCoord::Revision(key);
            const auto* snapshot=CachedFolder(volume,revision,id,ancestors);
            if(!snapshot)return false;
            folderTitle=snapshot->title;
            entries=snapshot->entries;
            free=snapshot->free;
            return true;
        };
        if(!reload&&!VolumeCoord::Readable(key))cached=restoreCached();
        const auto selectedFolder=find?find->folder:Location::Mounted(volume,id);
        auto readFolder = [rebuild,savedPath,strict=find&&find->intent!=BrowserNavigation::Intent::RefreshFolder,selectedFolder](const std::shared_ptr<Volume>& volume,
            uint32_t& id,uint64_t& revision,std::vector<uint32_t>& ancestors,std::wstring& folderTitle,
            std::vector<std::pair<uint32_t,std::vector<Entry>>>& levels,std::vector<Entry>& entries,
            uint64_t& free,bool& mutableFolder) {
            revision=VolumeCoord::Revision(volume->ResourceKey());
            if(rebuild){
                // Follow the visible path rather than backing-inode parent IDs.
                // Converting a folder to hard links retains its inode CNID but
                // gives the original visible name a new catalog CNID.
                id=2;std::vector<uint32_t> chain{2};
                for(auto old=savedPath.rbegin();old!=savedPath.rend();++old){if(old->first==2)continue;auto children=volume->List(id);auto next=std::find_if(children.begin(),children.end(),[&](const Entry& e){return e.directory&&(e.id==old->first||(e.hardLink&&e.resolvedId==old->first&&e.name==old->second));});if(next==children.end()){if(strict)throw HfsPlus::Error("The search result folder changed.");break;}auto nextId=next->id;levels.push_back({id,std::move(children)});id=nextId;chain.push_back(id);}
                ancestors.assign(chain.rbegin(),chain.rend());
            }
            std::set<uint32_t> visited;
            for(auto ancestor:ancestors){
                auto e=volume->Stat(ancestor);auto key=e.resolvedId?e.resolvedId:e.id;
                if(!visited.insert(key).second)throw HfsPlus::Error("Cyclic directory hard link");
            }
            auto location=selectedFolder;
            location.SetMounted(volume,id);
            const auto currentFolder=StorageLocation::ResolveEntry(location).entry;
            if(!currentFolder.directory)throw HfsPlus::Error("The selected entry is not a folder.");
            folderTitle=Wide(currentFolder.name);
            mutableFolder=(currentFolder.id==2||!currentFolder.protectedFile);
            entries=volume->List(id);free=volume->FreeBytes();
            StorageLocation::ValidateMedia(location);
        };
        if(!cached){
            using namespace BrowserNavigation;
            Request request=find?*find:Request{Location::Mounted(volume,id),
                (rebuild||reload)?Intent::RefreshFolder:Intent::SelectFolder,key};
            request.folder.directory=true;
            struct MountedListing {
                uint64_t free=0;
                std::vector<uint32_t> ancestors;
                std::wstring title;
                std::vector<std::pair<uint32_t,std::vector<Entry>>> levels;
                std::vector<Entry> entries;
                bool mutableFolder=true;
            };
            auto listing=std::make_shared<MountedListing>();
            listing->ancestors=ancestors;
            const auto& mount=mounts[index];
            const auto mountRoot=mount.root;
            const auto drive=mount.drive;
            request.mediaDrive=drive;
            const auto physicalPath=mount.physicalPath;
            const auto physicalOffset=mount.physicalOffset;
            const auto capturedMount=session_.Mounts().Lookup({BrowserMountInventory::Kind::MountedVolume,index});
            if(!capturedMount)return false;
            const auto retry=[this,index,id,item,rebuild,reload,found=find?std::optional<Request>(*find):std::nullopt]{
                Show(index,id,item,rebuild,reload,found?&*found:nullptr);
            };
            const bool showInvisible=App().m_show_invisible_mac!=FALSE;
            session_.Navigate(std::move(request),
                [listing,readFolder,index,reload,drive,physicalPath,physicalOffset,showInvisible]
                (const Request& request,Result& result,Storage::Job& job){
                if(job.cancelled())throw Storage::Cancelled();
                try{
                    VolumeCoord::Lease access({{request.resource,VolumeCoord::Mode::Read}});
                    auto volume=request.folder.Volume();
                    auto id=request.folder.Id();
                    if(reload){
                        if(!physicalPath.empty())volume=Removable::Reopen(physicalPath,physicalOffset);
                        else if(drive>=0){
                            volume=MacBrowser::OpenCd(drive);
                            if(!volume)throw HfsPlus::Error("No supported Mac disc is present");
                        }else volume->Reload();
                    }
                    if(volume!=request.folder.Volume()){
                        id=2;
                        listing->ancestors={2};
                    }
                    auto& snapshot=result.listing;
                    snapshot.resourceKey=volume->ResourceKey();
                    readFolder(volume,id,snapshot.revision,listing->ancestors,listing->title,
                        listing->levels,listing->entries,listing->free,listing->mutableFolder);
                    result.listing.folder.SetMounted(volume,id);
                    result.listing.folder.directory=true;
                    FillListing(snapshot,listing->entries,volume->Supports(),showInvisible,listing->mutableFolder);
                }catch(const VolumeCoord::Busy&){return Storage::Result{1,ERROR_BUSY,"This volume is busy"};}
                return Storage::Result{};
            }, [this,volume,listing,item,index,mountRoot,rebuild](Result& result){
                auto target=item;
                auto mounted=result.listing.folder.Volume();
                if(mounted!=volume){
                    ForgetFolders(volume);
                    auto replacement=mounts[index];
                    replacement.volume=mounted;
                    session_.Mounts().ReplaceMounted(index,std::move(replacement));
                    session_.Tree()->RenewBrowserMount(mountRoot);
                    target=mountRoot;
                    if(!mounts[index].physicalPath.empty())
                        TreeText(session_,target,Wide(mounted->Name())+L" - "+mounts[index].suffix+L" [read-only]");
                }
                Publish(index,result.listing.folder.Id(),target,rebuild,false,mounted,result.listing.revision,listing->title,
                    listing->levels,listing->entries,listing->free,listing->mutableFolder,listing->ancestors,&result.listing);
            },capturedMount,retry);
            return true;
        }
        return Publish(index,id,item,rebuild,true,volume,revision,folderTitle,levels,entries,free,mutableFolder,ancestors);
    }
    static void FillListing(BrowserDirectory::Snapshot& snapshot,const std::vector<Entry>& entries,
        const MacBrowser::Capabilities& capabilities,bool showInvisible,bool mutableFolder)
    {
        snapshot.capabilities=capabilities;
        snapshot.capabilitiesKnown=true;
        if(!mutableFolder)
            snapshot.capabilities.create=snapshot.capabilities.rename=snapshot.capabilities.remove=false;
        snapshot.rows.reserve(entries.size());
        for(const auto& entry:entries)
            if(showInvisible||!((uint16_t(entry.finder[8])<<8|entry.finder[9])&0x4000))
                snapshot.rows.push_back(BrowserDirectory::FromMac(entry,snapshot.folder.Volume(),capabilities));
    }
    bool Publish(int index,uint32_t id,HTREEITEM item,bool rebuild,bool cached,
        const std::shared_ptr<Volume>& volume,uint64_t revision,const std::wstring& folderTitle,
        const std::vector<std::pair<uint32_t,std::vector<Entry>>>& levels,std::vector<Entry>& entries,
        uint64_t free,bool mutableFolder,const std::vector<uint32_t>& ancestors,
        const BrowserDirectory::Snapshot* prepared=nullptr)
    {
        const auto key=volume->ResourceKey();
        const bool canCache=ExecutionFor(*volume).Persistent();
        if(canCache&&!cached)RememberFolder({volume,revision,id,ancestors,folderTitle,entries,free});
        auto tree=session_.Tree();auto& control=*tree->m_ct;BrowserPresentation::Batch treeBatch(control.m_hWnd),listBatch(session_.List()->GetListCtrl().m_hWnd,true);bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
        if(rebuild){
            auto root=mounts[index].root;tree->delete_subdir_contents(root);std::map<uint32_t,HTREEITEM> pathItems;pathItems[2]=root;
            for(auto& level:levels)for(auto& entry:level.second)if(entry.directory&&(App().m_show_invisible_mac||!((uint16_t(entry.finder[8])<<8|entry.finder[9])&0x4000)||std::find(ancestors.begin(),ancestors.end(),entry.id)!=ancestors.end()))pathItems[entry.id]=Insert(index,entry.id,pathItems.at(level.first),Wide(entry.name));
            item=pathItems.at(id);
        }
        if(id!=2)TreeText(session_,item,folderTitle);
        // Replace only this directory's child nodes, never unrelated volumes.
        for(HTREEITEM child=control.GetChildItem(item);child;){auto next=control.GetNextSiblingItem(child);control.DeleteItem(child);child=next;}
        BrowserDirectory::Snapshot snapshot;
        if(!prepared){
            snapshot.resourceKey=key;
            snapshot.revision=revision;
            snapshot.folder=Location::Mounted(volume,id);
            snapshot.folder.directory=true;
            FillListing(snapshot,entries,volume->Supports(),App().m_show_invisible_mac!=FALSE,!cached&&mutableFolder);
            prepared=&snapshot;
        }
        CString status;
        status.Format("%s%s - %I64u bytes free",volume->Kind(),volume->Writable()?"":" (read-only)",free);
        const auto title=id==2?Wide(volume->Name()):Wide(volume->Name())+L":"+folderTitle;
        session_.PublishFolder(*prepared,item,id==2?std::nullopt:std::optional<std::wstring>(L""),title,status,
            [&](const BrowserDirectory::Row& row) {
                return Insert(index,row.location.Id(),item,Wide(row.entry.name));
            });
        current=index;folder=id;folderMutable=!cached&&mutableFolder;
        tree->enable_selchange(enabled);
        if(cached)session_.Defer(key,false,[this,index,id]{
            int selected;uint32_t selectedId;
            if(Selected(selected,selectedId) && selected==index && selectedId==id){
                StorageQuietRefresh quiet;
                Show(index,id,session_.Tree()->m_ct->GetSelectedItem(),true);
            }
        },true,true);
        return true;
    }
    bool Transfer(const std::vector<Location>& sources,const Location& target,std::function<void(bool)> completed)
    {
        const FileOperations::TransferSettings settings{CaptureLegacyCopyOptions(HFSIFACE_copy_mode()),
            App().m_copyin_mode==6,App().m_copyout_mode==6};
        auto requests=FileOperations::PlanTransfers({sources,target},settings);
        const bool single=sources.size()==1&&!sources.front().directory;
        if(BackgroundCopies::Enabled())
            return BackgroundCopies::Start(std::move(requests),session_.Owner(),single,std::move(completed));

        bool complete=true;
        for(auto& request:requests){
            const auto* conversion=request.Conversion();
            const bool singleOperation=conversion?!conversion->directory:single;
            if(!RunOperation(session_,std::move(request),true,singleOperation)){
                complete=false;
                break;
            }
        }
        if(completed)completed(complete);
        session_.RefreshSelected();
        return complete;
    }
};
MainMacBrowser::MainMacBrowser(BrowserSession& session):session_(session),state_(new State(session)){}
MainMacBrowser::~MainMacBrowser()=default;
void MainMacBrowser::ForgetTree(){state_->media.Cancel();state_->restore.clear();session_.Mounts().ClearMountedRoots();}
std::string MainMacBrowser::SelectedResourceKey()const{int index;uint32_t id;if(state_->Selected(index,id))return state_->mounts[index].volume->ResourceKey();auto tree=session_.Tree();auto doc=session_.Document();if(!tree||!doc)return {};auto item=tree->m_ct->GetSelectedItem();auto p=item?tree->BrowserNodeAt(item):nullptr;if(!p)return {};if(p->LegacyType()==LIST_HFS&&p->mount.index>=0&&p->mount.index<doc->m_hfs_count)return doc->m_volumes[p->mount.index].m_file_name.IsEmpty()?std::string():VolumeCoord::ImageKey(std::string((LPCSTR)doc->m_volumes[p->mount.index].m_file_name));if(p->LegacyType()==LIST_FAT){const auto* folder=tree->BrowserFolderAt(item);return folder&&folder->location.IsWindows()?session_.ObservedWindowsKey(folder->location.WindowsPath()):std::string();}return {};}
bool MainMacBrowser::Active()const{int index;uint32_t id;return state_->Selected(index,id);}
bool MainMacBrowser::Writable()const{int i;uint32_t id;return session_.ListingAvailable()&&state_->Selected(i,id)&&state_->folderMutable&&state_->mounts[i].volume->Supports().create;}
bool MainMacBrowser::CanCreateFolder()const{int i;uint32_t id;return Writable()&&state_->Selected(i,id)&&state_->mounts[i].volume->Supports().createFolders;}
bool MainMacBrowser::SelectionWritable()const{
    if(!Writable())return false;auto& list=session_.List()->GetListCtrl();bool selected=false;
    for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){
        const auto entry=session_.List()->BrowserRowAt(row);if(!entry||!entry->location.IsMounted())continue;selected=true;if(entry->entry.protectedFile||!entry->entry.readError.empty())return false;}
    return selected;
}
bool MainMacBrowser::CanExport()const{
    if(!Active()||!session_.ListingAvailable())return false;auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return false;
    auto model=session_.List()->BrowserRowAt(row);return model&&model->location.IsMounted()&&!model->entry.directory;
}
int MainMacBrowser::SelectedCdDrive()const{int i;uint32_t id;return state_->Selected(i,id)?state_->mounts[i].drive:-1;}
int MainMacBrowser::OpenImage(const char* path) {
    if(!path||!*path)return 0;return OpenImage(FromAnsi(path));
}
namespace {
std::wstring AbsoluteImagePath(const std::wstring& path)
{
    wchar_t full[32768];
    const auto size=GetFullPathNameW(path.c_str(),ARRAYSIZE(full),full,nullptr);
    return size&&size<ARRAYSIZE(full)?std::wstring(full,size):path;
}

void RememberImage(const std::wstring& path)
{
    const auto size=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,nullptr,0,nullptr,nullptr);
    if(!size)return;
    std::string ansi(size,0);
    BOOL lost=FALSE;
    if(size<=MAX_PATH&&WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,ansi.data(),size,nullptr,&lost)&&!lost){
        try{App().AddToRecentFileList(ansi.c_str());}
        catch(CException* error){error->Delete();} // Shell history is best effort after publication.
    }
}
}

int MainMacBrowser::QueueImageOpen(const std::wstring& path,const std::wstring& externalJournal,
    std::function<bool()> next)
{
    using namespace BrowserNavigation;
    Request request{};
    request.intent=Intent::OpenImage;
    request.image=ImageSource{path,externalJournal};

    // A mounted root is still read on the navigation worker. Do not change the
    // tree selection until both opening and reading the root have succeeded.
    std::shared_ptr<Volume> existing;
    for(const auto& mount:state_->mounts)
        if(mount.volume&&!_wcsicmp(path.c_str(),mount.key.c_str())){
            existing=mount.volume;
            break;
        }

    struct RootListing {std::vector<Entry> entries;uint64_t free=0;};
    auto root=std::make_shared<RootListing>();
    const bool showInvisible=App().m_show_invisible_mac!=FALSE;
    session_.Navigate(std::move(request),
        [existing,root,showInvisible](const Request& request,Result& result,Storage::Job& job){
            const auto& source=*request.image;
            const auto key=VolumeCoord::ImageKey(source.path);
            result.listing.resourceKey=key;
            if(job.cancelled())throw Storage::Cancelled();
            try{
                std::vector<VolumeCoord::Access> access{{key,VolumeCoord::Mode::Read}};
                if(!source.externalJournal.empty())
                    access.push_back({VolumeCoord::ImageKey(source.externalJournal),VolumeCoord::Mode::Read});
                VolumeCoord::Lease lease(std::move(access));
                auto volume=existing;
                if(!volume||volume->ResourceKey()!=key)
                    volume=MacBrowser::OpenBrowserImage(source.path,source.externalJournal);
                if(!volume)return Storage::Result{1,ERROR_UNRECOGNIZED_VOLUME,"This file does not contain a supported Mac volume."};
                result.listing.revision=VolumeCoord::Revision(key);
                if(job.cancelled())throw Storage::Cancelled();
                BrowserDirectory::ObserveRead("image-root");
                if(job.cancelled())throw Storage::Cancelled();
                root->entries=volume->List(2);
                root->free=volume->FreeBytes();
                auto folder=Location::Mounted(volume);
                StorageLocation::ValidateMedia(folder);
                StorageLocation::ValidateImage(source.path,key);
                folder.directory=true;
                result.listing.folder=std::move(folder);
                State::FillListing(result.listing,root->entries,volume->Supports(),showInvisible,true);
                if(job.cancelled()){
                    result.listing={};
                    throw Storage::Cancelled();
                }
            }catch(const HfsPlus::ExternalJournalRequired& error){
                result.listing.resourceKey.clear();
                result.journalPrompt=error.what();
            }catch(const VolumeCoord::Busy&){
                return Storage::Result{1,ERROR_BUSY,"The image is busy."};
            }
            return Storage::Result{};
        },
        [this,path,root,next](Result& result){
            if(!result.journalPrompt.empty()){
                if(Hidden(session_)){
                    if(!next||!next())session_.RejectNavigation({},result.journalPrompt);
                    return;
                }
                std::wstring journal;
                if(SelectHfsPlusExternalJournal(session_.Owner(),journal,result.journalPrompt))
                    QueueImageOpen(path,journal,next);
                else if(next)next();
                return;
            }
            const auto volume=result.listing.folder.Volume();
            auto match=std::find_if(state_->mounts.begin(),state_->mounts.end(),[&](const auto& mount){
                return mount.volume&&mount.volume->ResourceKey()==volume->ResourceKey();
            });
            int index;
            if(match==state_->mounts.end()){
                index=session_.Mounts().AddMounted({volume,path,nullptr,-1});
            }else index=int(match-state_->mounts.begin());
            if(!state_->mounts[index].root)state_->Root(index);
            state_->Publish(index,2,state_->mounts[index].root,false,false,volume,
                result.listing.revision,Wide(volume->Name()),{},root->entries,root->free,true,{2},&result.listing);
            RememberImage(path);
            if(next)next();
        },
        {},[this,path,externalJournal,next]{QueueImageOpen(path,externalJournal,next);},next);
    return 1;
}

int MainMacBrowser::OpenImage(const std::wstring& path,const std::wstring& externalJournal)
{
    if(!session_.UsesAsyncNavigation())return -1;
    return QueueImageOpen(AbsoluteImagePath(path),externalJournal);
}

bool MainMacBrowser::QueueNextImage(const std::shared_ptr<const std::vector<std::wstring>>& batch,size_t index)
{
    if(index>=batch->size())return false;
    return QueueImageOpen((*batch)[index],{},[this,batch,index]{return QueueNextImage(batch,index+1);})>0;
}

void MainMacBrowser::OpenImages(std::vector<std::wstring> paths)
{
    if(paths.empty())return;
    for(auto& path:paths)path=AbsoluteImagePath(path);
    // A drop owns one sequence of pending opens. New navigation supersedes it.
    QueueNextImage(std::make_shared<const std::vector<std::wstring>>(std::move(paths)),0);
}

void MainMacBrowser::ScanRemovable(bool silent,bool startupOnly)
{
 state_->media.ScanExternal(silent,startupOnly);
}
void MainMacBrowser::AdoptRemovable(std::vector<Removable::Mounted>& found,int drive,bool select){
 if(select&&!found.empty()){
  const auto key=found.front().volume->ResourceKey();
  ReconcileRemovable(found,drive);
  for(size_t index=0;index<state_->mounts.size();++index){
   const auto& mount=state_->mounts[index];
   if(mount.volume&&mount.root&&mount.volume->ResourceKey()==key){
    state_->Show(int(index),2,mount.root);
    break;
   }
  }
  return;
 }
 for(const auto& foundVolume:found){
  const auto key=VolumeCoord::Wide(foundVolume.volume->ResourceKey());
  const auto old=std::find_if(state_->mounts.begin(),state_->mounts.end(),
      [&](const auto& mount){return mount.key==key;});
  State::Mount mount{foundVolume.volume,key,nullptr,-1};
  mount.physicalPath=foundVolume.device.path;
  mount.physicalOffset=foundVolume.partition.offset;
  mount.suffix=foundVolume.label;
  mount.sourceDrive=drive;
  int index;
  if(old==state_->mounts.end())index=session_.Mounts().AddMounted(std::move(mount));
  else{
   index=int(old-state_->mounts.begin());
   session_.Mounts().ReplaceMounted(index,std::move(mount));
  }
  state_->Root(index);
 }
}
void MainMacBrowser::ReconcileRemovable(std::vector<Removable::Mounted>& found,int drive,
                                       const std::vector<std::wstring>& failed)
{
 auto tree=session_.Tree();
 auto& control=tree->GetTreeCtrl();
 const auto enabled=tree->SelectionNotificationsEnabled();
 tree->enable_selchange(0);
 int selected=-1;uint32_t folder=0;
 state_->Selected(selected,folder);
 bool affected=false;
 for(size_t i=0;i<state_->mounts.size();++i){
  auto& mount=state_->mounts[i];
  if(!mount.volume||mount.physicalPath.empty())continue;
  auto replacement=std::find_if(found.begin(),found.end(),[&](const auto& item){
   return item.volume->ResourceKey()==mount.volume->ResourceKey();
  });
  const auto action=MediaDiscovery::ClassifyExternal(*mount.volume,mount.physicalPath,mount.sourceDrive,
      replacement==found.end()?nullptr:replacement->volume.get(),drive,failed);
  if(action==MediaDiscovery::ExternalChange::Keep)continue;
  if(action==MediaDiscovery::ExternalChange::Reuse){
   if(drive>=0)session_.Mounts().SetMountedSourceDrive(int(i),drive);
   found.erase(replacement);
   continue;
  }

  try{
   VolumeCoord::OperationLease access({{mount.volume->ResourceKey(),VolumeCoord::Mode::Write}});
   if(mount.root){tree->delete_subdir_contents(mount.root);control.DeleteItem(mount.root);}
   affected|=selected==int(i);
   auto released=session_.Mounts().RemoveMounted(int(i));
   Storage::navigationWorker().submit([released](Storage::Job&){return Storage::Result{};});
  }catch(const VolumeCoord::Busy&){
   if(replacement!=found.end())found.erase(replacement);
  }
 }
 AdoptRemovable(found,drive,false);
 if(affected){
  session_.ClearCurrent();
  session_.SetLocationTitle({});
  session_.List()->initialize_volume("",0);
  session_.List()->SetBrowserCapabilities({});
 }
 tree->enable_selchange(enabled);
}

void MainMacBrowser::ScanMedia()
{
 #ifdef HFV_TESTING
 DWORD drives=0;
 for(int drive=0;drive<26;++drive)
  if(session_.Mounts().FindWindowsDrive(drive))drives|=DWORD(1)<<drive;
 #else
 DWORD drives=GetLogicalDrives();
 if(!drives)return;
 #endif
 DWORD relevant=drives;
 for(int drive=0;drive<26;++drive)
  if(session_.Mounts().FindWindowsDrive(drive))relevant|=DWORD(1)<<drive;
 QueueMediaChanges(relevant,drives,true);
 ScanRemovable(true);
}

void MainMacBrowser::QueueMediaChanges(DWORD changed,DWORD arrived,bool rescan)
{
 state_->media.Queue(changed,arrived,rescan);
 session_.ScheduleMediaUpdates();
}

MainMacBrowser::MediaUpdate MainMacBrowser::ApplyMediaChanges()
{
 using Status=MediaDiscovery::UpdateController::Status;
 if(!session_.Tree()||!session_.List()||!session_.Document())return MediaUpdate::Idle;
 const auto status=state_->media.Apply(App().m_startup_floppies,
   [this](auto& update,auto access){PublishDriveUpdate(update,std::move(access));},
   [this](auto& media,bool silent,const auto& result){
    if(!result.code)ReconcileRemovable(media.volumes,-1,media.failedDevices);
    if(silent||!session_.Status())return;
    if(result.code)session_.Status()->SetPaneText(0,result.message.c_str());
    else if(!media.errors.empty())session_.Status()->SetPaneText(0,"Some removable disks could not be read.");
   });
 if(status==Status::Retry)return MediaUpdate::Retry;
 return status==Status::Working?MediaUpdate::Working:MediaUpdate::Idle;
}

void MainMacBrowser::PublishDriveUpdate(MediaDiscovery::DriveUpdate& update,std::shared_ptr<VolumeCoord::Lease> access)
{
  auto tree=session_.Tree();
  auto doc=session_.Document();
  const auto drive=update.drive;
  const char root[]={char('A'+drive),':','\\',0};
  char classicPath[MAX_PATH]{};
  get_floppy_volume_file_name(drive,classicPath);
  const auto classicIndex=doc->get_volume_index_by_filename(classicPath);
  auto& control=tree->GetTreeCtrl();
  auto selected=control.GetSelectedItem();
  while(selected&&control.GetParentItem(selected))selected=control.GetParentItem(selected);
  const auto enabled=tree->SelectionNotificationsEnabled();
  tree->enable_selchange(0);
  bool affected=false;
  const auto clearRoot=[&](HTREEITEM item,bool remove){
   if(!item)return;
   affected|=selected==item;
   tree->delete_subdir_contents(item);
   if(remove)control.DeleteItem(item);
  };
  int windows=-1;
  for(int index=0;index<doc->m_fat_count;++index)
   if(doc->m_fats[index].m_letter==root[0]){windows=index;break;}
  const bool present=update.Present();
  if(windows<0&&present)windows=doc->insert_fat_volume(root[0],update.type);
  if(windows>=0){
   auto& slot=doc->m_fats[windows];
   const bool replace=update.ReplaceWindows(slot.m_type);
   if(!update.rescan||replace)clearRoot(slot.m_rootitem,replace);
   const auto address=BrowserMountInventory::Address{BrowserMountInventory::Kind::WindowsDrive,windows};
   session_.Mounts().SetWindowsType(windows,present?update.type:DRIVE_NO_ROOT_DIR);
   if(replace)session_.Mounts().SetRoot(address,nullptr);
   if(present&&!slot.m_rootitem)
    session_.Mounts().SetRoot(address,tree->initialize_fat_volume(windows,&slot.m_volume_name,slot.m_type,&slot.m_volume_name,drive<2));
  }

  int floppy=classicIndex;
  if(floppy<0&&update.floppy)floppy=doc->insert_volume(classicPath,1,0,0,0);
  if(floppy>=0){
   auto& slot=doc->m_volumes[floppy];
   const auto generation=slot.m_mediaGenerationKnown?std::optional<DWORD>(slot.m_mediaGeneration):std::nullopt;
   if(update.RefreshFloppy(slot.m_opened,generation))clearRoot(slot.m_rootitem,!update.floppy);
   if(update.floppy){
    session_.Mounts().UpdateClassic(floppy,*update.floppy);
    const auto address=BrowserMountInventory::Address{BrowserMountInventory::Kind::ClassicVolume,floppy};
    if(!slot.m_rootitem)session_.Mounts().SetRoot(address,tree->initialize_volume(floppy,slot.m_volume_name,2,1,0,0,0));
    else{
     tree->RenewBrowserMount(slot.m_rootitem);
     control.SetItemText(slot.m_rootitem,slot.m_volume_name);
    }
   }else session_.Mounts().RemoveClassic(floppy);
  }

  std::vector<std::shared_ptr<Volume>> released;
  for(size_t index=0;index<state_->mounts.size();++index){
   const auto& mount=state_->mounts[index];
   if(!update.RemoveOptical(mount.drive,mount.volume))continue;
   clearRoot(mount.root,true);
   released.push_back(session_.Mounts().RemoveMounted(int(index)));
  }
  if(update.cd&&!HasCdRoot(drive))AdoptCd(drive,root,std::move(update.cd),false);
  if(update.UpdateExternal())ReconcileRemovable(update.removable,drive);
  if(affected){
   control.SelectItem(windows>=0?doc->m_fats[windows].m_rootitem:nullptr);
   session_.ClearCurrent();
   session_.SetLocationTitle({});
   session_.List()->initialize_volume("",0);
   session_.List()->SetBrowserCapabilities({});
  }
  tree->enable_selchange(enabled);
  if(!released.empty()){
   Storage::navigationWorker().submit([released=std::move(released),access=std::move(access)](Storage::Job&){
    for(const auto& volume:released)volume->ReleaseMedia();
    return Storage::Result{};
   });
  }
}

bool MainMacBrowser::MediaUpdatesPending() const
{
 return state_->media.Pending();
}

#ifdef HFV_TESTING
void MainMacBrowser::SetDriveTypeForTest(std::function<UINT(int)> probe){state_->media.testDriveType=std::move(probe);}
#endif

bool MainMacBrowser::HasCdRoot(int drive)const{
 return std::any_of(state_->mounts.begin(),state_->mounts.end(),
    [&](const auto& mount){return mount.drive==drive&&mount.root;});
}
void MainMacBrowser::ScanStartupDrives(bool floppies,bool cds)
{
#ifdef HFV_TESTING
    if(!state_->media.testDriveType)return;
#endif
    DWORD mask=0;
    const auto doc=session_.Document();
    for(int index=0;index<doc->m_fat_count;++index){
        const auto& entry=doc->m_fats[index];
        const int drive=toupper(static_cast<unsigned char>(entry.m_letter))-'A';
        if(drive<0||drive>=26)continue;
        if((floppies&&entry.m_type==DRIVE_REMOVABLE)||(cds&&entry.m_type==DRIVE_CDROM))
            mask|=DWORD(1)<<drive;
    }
    if(mask)QueueMediaChanges(mask,mask,true);
}

void MainMacBrowser::AdoptCd(int drive,const std::string& root,std::shared_ptr<Volume> volume,bool select){
 const auto existing=std::find_if(state_->mounts.begin(),state_->mounts.end(),
     [drive](const auto& mount){return mount.drive==drive&&mount.volume&&mount.root;});
 int index=int(existing-state_->mounts.begin());
 if(existing==state_->mounts.end()){
  index=session_.Mounts().AddMounted({std::move(volume),FromAnsi(root.c_str()),nullptr,drive});
  state_->Root(index);
 }
 if(!select)return;
 auto item=state_->mounts[index].root;
 state_->Show(index,2,item);
}
bool MainMacBrowser::Select(LPARAM data){auto p=session_.Tree()->BrowserNode(data);if(!p||p->LegacyType()!=LIST_MAC)return false;
    state_->Show(p->mount.index,uint32_t(p->CatalogId()),session_.Tree()->m_ct->GetSelectedItem(),false);return true;}
bool MainMacBrowser::Refresh()
{
    int index;
    uint32_t id;
    if(!state_->Selected(index,id))return false;
    const auto& mount=state_->mounts[index];
    ForgetFolders(mount.volume);
    const bool rebuild=mount.drive<0&&mount.physicalPath.empty();
    return state_->Show(index,id,session_.Tree()->m_ct->GetSelectedItem(),rebuild,true);
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
    auto released=DetachCd(drive);
    Run(session_,[&](Storage::Job&){for(auto& volume:released)volume->ReleaseMedia();released.clear();});
}

std::vector<std::shared_ptr<Volume>> MainMacBrowser::DetachCd(int drive){
    if(!session_.Tree()||!session_.List())return {};
    bool selected=SelectedCdDrive()==drive;std::vector<std::shared_ptr<Volume>> released;
    auto tree=session_.Tree();bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
    for(size_t index=0;index<state_->mounts.size();++index){
        const auto& mount=state_->mounts[index];
        if(mount.drive!=drive||!mount.volume)continue;
        if(mount.root)tree->m_ct->DeleteItem(mount.root);
        released.push_back(session_.Mounts().RemoveMounted(int(index)));
    }
    tree->enable_selchange(enabled);if(selected){session_.ClearCurrent();session_.SetLocationTitle({});session_.List()->initialize_volume("",0);}
    return released;
}

void MainMacBrowser::DetachTree(){state_->restore.clear();int i;uint32_t id;if(state_->Selected(i,id)){
    state_->current=i;for(auto item=session_.Tree()->m_ct->GetSelectedItem();item;item=session_.Tree()->m_ct->GetParentItem(item)){
        auto p=session_.Tree()->BrowserNodeAt(item);if(p&&p->LegacyType()==LIST_MAC)state_->restore.push_back(uint32_t(p->CatalogId()));}}
    session_.Mounts().ClearMountedRoots();}
bool MainMacBrowser::RebuildTree()
{
    for(size_t i=0;i<state_->mounts.size();++i)state_->Root(int(i));
    auto route=std::move(state_->restore);
    const auto index=state_->current;
    if(route.empty()||index<0||size_t(index)>=state_->mounts.size())return false;
    const auto& mount=state_->mounts[index];
    if(!mount.root)return false;
    BrowserNavigation::Request request{};
    request.folder=Location::Mounted(mount.volume,route.front());
    request.folder.directory=true;
    request.intent=BrowserNavigation::Intent::RefreshFolder;
    request.resource=mount.volume->ResourceKey();
    request.catalogRoute.assign(route.rbegin(),route.rend());
    return state_->Show(index,request.folder.Id(),mount.root,true,false,&request);
}
void MainMacBrowser::Clear(){session_.CancelNavigation();state_->media.Cancel();session_.Mounts().ClearMounted();state_->current=-1;}
bool MainMacBrowser::Activate(int row){auto model=session_.List()->BrowserRowAt(row);
    if(!model||!model->location.IsMounted())return false;if(!session_.ListingAvailable())return true;if(model->entry.directory){
     auto item=model->treeItem;if(!item)return true;
     auto tree=session_.Tree();
     const auto enabled=tree->SelectionNotificationsEnabled();
     tree->enable_selchange(0);tree->m_ct->SelectItem(item);tree->enable_selchange(enabled);
     Select(tree->m_ct->GetItemData(item));
    }
    else{auto& list=session_.List()->GetListCtrl();list.SetItemState(-1,0,LVIS_SELECTED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);Export(0);}return true;}
bool MainMacBrowser::CreateFolder(const FileOperations::Location& target,const std::u16string& name){
 FileOperations::CreateFolder request{target,name};return RunOperation(session_,std::move(request),false,true);
}
bool MainMacBrowser::CanPasteHardLink()const{
 if(!Writable())return false;int index;uint32_t folder;if(!state_->Selected(index,folder))return false;auto target=state_->mounts[index].volume;auto selection=BrowserTransfers::Snapshot();return target->SupportsHardLinks()&&selection.items.size()==1&&selection.items[0].Volume()&&selection.items[0].Volume()->SupportsHardLinks()&&selection.items[0].Volume()->ResourceKey()==target->ResourceKey();
}
bool MainMacBrowser::PasteHardLink(){
 if(!CanPasteHardLink())return true;auto selection=BrowserTransfers::Snapshot();auto target=state_->Target();std::wstring name=Wide(selection.items[0].expectedName)+L" link";
 if(!EditHfsPlusName(session_.Owner(),name,target.Volume()->CaseSensitive()))return true;
 FileOperations::CreateHardLink request{selection.items[0],target,U16(name)};request.source.cut=false;RunOperation(session_,std::move(request));if(!BackgroundCopies::Enabled())Refresh();return true;
}
bool MainMacBrowser::NewFolder(){if(!Active())return false;if(!CanCreateFolder())return true;
 auto target=state_->Target();std::wstring name=L"New Folder";
 if(EditHfsPlusName(session_.Owner(),name,target.Volume()->CaseSensitive())){CreateFolder(target,U16(name));if(!BackgroundCopies::Enabled())Refresh();}return true;}
FileOperations::Location MainMacBrowser::CaptureItem(int row)
{
 return state_->Item(row,false);
}
bool MainMacBrowser::RenameItem(int row,const std::u16string& name,bool checkLock){
 auto item=CaptureItem(row);FileOperations::RenameEntry request{item, name, {}, checkLock};
 bool result=RunOperation(session_,std::move(request),false,true);if(!BackgroundCopies::Enabled()&&Active())Refresh();return result;
}
bool MainMacBrowser::RenameWindowsItem(int row){
 const auto* selected=session_.List()->BrowserRowAt(row);
 if(!selected||!selected->location.IsWindows())return false;
 try{
  auto name=Wide(selected->entry.name);
  auto item=CaptureItem(row);
  if(EditWindowsName(session_.Owner(),name))
   RunOperation(session_,FileOperations::RenameEntry{std::move(item),U16(name),{},false},false,true);
 }catch(const std::exception& error){MessageBoxA(session_.Owner()->GetSafeHwnd(),error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);}
 return true;
}
bool MainMacBrowser::Rename(){if(!Active())return false;if(!SelectionWritable())return true;
 auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return true;
 auto item=state_->Item(row,false);auto name=Wide(item.expectedName);
 if(EditHfsPlusName(session_.Owner(),name,item.Volume()->CaseSensitive())){FileOperations::RenameEntry request{item, U16(name)};RunOperation(session_,std::move(request),false,true);if(!BackgroundCopies::Enabled())Refresh();}return true;
}
bool MainMacBrowser::Delete(bool confirm){if(!Active())return false;if(!SelectionWritable())return true;
    std::vector<Location> selected;auto& list=session_.List()->GetListCtrl();for(int i=-1;(i=list.GetNextItem(i,LVNI_SELECTED))>=0;){const auto row=session_.List()->BrowserRowAt(i);if(row&&row->location.IsMounted())selected.push_back(state_->Item(i,false));}
    if(selected.empty()||(confirm&&session_.Owner()->MessageBox("Delete the selected files and folders?","HFVExplorer",MB_YESNO|MB_ICONQUESTION)!=IDYES))return true;
    FileOperations::DeleteEntries request{std::move(selected)};RunOperation(session_,std::move(request),false,true);if(!BackgroundCopies::Enabled())Refresh();return true;}
bool MainMacBrowser::CanInspectProperties(int index) const
{
    if (index < 0 || !session_.ListingAvailable()) return false;
    const auto row = session_.List()->BrowserRowAt(index);
    return row && !row->location.IsNone() && !row->location.IsWindows();
}

bool MainMacBrowser::Properties(int index)
{
    if (!CanInspectProperties(index)) return false;
    return MetadataDialogs::Show(session_.List()->BrowserRowAt(index)->location, session_.Owner());
}
BrowserTransfers::Selection MainMacBrowser::SelectedItems(bool cut){
 BrowserTransfers::Selection out;if(!session_.ListingAvailable())return out;out.canMove=session_.List()->BrowserSelectionAllows(false);
 if(cut&&!out.canMove)return out;
 auto& list=session_.List()->GetListCtrl();
 for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){if(session_.List()->BrowserRowAt(row))out.items.push_back(state_->Item(row,cut));}
 return out;
}
FileOperations::Location MainMacBrowser::TargetFolder(HTREEITEM item){return state_->Target(item);}
BrowserDirectory::Folder MainMacBrowser::DescribeFolder(HTREEITEM item){return state_->Folder(item);}
bool MainMacBrowser::TransferSelection(const std::vector<Location>& sources,const Location& target,std::function<void(bool)> completed){
 try{return !sources.empty()&&state_->Transfer(sources,target,std::move(completed));}catch(const std::exception& e){
#ifdef HFV_TESTING
 browserTransferError=e.what();
#endif
 if(!Hidden(session_))session_.Owner()->MessageBox(e.what(),"HFVExplorer",MB_OK|MB_ICONERROR);return false;}
}
bool MainMacBrowser::Capture(bool cut,bool append){BrowserTransfers::Publish(SelectedItems(cut),append);return true;}
bool MainMacBrowser::Paste(){try{return BrowserTransfers::Paste(session_);}catch(const std::exception& e){
#ifdef HFV_TESTING
 browserTransferError=e.what();
#endif
 if(!Hidden(session_))session_.Owner()->MessageBox(e.what(),"HFVExplorer",MB_OK|MB_ICONERROR);return true;}}
bool MainMacBrowser::Command(UINT id){if(!Active())return false;
    if(id>=ID_OWN_WITH_MAC_FIRST&&id<ID_OWN_FILE_PROPERTIES)return true; // Classic emulator/file-type actions do not apply.
    if(id>=ID_OWN_MAP_THIS&&id<=ID_OWN_SHORTCUT_WINDOWS)return true;return false;}
bool MainMacBrowser::OpenMountedFolderFrom(const MainMacBrowser& source,const BrowserNavigation::Request& request)
{
 const auto& volume=request.folder.Volume();
 auto existing=std::find_if(state_->mounts.begin(),state_->mounts.end(),[&](const State::Mount& mount){return mount.volume==volume;});
 if(existing==state_->mounts.end()){
  auto original=std::find_if(source.state_->mounts.begin(),source.state_->mounts.end(),[&](const State::Mount& mount){return mount.volume==volume;});
  if(original==source.state_->mounts.end())return false;
  auto mount=*original;
  mount.root=nullptr;
  const auto index=session_.Mounts().AddMounted(std::move(mount));
  state_->Root(index);
  return state_->Show(index,request.folder.Id(),state_->mounts[index].root,true,false,&request);
 }
 const auto index=int(existing-state_->mounts.begin());
 return state_->Show(index,request.folder.Id(),existing->root,true,false,&request);
}

bool MainMacBrowser::RevealFindHit(const BrowserNavigation::Request& request)
{
    const auto& volume=request.folder.Volume();
    for(size_t index=0;index<state_->mounts.size();++index){
        const auto& mount=state_->mounts[index];
        if(!mount.root||mount.volume!=volume)continue;
        auto anchor=session_.Tree()->GetTreeCtrl().GetSelectedItem();
        return state_->Show(int(index),request.folder.Id(),anchor,true,false,&request);
    }
    return false;
}
void MainMacBrowser::Import(){if(!Active()||!Writable())return;std::wstring path;if(!FileDialog(session_,path,false))return;
    Location source=Location::Windows(path);try{TransferSelection({source},state_->Target());}catch(const std::exception& e){AfxMessageBox(e.what());}}
void MainMacBrowser::ImportResource(){
    if(!Active()||!Writable())return;auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return;
    const auto model=session_.List()->BrowserRowAt(row);if(!model||!model->location.IsMounted()||model->entry.directory)return;
    auto selected=state_->Item(row,false);
    std::wstring path;if(!FileDialog(session_,path,false))return;Location input;input.SetWindows(path);
    FileOperations::ImportResourceFork request{input,selected};
    RunOperation(session_,std::move(request),true,true);if(!BackgroundCopies::Enabled())Refresh();
}
void MainMacBrowser::Export(unsigned fork){if(!Active())return;auto& list=session_.List()->GetListCtrl();int row=list.GetNextItem(-1,LVNI_SELECTED);if(row<0)return;
 auto item=state_->Item(row,false);if(item.directory)return;auto path=Wide(item.expectedName)+(fork?L".rsrc":L"");if(!FileDialog(session_,path,true))return;path=BrowserPresentation::WindowsPath(path);
 FileOperations::ExportFileFork request{item,Location::Windows(path),fork ? FileOperations::ForkSelector::Resource : FileOperations::ForkSelector::Data};RunOperation(session_,std::move(request),true,true);}

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
    menu.AppendMenu(MF_STRING, ID_OWN_FILE_PROPERTIES, "P&roperties...");
    session_.List()->UpdateBrowserItemMenu(menu);
    const UINT command = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, session_.Owner());
    if (command) session_.Owner()->SendMessage(WM_COMMAND, command);
    return true;
}

#ifdef HFV_TESTING
size_t MainMacBrowser::TestClipboardCount()const{return BrowserTransfers::Snapshot().items.size();}
int MainMacBrowser::AttachTestVolume(std::shared_ptr<Volume> volume,const char* key,int drive){const int i=session_.Mounts().AddMounted({volume,FromAnsi(key),nullptr,drive});state_->Root(i);return i;}
bool MainMacBrowser::ShowTestFolder(int index,uint32_t id)
{
 const auto& mount=state_->mounts.at(index);
 if(!state_->Show(index,id,mount.root))return false;
 return !session_.Navigating()||session_.AwaitNavigationForTest();
}
#endif
