// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "StorageHandoff.h"
#include "MetadataDialogs.h"
#include "PhysicalRemovable.h"
#include "HFVExplorer.h"
#include "MainMacBrowser.h"
#include "FileOperations.h"
#include "BackgroundCopies.h"
#include "VolumeCoordinator.h"
#include "BrowserSession.h"
#include "StorageLocation.h"
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
bool readBusy=false;
bool Run(BrowserSession& session_,std::function<void(Storage::Job&)> work,bool copy=false,bool single=false) {
    auto body=[&](Storage::Job& job){try{work(job);return Storage::Result{};}catch(const VolumeCoord::Busy& e){return Storage::Result{1,ERROR_BUSY,e.what()};}};
    auto result=copy?ShowStorageJob(body,"Copying files",Hidden(session_),single,true,session_.Owner()):ShowStorageWaitJob(body,Hidden(session_),session_.Owner());
    readBusy=result.error==ERROR_BUSY;
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
    explicit State(BrowserSession& session):session_(session){}
    struct Mount {std::shared_ptr<Volume> volume;std::wstring key;HTREEITEM root=nullptr;int drive=-1;std::wstring physicalPath,suffix;uint64_t physicalOffset=0;};
    std::vector<Mount> mounts;bool removableScanned=false;

    int current=-1;uint32_t folder=2;bool showing=false,folderMutable=true;
    std::vector<uint32_t> restore;
    bool Selected(int& mount,uint32_t& id,HTREEITEM item=nullptr)const {
        if(!session_.Tree()||!session_.List()||!IsWindow(session_.Tree()->GetSafeHwnd())||!IsWindow(session_.List()->GetSafeHwnd()))return false;if(!item)item=session_.Tree()->m_ct->GetSelectedItem();if(!item)return false;
        auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(item));
        if(!p||p->type!=LIST_MAC)return false;mount=p->volinx;id=uint32_t(p->id);
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
        auto binding=reinterpret_cast<lparam_struct*>(tree->GetTreeCtrl().GetItemData(item));
        if(!binding)throw HfsPlus::Error("Select a destination folder");
        BrowserDirectory::Folder folder;
        auto& out=folder.location;
        out.directory=true;
        if(binding->type==LIST_MAC){
            auto volume=mounts.at(binding->volinx).volume;
            if(!volume)throw HfsPlus::Error("The selected volume was closed");
            out.SetMounted(volume,uint32_t(binding->id));
            folder.writable=volume->Supports().create;
        }else if(binding->type==LIST_FAT){
            out.SetWindows(FromAnsi(binding->path));
            folder.writable=session_.Document()->m_fats[binding->volinx].m_type!=DRIVE_CDROM;
        }else if(binding->type==LIST_HFS){
            int mac=0;
            auto& volume=session_.Document()->m_volumes[binding->volinx];
            auto path=binding->id==2?std::string(":"):std::string((LPCSTR)session_.Document()->get_directory(item,&mac));
            out.SetClassic(std::string((LPCSTR)volume.m_file_name),std::move(path));
            out.expectedCatalogId=uint32_t(binding->id);
            out.expectedResource=VolumeCoord::ImageKey(out.ClassicImage());
            if(volume.m_mediaGenerationKnown)out.BindMediaGeneration(volume.m_mediaGeneration);
            folder.writable=!volume.m_is_cd;
        }else throw HfsPlus::Error("This item is not a destination folder");
        return folder;
    }
    Location Target(HTREEITEM item=nullptr) { return Folder(item).location; }

    void BindRow(BrowserDirectory::Row& row,const std::string& resourceKey) {
        auto& out=row.location;
        out.directory=row.entry.directory;
        out.expectedIdentity=row.entry.identity;
        if(row.storageType==LIST_MAC){
            auto& mount=mounts.at(row.volume);
            out.SetMounted(mount.volume,row.id);
            row.mediaDrive=mount.drive;
            row.accessKey=mount.volume->ResourceKey();
            out.expectedName=row.entry.name;
            out.expectedParent=row.entry.parent;
        }else if(row.storageType==LIST_FAT&&(out.IsWindows()||!row.path.empty())){
            if(!out.IsWindows())out.SetWindows(FromAnsi(row.path));
            if(out.WindowsPath().size()>=2 && out.WindowsPath()[1]==L':') {
                const auto drive=towupper(out.WindowsPath()[0]);
                if(drive>=L'A'&&drive<=L'Z')row.mediaDrive=drive-L'A';
            }
            if(row.accessKey.empty())row.accessKey=resourceKey;
        }else if(row.storageType==LIST_HFS&&!row.path.empty()){
            auto& volume=session_.Document()->m_volumes[row.volume];
            out.SetClassic(std::string((LPCSTR)volume.m_file_name),row.path);
            out.expectedCatalogId=row.id;
            out.expectedResource=resourceKey.empty()?VolumeCoord::ImageKey(out.ClassicImage()):resourceKey;
            row.accessKey=out.expectedResource;
            if(out.IsPhysical())row.mediaDrive=out.Drive();
            if(volume.m_mediaGenerationKnown)out.BindMediaGeneration(volume.m_mediaGeneration);
        }
    }

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
        ins.item.lParam=tree->make_lparam(LIST_MAC,index,long(id),nullptr);
        return reinterpret_cast<HTREEITEM>(SendMessageW(tree->m_ct->m_hWnd,TVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&ins)));
    }
    void Root(int index) {
        auto& m=mounts[index];if(!m.volume)return;auto label=Wide(m.volume->Name())+L" ("+FromAnsi(m.volume->Description())+L")";
        if(!m.suffix.empty())label+=L" - "+m.suffix;
        if(m.drive>=0)label+=std::wstring(L" - ")+wchar_t('A'+m.drive)+L":";
        if(!m.volume->Writable())label+=L" [read-only]";
        m.root=Insert(index,2,nullptr,label,true);
    }
    bool UseAsyncNavigation() const {
        return session_.UsesAsyncNavigation();
    }
    bool Show(int index,uint32_t id,HTREEITEM item,bool rebuild=false,bool asynchronous=false,bool reload=false,const BrowserNavigation::Request* find=nullptr) {
        session_.CancelNavigation();
        if(showing)return true;
        auto title=Wide(mounts.at(index).volume->Name());if(id!=2&&item){wchar_t leaf[1024]{};TVITEMW value{};value.mask=TVIF_TEXT;value.hItem=item;value.pszText=leaf;value.cchTextMax=ARRAYSIZE(leaf);if(SendMessageW(session_.Tree()->m_ct->m_hWnd,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&value)))title+=L":"+std::wstring(leaf);}
        if(!asynchronous)session_.SetLocationTitle(std::move(title));
        auto volume=mounts.at(index).volume;
        const auto key=volume->ResourceKey();
        uint64_t revision=0;std::wstring folderTitle;std::vector<std::pair<uint32_t,std::vector<Entry>>> levels;std::vector<Entry> entries;uint64_t free=0;bool mutableFolder=true;
        std::vector<uint32_t> ancestors;std::vector<std::pair<uint32_t,std::u16string>> savedPath;
        for(auto a=item;a;a=session_.Tree()->m_ct->GetParentItem(a)){auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(a));if(!p)return false;ancestors.push_back(uint32_t(p->id));wchar_t text[1024]{};TVITEMW value{};value.mask=TVIF_TEXT;value.hItem=a;value.pszText=text;value.cchTextMax=ARRAYSIZE(text);SendMessageW(session_.Tree()->m_ct->m_hWnd,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&value));savedPath.push_back({uint32_t(p->id),U16(text)});}
        if(find){
            savedPath.clear();
            for(auto node=find->catalogRoute.rbegin();node!=find->catalogRoute.rend();++node)
                savedPath.push_back({*node,{}});
        }
        const bool canCache=volume->HasPersistentWorker();
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
        auto readFolder = [rebuild,savedPath,strict=bool(find),selectedFolder](const std::shared_ptr<Volume>& volume,
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
        if(asynchronous && !cached){
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
            const auto mountCurrent=[this,index,volume,mountRoot]{
                return index>=0&&size_t(index)<mounts.size()&&mounts[index].root==mountRoot&&mounts[index].volume==volume;
            };
            const auto retry=[this,index,id,item,rebuild,reload,found=find?std::optional<Request>(*find):std::nullopt]{
                Show(index,id,item,rebuild,true,reload,found?&*found:nullptr);
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
                    result.folder.SetMounted(volume,id);
                    FillListing(snapshot,listing->entries,index,volume->Supports(),showInvisible,listing->mutableFolder);
                }catch(const VolumeCoord::Busy&){return Storage::Result{1,ERROR_BUSY,"This volume is busy"};}
                return Storage::Result{};
            }, [this,volume,listing,item,index,mountRoot,rebuild](Result& result){
                auto target=item;
                auto mounted=result.folder.Volume();
                if(mounted!=volume){
                    ForgetFolders(volume);
                    mounts[index].volume=mounted;
                    target=mountRoot;
                    if(!mounts[index].physicalPath.empty())
                        TreeText(session_,target,Wide(mounted->Name())+L" - "+mounts[index].suffix+L" [read-only]");
                }
                Publish(index,result.folder.Id(),target,rebuild,false,mounted,result.listing.revision,listing->title,
                    listing->levels,listing->entries,listing->free,listing->mutableFolder,listing->ancestors,&result.listing);
            },mountCurrent,retry);
            return true;
        }
        if(!cached && session_.Defer(key,mounts.at(index).drive>=0,[this,index,id,item,rebuild]{Show(index,id,item,rebuild);}))return true;
        session_.ClearCurrent();showing=true;struct Done{bool& flag;~Done(){flag=false;}}done{showing};
        if(!cached && !Run(session_,[&](Storage::Job&){
            VolumeCoord::Lease read({{key,VolumeCoord::Mode::Read}});
            readFolder(volume,id,revision,ancestors,folderTitle,levels,entries,free,mutableFolder);
        })){
            if(readBusy){
                cached=restoreCached();
                if(!cached){session_.Defer(key,mounts.at(index).drive>=0,[this,index,id,item,rebuild]{Show(index,id,item,rebuild);},true);return true;}
            }
            if(!cached){
                auto tree=session_.Tree();bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);
                auto root=mounts[index].root;for(auto child=tree->m_ct->GetChildItem(root);child;){auto next=tree->m_ct->GetNextSiblingItem(child);tree->m_ct->DeleteItem(child);child=next;}
                tree->m_ct->SelectItem(root);tree->enable_selchange(enabled);
                session_.List()->initialize_volume("",0);session_.List()->SetBrowserCapabilities({});current=index;folder=2;folderMutable=false;return false;
            }
        }
        return Publish(index,id,item,rebuild,cached,volume,revision,folderTitle,levels,entries,free,mutableFolder,ancestors);
    }
    static void FillListing(BrowserDirectory::Snapshot& snapshot,const std::vector<Entry>& entries,
        int index,const MacBrowser::Capabilities& capabilities,bool showInvisible,bool mutableFolder)
    {
        snapshot.capabilities=capabilities;
        snapshot.capabilitiesKnown=true;
        if(!mutableFolder)
            snapshot.capabilities.create=snapshot.capabilities.rename=snapshot.capabilities.remove=false;
        snapshot.rows.reserve(entries.size());
        for(const auto& entry:entries)
            if(showInvisible||!((uint16_t(entry.finder[8])<<8|entry.finder[9])&0x4000))
                snapshot.rows.push_back(BrowserDirectory::FromMac(entry,index,capabilities));
    }
    bool Publish(int index,uint32_t id,HTREEITEM item,bool rebuild,bool cached,
        const std::shared_ptr<Volume>& volume,uint64_t revision,const std::wstring& folderTitle,
        const std::vector<std::pair<uint32_t,std::vector<Entry>>>& levels,std::vector<Entry>& entries,
        uint64_t free,bool mutableFolder,const std::vector<uint32_t>& ancestors,
        const BrowserDirectory::Snapshot* prepared=nullptr)
    {
        const auto key=volume->ResourceKey();
        const bool canCache=volume->HasPersistentWorker();
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
        auto list = session_.List();
        list->list_clear();
        list->destroy_pointers();
        list->initialize_volume("", id == 2 ? 0 : ID_GOTO_PARENT);
        BrowserDirectory::Snapshot snapshot;
        if(!prepared){
            snapshot.resourceKey=key;
            snapshot.revision=revision;
            FillListing(snapshot,entries,index,volume->Supports(),App().m_show_invisible_mac!=FALSE,!cached&&mutableFolder);
            prepared=&snapshot;
        }
        auto location=Location::Mounted(volume,id);
        location.directory=true;
        tree->BindBrowserFolder(item,{std::move(location),prepared->capabilities.create});
        session_.PresentDirectory(*prepared, LIST_MAC, index, id, {}, true,
            [&](const BrowserDirectory::Row& row) {
                return Insert(index, row.id, item, Wide(row.entry.name));
            });
        session_.SetLocationTitle(id==2?Wide(volume->Name()):Wide(volume->Name())+L":"+folderTitle);
        current=index;folder=id;folderMutable=!cached&&mutableFolder;
        control.SelectItem(item);control.EnsureVisible(item);tree->enable_selchange(enabled);
        if(session_.Status()){CString status;status.Format("%s%s - %I64u bytes free",volume->Kind(),volume->Writable()?"":" (read-only)",free);
            session_.Status()->SetPaneText(0,status);}
        if(cached)session_.Defer(key,false,[this,index,id]{
            int selected;uint32_t selectedId;
            if(Selected(selected,selectedId) && selected==index && selectedId==id){
                StorageQuietRefresh quiet;
                Show(index,id,session_.Tree()->m_ct->GetSelectedItem(),true,UseAsyncNavigation());
            }
        },true,true);
        return true;
    }
    bool Transfer(const std::vector<Location>& sources,const Location& target,std::function<void(bool)> completed={}) {
        if(target.IsClassic()){auto doc=session_.Document();auto path=target.ClassicImage();for(int n=0;n<doc->m_hfs_count;++n)if(!doc->m_volumes[n].m_file_name.IsEmpty()&&!_stricmp(Location::Classic(std::string((LPCSTR)doc->m_volumes[n].m_file_name),":").ClassicImage().c_str(),path.c_str()))doc->m_volumes[n].close();}
        auto convertible=[](const Location& p){if(p.LegacyType()!=LIST_HFS)return true;auto n=p.ClassicImage().size();return n>=4&&p.ClassicImage()[n-4]=='.'&&(!_stricmp(p.ClassicImage().c_str()+n-3,"DSK")||!_strnicmp(p.ClassicImage().c_str()+n-3,"HF",2));};
        auto classicImage=[](const Location& item){
            return item.IsMounted()&&!strcmp(item.Volume()->Kind(),"HFS")&&
                (item.Volume()->HasPersistentWorker()||item.Volume()->CdDrive()>=0);
        };
        auto legacyType=[&](const Location& item){return classicImage(item)?LIST_HFS:item.LegacyType();};
        auto conversionEndpoint=[&](const Location& item){
            if(!classicImage(item))return item;
            if(const int drive=item.Volume()->CdDrive();drive>=0)
                return Location::Physical(drive,FileOperations::ClassicMedia::Cd,":");
            auto path=item.Volume()->WorkerDescription().path;
            return Location::Classic((LPCSTR)CString(path.c_str()),":");
        };
        auto canConvert=[&](const Location& item){return classicImage(item)||(!item.Volume()&&convertible(item));};
        bool legacy=canConvert(target)&&std::all_of(sources.begin(),sources.end(),canConvert)&&std::any_of(sources.begin(),sources.end(),[&](const Location& p){return legacyType(p)!=legacyType(target);});
        bool complete=true;std::vector<FileOperations::Request> background;
        if(legacy){
         for(auto& source:sources){
          FileOperations::Request request = FileOperations::OrdinaryCopy{{source}, target};
          if (legacyType(source) != legacyType(target)) {
              const bool ask = (legacyType(source)==LIST_HFS ? App().m_copyout_mode : App().m_copyin_mode) == 6;
              request = FileOperations::LegacyConversion{
                  conversionEndpoint(source), conversionEndpoint(target), source.directory, source.cut, ask,
                  CaptureLegacyCopyOptions(HFSIFACE_copy_mode())};
              request.Legacy().selection = FileOperations::ConversionSelection{source, target};
          }
          if(BackgroundCopies::Enabled())background.push_back(std::move(request));else if(!RunOperation(session_,std::move(request),true,!source.directory)){complete=false;break;}
         }
        }else{
         FileOperations::Request request = FileOperations::OrdinaryCopy{sources, target};
         if(BackgroundCopies::Enabled())background.push_back(std::move(request));else complete=RunOperation(session_,std::move(request),true,sources.size()==1&&!sources[0].directory);
        }
        if(BackgroundCopies::Enabled()){if(!complete)return false;return BackgroundCopies::Start(std::move(background),session_.Owner(),sources.size()==1&&!sources[0].directory,std::move(completed));}
        if(completed)completed(complete);
        if(session_.Tree()&&session_.List()){
            if(target.LegacyType()==LIST_HFS)session_.Document()->OnOpenDocument("");
            else if(target.LegacyType()==LIST_MAC)session_.MacBrowser().Refresh();else session_.List()->update_current_view();
        }
        return complete;
    }
};
MainMacBrowser::MainMacBrowser(BrowserSession& session):session_(session),state_(new State(session)){}
MainMacBrowser::~MainMacBrowser()=default;
void MainMacBrowser::ForgetTree(){state_->restore.clear();for(auto& mount:state_->mounts)mount.root=nullptr;}
std::string MainMacBrowser::SelectedResourceKey()const{int index;uint32_t id;if(state_->Selected(index,id))return state_->mounts[index].volume->ResourceKey();auto tree=session_.Tree();auto doc=session_.Document();if(!tree||!doc)return {};auto item=tree->m_ct->GetSelectedItem();auto p=item?reinterpret_cast<lparam_struct*>(tree->m_ct->GetItemData(item)):nullptr;if(!p)return {};if(p->type==LIST_HFS&&p->volinx>=0&&p->volinx<doc->m_hfs_count)return doc->m_volumes[p->volinx].m_file_name.IsEmpty()?std::string():VolumeCoord::ImageKey(std::string((LPCSTR)doc->m_volumes[p->volinx].m_file_name));if(p->type==LIST_FAT){const auto* folder=tree->BrowserFolderAt(item);return session_.ObservedWindowsKey(folder&&folder->location.IsWindows()?folder->location.WindowsPath():VolumeCoord::Wide(p->path));}return {};}
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
    if(WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,ansi.data(),size,nullptr,&lost)&&!lost)
        App().AddToRecentFileList(ansi.c_str());
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
                root->entries=volume->List(2);
                root->free=volume->FreeBytes();
                result.folder=Location::Mounted(volume);
                StorageLocation::ValidateMedia(result.folder);
                StorageLocation::ValidateImage(source.path,key);
                result.folder.directory=true;
                State::FillListing(result.listing,root->entries,-1,volume->Supports(),showInvisible,true);
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
            const auto volume=result.folder.Volume();
            auto match=std::find_if(state_->mounts.begin(),state_->mounts.end(),[&](const auto& mount){
                return mount.volume&&mount.volume->ResourceKey()==volume->ResourceKey();
            });
            int index;
            if(match==state_->mounts.end()){
                index=int(state_->mounts.size());
                state_->mounts.push_back({volume,path,nullptr,-1});
            }else index=int(match-state_->mounts.begin());
            if(!state_->mounts[index].root)state_->Root(index);
            for(auto& row:result.listing.rows)row.volume=index;
            state_->Publish(index,2,state_->mounts[index].root,false,false,volume,
                result.listing.revision,Wide(volume->Name()),{},root->entries,root->free,true,{2},&result.listing);
            RememberImage(path);
            if(next)next();
        },
        []{return true;},[this,path,externalJournal,next]{QueueImageOpen(path,externalJournal,next);},next);
    return 1;
}

int MainMacBrowser::OpenImage(const std::wstring& path,const std::wstring& externalJournal) {
    const auto wide=AbsoluteImagePath(path);
    if(session_.UsesAsyncNavigation())return QueueImageOpen(wide,externalJournal);
#ifndef HFV_TESTING
    return -1;
#else
    for(size_t i=0;i<state_->mounts.size();++i)if(!_wcsicmp(wide.c_str(),state_->mounts[i].key.c_str())){
        if(!state_->mounts[i].root)state_->Root(int(i));state_->Show(int(i),2,state_->mounts[i].root);return 1;}
    auto key=VolumeCoord::ImageKey(wide);if(session_.Defer(key,false,[this,wide,externalJournal]{OpenImage(wide,externalJournal);}))return 1;
    std::shared_ptr<Volume> volume;std::string journalPrompt;bool ok=Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{key,VolumeCoord::Mode::Read}});try{volume=MacBrowser::OpenBrowserImage(wide,externalJournal);}catch(const HfsPlus::ExternalJournalRequired& e){journalPrompt=e.what();}});
    if(ok&&!journalPrompt.empty()){
        if(Hidden(session_))return -1;
        std::wstring external;if(!SelectHfsPlusExternalJournal(session_.Owner(),external,journalPrompt))return -1;
        ok=Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{key,VolumeCoord::Mode::Read},{VolumeCoord::ImageKey(external),VolumeCoord::Mode::Read}});volume=MacBrowser::OpenBrowserImage(wide,external);});
    }
    if(!ok)return -1;if(!volume)return 0;
    for(size_t n=0;n<state_->mounts.size();++n)if(state_->mounts[n].volume&&state_->mounts[n].volume->ResourceKey()==volume->ResourceKey()){state_->Show(int(n),2,state_->mounts[n].root);return 1;}
    state_->mounts.push_back({volume,wide,nullptr,-1});int i=int(state_->mounts.size()-1);state_->Root(i);state_->Show(i,2,state_->mounts[i].root);
    RememberImage(wide);
    return 1;
#endif
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

bool MainMacBrowser::ScanRemovable(bool silent,bool startupOnly){
 if(startupOnly&&state_->removableScanned)return true;state_->removableScanned=true;
 Removable::Discovery snapshot;auto result=ShowStorageWaitJob([&](Storage::Job& job){snapshot=Removable::Scan(job);return Storage::Result{};},Hidden(session_),session_.Owner());
 if(result.code){if(!silent&&!Hidden(session_))session_.Owner()->MessageBox(result.message.c_str(),"Checking Mac removable disks",MB_OK|MB_ICONERROR);return false;}
 int selected=-1;uint32_t folder=2;state_->Selected(selected,folder);auto tree=session_.Tree();bool enabled=tree->m_selchange_enabled!=0;tree->enable_selchange(0);std::set<int> retained;
 for(auto& found:snapshot.volumes){int slot=-1;auto key=VolumeCoord::Wide(found.volume->ResourceKey());for(size_t i=0;i<state_->mounts.size();++i)if(state_->mounts[i].key==key){slot=int(i);break;}if(slot<0){slot=int(state_->mounts.size());state_->mounts.push_back({found.volume,key,nullptr,-1});}auto& mount=state_->mounts[slot];mount.volume=found.volume;mount.physicalPath=found.device.path;mount.physicalOffset=found.partition.offset;mount.suffix=found.label;if(mount.root){tree->delete_subdir_contents(mount.root);auto old=mount.root;mount.root=nullptr;tree->m_ct->DeleteItem(old);}state_->Root(slot);retained.insert(slot);}
 for(size_t i=0;i<state_->mounts.size();++i){auto& mount=state_->mounts[i];if(mount.physicalPath.empty()||retained.count(int(i)))continue;if(mount.root)tree->m_ct->DeleteItem(mount.root);mount.root=nullptr;mount.volume.reset();}
 tree->enable_selchange(enabled);
 if(selected>=0&&!state_->mounts[selected].physicalPath.empty()){if(retained.count(selected))state_->Show(selected,2,state_->mounts[selected].root);else{session_.ClearCurrent();session_.SetLocationTitle({});session_.List()->initialize_volume("",0);session_.List()->SetBrowserCapabilities({});}}
 if(!silent&&!snapshot.errors.empty()&&!Hidden(session_)){std::string message="Some removable disks could not be read:\r\n";for(size_t i=0;i<snapshot.errors.size()&&i<8;++i)message+="\r\n"+snapshot.errors[i];session_.Owner()->MessageBox(message.c_str(),"Checking Mac removable disks",MB_OK|MB_ICONINFORMATION);}
 return true;
}
int MainMacBrowser::OpenRemovableRoot(const char* root){
 std::vector<Removable::Mounted> found;auto wide=FromAnsi(root);auto result=ShowStorageWaitJob([&](Storage::Job&){found=Removable::FromDriveRoot(wide);return Storage::Result{};},Hidden(session_),session_.Owner());
 if(result.code){session_.List()->initialize_volume("",0);session_.List()->SetBrowserCapabilities({});if(!Hidden(session_))session_.Owner()->MessageBox(result.message.c_str(),"Reading Mac removable disk",MB_OK|MB_ICONERROR);return -1;}
 if(found.empty())return 0;
 AdoptRemovable(found,false);
 return 1;
}
void MainMacBrowser::AdoptRemovable(std::vector<Removable::Mounted>& found,bool asynchronous){
 int selected=-1;
 for(auto& v:found){auto key=VolumeCoord::Wide(v.volume->ResourceKey());int slot=-1;for(size_t i=0;i<state_->mounts.size();++i)if(state_->mounts[i].key==key){slot=int(i);break;}if(slot<0){slot=int(state_->mounts.size());state_->mounts.push_back({v.volume,key,nullptr,-1});}auto& mount=state_->mounts[slot];mount.volume=v.volume;mount.physicalPath=v.device.path;mount.physicalOffset=v.partition.offset;mount.suffix=v.label;if(!mount.root)state_->Root(slot);if(selected<0)selected=slot;}
 if(selected<0)return;
 auto item=state_->mounts[selected].root;
 if(asynchronous){
  auto tree=session_.Tree();auto enabled=tree->SelectionNotificationsEnabled();
  tree->enable_selchange(0);tree->GetTreeCtrl().SelectItem(item);tree->enable_selchange(enabled);
 }
 state_->Show(selected,2,item,false,asynchronous);
}
int MainMacBrowser::OpenCd(const char* root) {
    int drive=HfsPlus::CdDriveFromRoot(root);if(drive<0)return 0;
    if(HasCdRoot(drive))return 0; // Windows side stays selectable.
    bool detected=false,busy=false;std::shared_ptr<Volume> volume;auto result=ShowStorageWaitJob([&](Storage::Job&){try{VolumeCoord::Lease read({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}});volume=MacBrowser::OpenCdRoot(root);}catch(const VolumeCoord::Busy&){busy=true;}catch(const MacBrowser::DetectedVolumeError&){detected=true;throw;}return Storage::Result{};},Hidden(session_),session_.Owner());
    if(busy){std::string saved=root;auto doc=session_.Document();int index=-1;for(int n=0;n<doc->m_fat_count;++n)if(toupper((unsigned char)doc->m_fats[n].m_letter)-'A'==drive){index=n;break;}session_.Defer(VolumeCoord::DeviceKey(drive),true,[this,index,saved]{if(index>=0)session_.Document()->open_fat_directory(index,const_cast<char*>(saved.c_str()),1);else OpenCd(saved.c_str());},true);return 1;}
    if(result.code){if(detected){if(!Hidden(session_))AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);return -1;}return 0;}if(!volume)return 0;
    AdoptCd(drive,root,std::move(volume),false);
    return 1;
}
bool MainMacBrowser::HasCdRoot(int drive)const{
 return std::any_of(state_->mounts.begin(),state_->mounts.end(),
    [&](const auto& mount){return mount.drive==drive&&mount.root;});
}
void MainMacBrowser::ScanCds() {
#ifndef HFV_TESTING
    struct Probe {
        int drive;
        std::string root;
        std::shared_ptr<Volume> existing,replacement;
        bool changed=false;
    };
    std::vector<Probe> probes;
    const auto doc=session_.Document();
    for(int index=0;index<doc->m_fat_count;++index){
        const auto& entry=doc->m_fats[index];
        if(entry.m_type!=DRIVE_CDROM)continue;
        const auto drive=toupper(static_cast<unsigned char>(entry.m_letter))-'A';
        const auto mounted=std::find_if(state_->mounts.begin(),state_->mounts.end(),
            [&](const auto& item){return item.drive==drive&&item.volume;});
        probes.push_back({drive,std::string((LPCSTR)entry.m_volume_name),
            mounted==state_->mounts.end()?nullptr:mounted->volume});
    }
    if(probes.empty())return;
    const auto result=ShowStorageWaitJob([&](Storage::Job& job){
        for(auto& probe:probes){
            if(job.cancelled())throw Storage::Cancelled();
            try{
                VolumeCoord::Lease read({{VolumeCoord::DeviceKey(probe.drive),VolumeCoord::Mode::Read}});
                if(probe.existing){
                    try{probe.existing->ValidateMedia();continue;}
                    catch(const std::exception&){}
                }
                probe.changed=true;
                probe.replacement=MacBrowser::OpenCdRoot(probe.root.c_str());
            }catch(const VolumeCoord::Busy&){}
            catch(const MacBrowser::DetectedVolumeError&){/* Explicit selection reports mount errors. */}
        }
        return Storage::Result{};
    },Hidden(session_),session_.Owner());
    if(result.code)return;
    for(auto& probe:probes)if(probe.changed){
        if(probe.existing)ReleaseCd(probe.drive);
        if(probe.replacement&&!HasCdRoot(probe.drive))
            AdoptCd(probe.drive,probe.root,std::move(probe.replacement),false,false);
    }
#endif
}

void MainMacBrowser::AdoptCd(int drive,const std::string& root,std::shared_ptr<Volume> volume,bool asynchronous,bool select){
 state_->mounts.push_back({std::move(volume),FromAnsi(root.c_str()),nullptr,drive});
 int index=int(state_->mounts.size()-1);
 state_->Root(index);
 if(!select)return;
 auto item=state_->mounts[index].root;
 if(asynchronous){
  auto tree=session_.Tree();auto enabled=tree->SelectionNotificationsEnabled();
  tree->enable_selchange(0);tree->GetTreeCtrl().SelectItem(item);tree->enable_selchange(enabled);
 }
 state_->Show(index,2,item,false,asynchronous);
}
bool MainMacBrowser::Select(LPARAM data){auto p=reinterpret_cast<lparam_struct*>(data);if(!p||p->type!=LIST_MAC)return false;
    state_->Show(p->volinx,uint32_t(p->id),session_.Tree()->m_ct->GetSelectedItem(),false,state_->UseAsyncNavigation());return true;}
bool MainMacBrowser::Refresh(){int i;uint32_t id;if(!state_->Selected(i,id))return false;
    auto& mount=state_->mounts[i];
    if(state_->UseAsyncNavigation()){
        ForgetFolders(mount.volume);
        return state_->Show(i,id,session_.Tree()->m_ct->GetSelectedItem(),mount.drive<0&&mount.physicalPath.empty(),true,true);
    }
    if(mount.volume->HasPersistentWorker()&&!VolumeCoord::Readable(mount.volume->ResourceKey()))
        return state_->Show(i,id,session_.Tree()->m_ct->GetSelectedItem());
    if(session_.Defer(mount.volume->ResourceKey(),mount.drive>=0,[this]{Refresh();}))return true;
    ForgetFolders(mount.volume);
    bool ready=Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{mount.volume->ResourceKey(),VolumeCoord::Mode::Read}});if(!mount.physicalPath.empty())mount.volume=Removable::Reopen(mount.physicalPath,mount.physicalOffset);else if(mount.drive>=0){auto fresh=MacBrowser::OpenCd(mount.drive);if(!fresh)throw HfsPlus::Error("No supported Mac disc is present");mount.volume=std::move(fresh);}else mount.volume->Reload();});
    if(!ready){if(readBusy){session_.Defer(mount.volume->ResourceKey(),mount.drive>=0,[this]{Refresh();},true);return true;}if(mount.drive>=0){ReleaseCd(mount.drive);return false;}session_.List()->initialize_volume("",0);return true;}
    if(!mount.physicalPath.empty()){id=2;TreeText(session_,mount.root,Wide(mount.volume->Name())+L" - "+mount.suffix+L" [read-only]");state_->Show(i,2,mount.root);}
    else if(mount.drive>=0){id=2;state_->Show(i,2,mount.root);}
    else if(!state_->Show(i,id,session_.Tree()->m_ct->GetSelectedItem())&&id!=2)state_->Show(i,2,mount.root);
    return true;
}
bool MainMacBrowser::RefreshForChange(const std::string& key){
 int index;uint32_t id;if(!state_->Selected(index,id)||state_->mounts[index].volume->ResourceKey()!=key)return false;
 // Read the ancestry, ancestor directories and current listing under one read
 // lease, then rebuild the affected tree path and list in a single UI batch.
 if(!state_->Show(index,id,session_.Tree()->m_ct->GetSelectedItem(),true,state_->UseAsyncNavigation()))throw HfsPlus::Error("Cannot refresh the changed volume");return true;
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
void MainMacBrowser::Clear(){session_.CancelNavigation();state_->mounts.clear();state_->current=-1;}
bool MainMacBrowser::Activate(int row){auto model=session_.List()->BrowserRowAt(row);
    if(!model||!model->location.IsMounted())return false;if(!session_.ListingAvailable())return true;if(model->entry.directory){auto item=model->treeItem;if(!item)return true;session_.Tree()->m_ct->SelectItem(item);Select(session_.Tree()->m_ct->GetItemData(item));}
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
FileOperations::Location MainMacBrowser::CaptureItem(int row){
 auto item=state_->Item(row,false);
 if(item.LegacyType()==LIST_FAT&&item.expectedIdentity.empty()){
  if(!Run(session_,[&](Storage::Job&){VolumeCoord::Lease read({{VolumeCoord::PathKey(item.WindowsPath()),VolumeCoord::Mode::Read}});item.expectedIdentity=DescribeWindowsFile(item.WindowsPath()).identity;}))throw HfsPlus::Error("Cannot identify the selected Windows entry");
  if(item.expectedIdentity.empty())throw HfsPlus::Error("Cannot safely identify the selected Windows entry");
 }
 return item;
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
bool MainMacBrowser::Properties(int index)
{
    const auto row=session_.List()->BrowserRowAt(index);
    if(!session_.ListingAvailable()||!row||row->location.IsNone()||row->location.IsWindows())return false;
    return MetadataDialogs::Show(row->location,session_.Owner());
}
BrowserTransfers::Selection MainMacBrowser::SelectedItems(bool cut){
 BrowserTransfers::Selection out;if(!session_.ListingAvailable())return out;out.canMove=session_.List()->BrowserSelectionAllows(false);
 if(cut&&!out.canMove)return out;
 auto& list=session_.List()->GetListCtrl();
 for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){if(session_.List()->BrowserRowAt(row))out.items.push_back(state_->Item(row,cut));}
 bool missing=false;for(auto& item:out.items)missing|=item.LegacyType()==LIST_FAT&&item.expectedIdentity.empty();
 if(missing&&!Run(session_,[&](Storage::Job&){for(auto& item:out.items)if(item.LegacyType()==LIST_FAT&&item.expectedIdentity.empty()){VolumeCoord::Lease read({{VolumeCoord::PathKey(item.WindowsPath()),VolumeCoord::Mode::Read}});item.expectedIdentity=MacBrowser::DescribeWindowsFile(item.WindowsPath()).identity;}}))out.items.clear();
 return out;
}
FileOperations::Location MainMacBrowser::TargetFolder(HTREEITEM item){return state_->Target(item);}
BrowserDirectory::Folder MainMacBrowser::DescribeFolder(HTREEITEM item){return state_->Folder(item);}
void MainMacBrowser::BindRow(BrowserDirectory::Row& row,const std::string& key){state_->BindRow(row,key);}
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
bool MainMacBrowser::Find(){
    int index;uint32_t folder;if(!state_->Selected(index,folder))return false;
    FindLocation location;location.mac=true;location.native=state_->mounts[index].volume;location.nativeFolder=folder;
    location.root=BrowserPresentation::Utf8(location.native->Name());location.directory=location.root;
    for(auto item=session_.Tree()->m_ct->GetSelectedItem();item;item=session_.Tree()->m_ct->GetParentItem(item)){
        auto p=reinterpret_cast<lparam_struct*>(session_.Tree()->m_ct->GetItemData(item));if(p&&p->type==LIST_MAC)location.nativePath.insert(location.nativePath.begin(),uint32_t(p->id));}
    Run(session_,[&](Storage::Job&){for(size_t n=1;n<location.nativePath.size();++n)location.directory+="/"+BrowserPresentation::Utf8(location.native->Stat(location.nativePath[n]).name);});
    FindHit found;if(!ShowFindFilesDialog(session_.Owner(),location,found))return true;
    if(!session_.RevealFindHit(found))
        session_.RejectNavigation(location.native->ResourceKey(),"The search volume is no longer attached.");
    return true;
}

bool MainMacBrowser::OpenMountedFolderFrom(const MainMacBrowser& source,const BrowserNavigation::Request& request)
{
 const auto& volume=request.folder.Volume();
 auto existing=std::find_if(state_->mounts.begin(),state_->mounts.end(),[&](const State::Mount& mount){return mount.volume==volume;});
 if(existing==state_->mounts.end()){
  auto original=std::find_if(source.state_->mounts.begin(),source.state_->mounts.end(),[&](const State::Mount& mount){return mount.volume==volume;});
  if(original==source.state_->mounts.end())return false;
  auto mount=*original;
  mount.root=nullptr;
  state_->mounts.push_back(std::move(mount));
  const auto index=int(state_->mounts.size()-1);
  state_->Root(index);
  return state_->Show(index,request.folder.Id(),state_->mounts[index].root,true,true,false,&request);
 }
 const auto index=int(existing-state_->mounts.begin());
 return state_->Show(index,request.folder.Id(),existing->root,true,true,false,&request);
}

bool MainMacBrowser::RevealFindHit(const BrowserNavigation::Request& request)
{
    const auto& volume=request.folder.Volume();
    for(size_t index=0;index<state_->mounts.size();++index){
        const auto& mount=state_->mounts[index];
        if(!mount.root||mount.volume!=volume)continue;
        auto anchor=session_.Tree()->GetTreeCtrl().GetSelectedItem();
        return state_->Show(int(index),request.folder.Id(),anchor,true,true,false,&request);
    }
    return false;
}
void MainMacBrowser::Import(){if(!Active()||!Writable())return;std::wstring path;if(!FileDialog(session_,path,false))return;
    Location source;source.SetWindows(path);try{state_->Transfer({source},state_->Target());}catch(const std::exception& e){AfxMessageBox(e.what());}}
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
