// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacBrowserModel.h"
#include "StorageHandoff.h"
#include "StorageLocation.h"
#include "ImageHandleTransfer.h"
#include <io.h>
#include "VolumeCoordinator.h"
#include "LegacyEngine.h"
#include "ReadOnlySource.h"
#include "HfsPlusDisk.h"
#include "charset.h"
#include <fcntl.h>
#include <set>
extern "C" {
#undef ERROR
#include "hfs/libhfs/internal.h"
#include "hfs/Replacement.h"
#include "hfs/libhfs/block.h"
#include "hfs/libhfs/data.h"
int hfs_recognizes_image_fd(int);
int hfs_readdir2(hfsdir*,hfsdirent*,CatKeyRec*,CatDataRec*);
}
#include "ClassicCatalog.h"
#include "ClassicCopyArchive.h"
#include "ClassicImagePath.h"
#include "ClassicHfsRecovery.h"
#include "ReadOnlyCdBackend.h"
namespace MacBrowser {
namespace {
void Check(bool ok) { if (!ok) throw HfsPlus::Error(hfs_error ? hfs_error : "Classic HFS operation failed"); }
int AttachImage(HANDLE opened,int mode)
{
    const auto handle=ImageHandleTransfer::Duplicate(opened);
    const int fd=_open_osfhandle(intptr_t(handle),mode|O_BINARY|_O_NOINHERIT);
    if(fd<0){CloseHandle(handle);throw HfsPlus::Error("Cannot attach the verified classic HFS image");}
    return fd;
}
hfsvol* MountHostImage(const std::wstring& path,const std::string& expected,bool write,bool session)
{
    // Legacy aliases share one catalog cache. Match its held handle, since
    // asking libhfs to resolve the pathname again can select a different file.
    if(!session){
        for(auto* mounted=hfs_mounts;mounted;mounted=mounted->next){
            if(mounted->fd<0||mounted->fd>=0x10000000||(mounted->pnum!=0&&mounted->pnum!=1)||
               (write&&(mounted->flags&HFS_READONLY)))continue;
            const auto handle=reinterpret_cast<HANDLE>(_get_osfhandle(mounted->fd));
            if(VolumeCoord::ImageFileKey(handle)==expected)return hfs_retain_mount(mounted);
        }
    }
    std::unique_ptr<HfsPlus::Source> image;
    try{image=HfsPlus::OpenImage(path.c_str(),write);}
    catch(const HfsPlus::Error&){
        if(!write)throw;
        image=HfsPlus::OpenImage(path.c_str(),false);
    }
    const auto opened=reinterpret_cast<HANDLE>(image->WorkerFiles().front().second);
    StorageLocation::ValidateImageFile(opened,expected);
    const int mode=image->Writable()?O_RDWR:O_RDONLY;
    return hfs_mount_image_fd(AttachImage(opened,mode),1,mode,session);
}
std::u16string Decode(const std::string &s) {
    int n = MultiByteToWideChar(10000, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(10000, 0, s.data(), int(s.size()), &w[0], n); return U16(w);
}
std::string Encode(const std::u16string &s) {
    BOOL lost = FALSE; auto w = Wide(s); char out[32];
    int n = WideCharToMultiByte(10000, WC_NO_BEST_FIT_CHARS, w.data(), int(w.size()), out, sizeof(out)-1, nullptr, &lost);
    if (!n || lost || n > HFS_MAX_FLEN || s.empty() || (s.find(u':') != s.npos || s.find(u'\0') != s.npos))
        throw HfsPlus::Error("This name cannot be represented in classic HFS (31 MacRoman bytes maximum)");
    return {out, size_t(n)};
}
struct RootScope {
 hfsvol* volume;long previous;
 explicit RootScope(hfsvol* v):volume(v),previous(0){if(!v)throw HfsPlus::Error("This volume is being used by the copy worker");previous=hfs_getcwd(v);Check(hfs_setcwd(v,HFS_CNID_ROOTDIR)==0);}
 ~RootScope(){hfs_setcwd(volume,previous);}
};
class Classic final : public Volume, public WorkerEndpoint {
    std::shared_ptr<LegacyEngine::Context> context_;
    hfsvol *volume_ = nullptr;
    std::wstring hostImage_;
    std::string image_;
    std::string resourceKey_;
    std::string imageIdentity_;
    bool processLocal_=false;
    bool write_;bool suspended_=false;
    bool session_=false, markedDirty_=false, closeFailed_=false;
    void BeforeWrite() {
        if (!Writable()) throw HfsPlus::Error("Classic HFS volume is read-only");
        if (session_ && !markedDirty_) {
            Check(hfs_mark_session_dirty(volume_) == 0);
            markedDirty_ = true;
        }
    }
    std::unique_ptr<ReadOnlySources::Registration> registration_;
    std::shared_ptr<HfsPlus::Source> media_;
    std::optional<ClassicHfsRecovery::ImageRange> ndifRecovery_;
    std::unique_ptr<ClassicHfsRecovery::Transaction> copyTransaction_;
    bool copyWasDirty_=false;
    int cdDrive_=-1;
    std::map<uint32_t,std::string> paths_;
    std::map<uint32_t,uint32_t> artworkIds_;
    uint32_t next_ = 3;
    uint32_t Id(const std::string& path, uint32_t cnid=0) {
        if (session_) {
            if(!cnid){hfsdirent entry{};Check(hfs_stat(volume_,const_cast<char*>(path.c_str()),&entry)==0);cnid=uint32_t(entry.cnid);}
            paths_[cnid]=path;
            return cnid;
        }
        for (const auto& item : paths_) if (item.second == path) return item.first;
        paths_[next_]=path;
        return next_++;
    }
    std::string Path(uint32_t id) {
        if(!session_ || id==2)return paths_.at(id);
        RootScope scope(volume_);
        const auto cached=paths_.find(id);
        hfsdirent entry{};
        if(cached!=paths_.end() && hfs_stat(volume_,cached->second.data(),&entry)==0 && uint32_t(entry.cnid)==id)
            return cached->second;

        // The catalog ID survives a handoff. Rebuild stale paths lazily after
        // a remount or after an ancestor moved.
        std::vector<std::string> pending{":"};
        std::set<uint32_t> seen{2};
        while(!pending.empty()){
            auto path=std::move(pending.back());pending.pop_back();
            auto directory=hfs_opendir(volume_,path.data());Check(directory!=nullptr);
            struct Close{hfsdir* value;~Close(){hfs_closedir(value);}} close{directory};
            while(hfs_readdir(directory,&entry)==0){
                const auto child=path+entry.name;
                const auto cnid=uint32_t(entry.cnid);
                paths_[cnid]=child;
                if(cnid==id)return child;
                if((entry.flags&HFS_ISDIR)&&seen.insert(cnid).second)pending.push_back(child+":");
            }
            if(errno!=ENOENT)Check(false);
        }
        throw HfsPlus::Error("The selected classic HFS catalog entry no longer exists");
    }
    Entry Describe(const std::string &path, uint32_t id) {
        RootScope scope(volume_);hfsdirent in; Check(hfs_stat(volume_, const_cast<char*>(path.c_str()), &in) == 0); Entry e;
        e.id=id;if(id==2)e.parent=1;else{auto parent=path.substr(0,path.find_last_of(':'));if(parent.empty())parent=":";e.parent=session_?uint32_t(in.parid):Id(parent);}
        e.name=Decode(in.name);e.identity=EntryIdentity(imageIdentity_+":"+std::to_string(uint32_t(in.cnid)));e.resolvedId=uint32_t(in.cnid);
        e.directory=(in.flags&HFS_ISDIR)!=0;e.protectedFile=(in.flags&HFS_ISLOCKED)!=0;
        e.created=uint32_t(int64_t(in.crdate)+2082844800LL);e.modified=uint32_t(int64_t(in.mddate)+2082844800LL);e.datesValid=true;
        e.additionalDates.backup=uint32_t(d_tomtime(static_cast<unsigned long>(in.bkdate)));
        if(!e.directory){e.forks[0].size=in.u.file.dsize;e.forks[1].size=in.u.file.rsize;
            memcpy(e.finder.data(),in.u.file.type,4);memcpy(e.finder.data()+4,in.u.file.creator,4);}
        Check(hfs_getfinderinfo(volume_,const_cast<char*>(path.c_str()),e.finder.data())==0);return e;
    }
public:
    Classic(const std::string& image,std::string root,bool write,bool session=false,std::wstring hostImage={},
            const std::string& expectedImage={})
        :hostImage_(std::move(hostImage)),image_(image),write_(write),session_(session)
    {
        if(hostImage_.empty()&&!ReadOnlySources::IsPath(image.c_str())&&!readonly_media_path(image.c_str())&&
           physical_floppy_path(image.c_str())<0)
            hostImage_=ClassicImagePath::IsEncoded(image.c_str())?ClassicImagePath::Decode(image.c_str()):VolumeCoord::Wide(image);
        resourceKey_=hostImage_.empty()?VolumeCoord::ImageKey(image):VolumeCoord::ImageKey(hostImage_);
        if(!expectedImage.empty()&&resourceKey_!=expectedImage)
            throw Storage::MediaChanged("The selected classic HFS image was replaced.");
        context_=LegacyEngine::ForVolume(resourceKey_);LegacyEngine::Scope engine(context_);
        std::vector<char> converted(root.begin(),root.end());converted.push_back(0);pc_to_mac_charset((unsigned char*)converted.data());root=converted.data();
        if(hostImage_.empty()){
            imageIdentity_=image;
            volume_=(session_?hfs_mount_session:hfs_mount)(image_.data(),1,write?O_RDWR:O_RDONLY);
        }else{
            volume_=MountHostImage(hostImage_,resourceKey_,write,session_);
            imageIdentity_=resourceKey_.substr(6);
        }
        Check(volume_!=nullptr);
        auto colon=root.find(':');root=colon==root.npos?":":root.substr(colon);
        while(root.size()>1&&root.back()==':'&&root.find(':')<root.size()-1)root.pop_back();paths_[2]=root;
        try {Check(Describe(root,2).directory);}catch(...){hfs_umount(volume_);volume_=nullptr;throw;}
    }
    Classic(std::shared_ptr<HfsPlus::Source> source,const std::string& key,bool write,bool stable,
            std::optional<ClassicHfsRecovery::ImageRange> ndif={},bool recovered=false)
        :write_(write),session_(stable),ndifRecovery_(ndif){
        resourceKey_=key;context_=LegacyEngine::ForVolume(key);LegacyEngine::Scope engine(context_);
        media_=std::move(source);
        processLocal_=media_->ProcessLocal();
        cdDrive_=media_->MediaDrive();
        registration_.reset(new ReadOnlySources::Registration(media_,write_));
        image_=registration_->Path();
        uint32_t generation=0;
        if((session_||media_->MediaDrive()>=0) && media_->MediaGeneration(generation))
            imageIdentity_=key+":"+std::to_string(generation);
        else imageIdentity_=media_->WorkerFiles().empty()?key+":"+image_:key;
        volume_=(session_&&!recovered?hfs_mount_session:hfs_mount)(image_.data(),1,write_?O_RDWR:O_RDONLY);Check(volume_!=nullptr);paths_[2]=":";
        try{Check(Describe(":",2).directory);}catch(...){hfs_umount(volume_);volume_=nullptr;throw;}
    }
    ~Classic(){LegacyEngine::Scope engine(context_);if(volume_)hfs_umount(volume_);}
    WorkerExecution Execution()const override{WorkerExecution out;out.classicConversion=cdDrive_>=0;out.stableCatalogIds=session_;out.processLocal=processLocal_;return out;}
    WorkerOrigin WorkerDescription()const override {
        LegacyEngine::Scope engine(context_);
        WorkerOrigin out;
        if (media_) {
            uint32_t generation=0;
            if (media_->MediaDrive()>=0) {
                out.kind=WorkerOrigin::Cd;
                out.drive=media_->MediaDrive();
                out.generationKnown=media_->MediaGeneration(generation);
                out.generation=generation;
            }else{
                const auto files=media_->WorkerFiles();
                if(!files.empty()){
                    out.kind=WorkerOrigin::Image;
                    out.path=files.front().first;
                }
            }
            return out;
        }
        if (ReadOnlySources::IsPath(image_.c_str())) return out;
        out.kind=session_&&!hostImage_.empty()?WorkerOrigin::Image:WorkerOrigin::Classic;
        out.path=hostImage_.empty()?VolumeCoord::Wide(image_):hostImage_;
        out.root=paths_.at(2);
        out.identity=resourceKey_;
        out.writable=Writable();
        return out;
    }
    void ValidateMedia() override {
        LegacyEngine::Scope engine(context_);
        if (media_) media_->CheckMedia();
        else if (ReadOnlySources::IsPath(image_.c_str())) throw Storage::MediaChanged("The CD was released.");
    }
    void ReleaseMedia() override {
        LegacyEngine::Scope engine(context_);
        if (!registration_) return;
        Close();
        registration_.reset();
        media_.reset();
    }
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{LegacyEngine::Scope engine(context_);if(media_)return media_->WorkerFiles();if(registration_||!volume_||volume_->fd>=0x10000000)return {};auto file=_get_osfhandle(volume_->fd);if(file==-1)throw HfsPlus::Error("Cannot transfer classic image handle");return {{hostImage_.empty()?VolumeCoord::Wide(image_):hostImage_,uintptr_t(file)}};}
    void WorkerUncleanExit()override{LegacyEngine::Scope engine(context_);write_=false;}
    void SuspendWorker()override{LegacyEngine::Scope engine(context_);if(registration_)return;if(volume_){auto v=volume_;volume_=nullptr;suspended_=true;int closed=hfs_umount(v);if(closed)write_=false;Check(closed==0);}}
    void ResumeWorker()override{
        LegacyEngine::Scope engine(context_);
        if(!suspended_)return;
        if(hostImage_.empty())
            volume_=(session_?hfs_mount_session:hfs_mount)(image_.data(),1,write_?O_RDWR:O_RDONLY);
        else volume_=MountHostImage(hostImage_,resourceKey_,write_,session_);
        Check(volume_!=nullptr);
        suspended_=false;
        markedDirty_=false;
    }
    void Close()override {
        LegacyEngine::Scope engine(context_);
        if (volume_) {
            auto v=volume_; volume_=nullptr;
            if(hfs_umount(v)!=0){closeFailed_=true;Check(false);}
        }
    }
    bool FinishWorkerWrite(Volume&) override {
        LegacyEngine::Scope engine(context_);
        const bool safe = !closeFailed_ && (!volume_ || Writable());
        Close();
        return safe;
    }
    std::string ResourceKey()const override{return resourceKey_;}
    std::u16string Name()const override{LegacyEngine::Scope engine(context_);hfsvolent info{};Check(volume_!=nullptr);Check(hfs_vstat(volume_,&info)==0);return Decode(info.name);}
    const char *Kind()const override{return "HFS";}
    int CdDrive()const override{return cdDrive_;}
    bool Writable()const override{LegacyEngine::Scope engine(context_);return write_&&volume_&&!(volume_->flags&(HFS_READONLY|HFS_WRITE_FAILED));}
    bool CaseSensitive()const override{return false;}
    Capabilities Supports() const override {
        auto result = Volume::Supports();
        result.unicodeNames = false;
        result.classicProperties = true;
        return result;
    }
    bool GetClassicProperties(uint32_t id, hfsdirent& entry) override {
        LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);
        auto path=Path(id);
        Check(hfs_stat(volume_,path.data(),&entry)==0);
        return true;
    }
    void SetClassicProperties(uint32_t id, const hfsdirent& entry) override {
        LegacyEngine::Scope engine(context_);
        if(id==2)throw HfsPlus::Error("Select a file or folder below the volume root");
        std::string path;
        {
            RootScope scope(volume_);
            path=Path(id);
        }
        auto desired=entry;
        auto destination=path.substr(0,path.find_last_of(':')+1)+Encode(Decode(desired.name));
        DurableMutation([&](bool){
            RootScope scope(volume_);
            BeforeWrite();
            Check(hfs_editattrs(volume_,path.data(),destination.data(),&desired)==0);
        });
        paths_[id]=std::move(destination);
    }
    void ValidateCopyNames(uint32_t,const std::vector<Entry>& entries,bool)override{
        ValidateClassicCopyNames(entries);
    }
    void ValidateEntry(const Entry& e)const override{
        Volume::ValidateEntry(e);if(e.datesValid&&(e.created>UINT32_MAX||e.modified>UINT32_MAX))throw HfsPlus::Error("Classic HFS cannot preserve dates after February 2040");
        if(e.additionalDates.backup.value_or(0)>UINT32_MAX)throw HfsPlus::Error("Classic HFS cannot preserve this backup date");
        Encode(e.name);if(e.forks[0].size>HFS_MAX_FORK_SIZE||e.forks[1].size>HFS_MAX_FORK_SIZE)
            throw HfsPlus::Error("Fork exceeds the classic HFS 2 GiB minus one byte limit");
    }
    uint64_t ClassicAllocationBlockSize()const override{
        LegacyEngine::Scope engine(context_);
        hfsvolent info{};
        Check(volume_&&hfs_vstat(volume_,&info)==0);
        return info.alblocksz;
    }
    uint64_t FreeBytes()const override{LegacyEngine::Scope engine(context_);hfsvolent e;Check(volume_!=nullptr);Check(hfs_vstat(volume_,&e)==0);return e.freebytes;}
    Entry Stat(uint32_t id)override{LegacyEngine::Scope engine(context_);return Describe(Path(id),id);}
    void ValidateCopyTo(uint32_t id,Volume& target,uint32_t parent)override {LegacyEngine::Scope engine(context_);
        auto dst=dynamic_cast<Classic*>(&target);if(!dst)return;RootScope sourceScope(volume_),targetScope(dst->volume_);
        bool same=!_stricmp(image_.c_str(),dst->image_.c_str());
        if(!same){
            HANDLE a=CreateFileA(image_.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            HANDLE b=CreateFileA(dst->image_.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            BY_HANDLE_FILE_INFORMATION x{},y{};
            if(a!=INVALID_HANDLE_VALUE&&b!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(a,&x)&&GetFileInformationByHandle(b,&y))
                same=x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
            if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);
        }
        if(!same)return;
        auto source=Path(id),directory=dst->Path(parent);hfsdirent from,to;
        Check(hfs_stat(volume_,const_cast<char*>(source.c_str()),&from)==0&&hfs_stat(dst->volume_,const_cast<char*>(directory.c_str()),&to)==0);
        if(from.flags&HFS_ISDIR){long at=to.cnid;std::set<long> visited;
            for(;;){if(at==from.cnid||!visited.insert(at).second)throw HfsPlus::Error("Cannot copy a classic HFS folder into itself or its descendants");
                if(at==HFS_CNID_ROOTDIR)break;char name[32];Check(hfs_dirinfo(dst->volume_,&at,name)==0);}
        }
        auto name=directory+(directory.back()==':'?"":":")+from.name;
        if(hfs_stat(dst->volume_,const_cast<char*>(name.c_str()),&to)==0&&from.cnid==to.cnid)
            throw HfsPlus::Error("Source and destination are the same classic HFS entry");
    }
    ClassicMountInfo MountInfo() {
        LegacyEngine::Scope engine(context_);
        hfsvolent info{};
        Check(volume_&&hfs_vstat(volume_,&info)==0);
        mac_to_pc_charset(reinterpret_cast<unsigned char*>(info.name));
        ClassicMountInfo result{info.name,info.freebytes,{},resourceKey_};
        const auto drive=physical_floppy_path(image_.c_str());
        DWORD generation=0;
        if(drive>=0&&physical_floppy_generation(drive,&generation))
            result.generation=generation;
        return result;
    }

    void SelectCatalogFolder(const std::string& directory) {
        LegacyEngine::Scope engine(context_);
        auto colon=directory.find(':');
        std::string path=colon==directory.npos?":":directory.substr(colon);
        while(path.size()>1&&path.back()==':')path.pop_back();
        pc_to_mac_charset(reinterpret_cast<unsigned char*>(path.data()));
        Check(Describe(path,2).directory);
        paths_[2]=std::move(path);
    }

    std::vector<ClassicAncestor> Ancestors(uint32_t id) {
        LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);
        std::vector<ClassicAncestor> route;
        std::set<long> visited;
        long node=id;
        while(node!=HFS_CNID_ROOTDIR){
            if(!visited.insert(node).second||route.size()>=MAX_PATH)
                throw HfsPlus::Error("Cyclic or oversized HFS folder path");
            const auto current=node;
            char name[32]{};
            Check(hfs_dirinfo(volume_,&node,name)==0);
            mac_to_pc_charset(reinterpret_cast<unsigned char*>(name));
            route.push_back({uint32_t(current),uint32_t(node),name});
        }
        std::reverse(route.begin(),route.end());
        return route;
    }
    std::vector<ClassicAncestor> ResolveFolders(const std::string& relative, bool showHidden) {
        LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);
        auto path=Path(2);
        uint32_t id=HFS_CNID_ROOTDIR;
        size_t start=0;
        while(start<relative.size()) {
            const auto end=relative.find(':',start);
            auto component=relative.substr(start,end==relative.npos?end:end-start);
            if(component.empty())break;
            pc_to_mac_charset(reinterpret_cast<unsigned char*>(component.data()));
            if(path.back()!=':')path+=':';
            path+=component;
            hfsdirent entry{};
            if(hfs_stat(volume_,path.data(),&entry)<0){
                if(errno==ENOENT)break;
                Check(false);
            }
            if(!(entry.flags&HFS_ISDIR)||(!showHidden&&(entry.fdflags&HFS_FNDR_ISINVISIBLE)))break;
            id=uint32_t(entry.cnid);
            if(end==relative.npos)break;
            start=end+1;
        }
        return Ancestors(id);
    }

    void ReadArtwork(uint32_t catalogId,uint64_t offset,void* data,size_t size) {
        Read(artworkIds_.at(catalogId),1,offset,data,size);
    }

    std::vector<ClassicCatalogEntry> Catalog(Storage::Job* job) {
        LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);
        auto path=Path(2);
        if(path.back()!=':')path+=':';
        hfsdir* dir=hfs_opendir(volume_,path.data());
        Check(dir!=nullptr);
        struct CloseDir{hfsdir* value;~CloseDir(){hfs_closedir(value);}} close{dir};
        std::vector<ClassicCatalogEntry> result;
        hfsdirent entry{};
        CatKeyRec catalogKey{};
        CatDataRec catalogData{};
        static_assert(sizeof(CatDataRec)==sizeof(ClassicCatalogEntry::PackedRecord));
        for(;;){
            if(job&&job->cancelled())throw Storage::Cancelled();
            if(hfs_readdir2(dir,&entry,&catalogKey,&catalogData)<0)break;
            if(!catalogKey.ckrKeyLen)continue;
            ClassicCatalogEntry record;
            record.name=catalogKey.ckrCName;
            memcpy(record.data.data(),&catalogData,sizeof(catalogData));
            if(catalogData.cdrType==cdrFilRec&&entry.u.file.rsize)
                artworkIds_[uint32_t(entry.cnid)]=Id(path+record.name,uint32_t(entry.cnid));
            if(catalogData.cdrType==cdrDirRec){
                auto child=path+record.name+":";
                if(auto sub=hfs_opendir(volume_,child.data())){
                    CloseDir subClose{sub};
                    CatKeyRec key{};
                    CatDataRec data{};
                    while(hfs_readdir2(sub,&entry,&key,&data)==0){
                        if(job&&job->cancelled())throw Storage::Cancelled();
                        if(data.cdrType==cdrFilRec&&!strcmp(key.ckrCName,"Icon\r")){
                            artworkIds_[uint32_t(entry.cnid)]=Id(child+key.ckrCName,uint32_t(entry.cnid));
                            record.folderIcon.emplace();
                            memcpy(record.folderIcon->data(),&data,sizeof(data));
                            break;
                        }
                    }
                }
            }
            result.push_back(std::move(record));
        }
        if(errno!=ENOENT)Check(false);
        return result;
    }
    std::vector<Entry> List(uint32_t parent)override {LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);auto path=Path(parent);if(path.back()!=':')path+=":";hfsdir* dir=hfs_opendir(volume_,const_cast<char*>(path.c_str()));Check(dir!=nullptr);
        struct Close{hfsdir* p;~Close(){hfs_closedir(p);}}close{dir};hfsdirent e;std::vector<Entry> out;
        while(hfs_readdir(dir,&e)==0){auto p=path+e.name;auto item=Describe(p,Id(p,uint32_t(e.cnid)));item.parent=parent;out.push_back(item);}
        if(errno!=ENOENT)Check(false);return out;
    }
    void Read(uint32_t id,unsigned fork,uint64_t at,void* p,size_t n)override {LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);auto path=Path(id);hfsfile* f=hfs_open(volume_,const_cast<char*>(path.c_str()));Check(f!=nullptr);
        struct Close{hfsfile* p;~Close(){hfs_close(p);}}close{f};Check(hfs_setfork(f,fork)==0&&hfs_lseek(f,at,SEEK_SET)==at);
        Check(n<=LONG_MAX&&hfs_read(f,p,(unsigned long)n)==n);
    }
    uint32_t MakeDirectory(uint32_t parent,const std::u16string& name)override{
        LegacyEngine::Scope engine(context_);
        const auto leaf=Encode(name);
        uint32_t id=0;
        DurableMutation([&](bool){
            RootScope scope(volume_);
            const auto folder=Path(parent);
            auto path=folder+(folder.back()==':'?"":":")+leaf;
            BeforeWrite();
            Check(hfs_mkdir(volume_,path.data())==0);
            id=Id(path);
        });
        return id;
    }
    void FinalizeDirectory(uint32_t id,const Entry& desired,const Entry& expected,bool merge)override{
        LegacyEngine::Scope engine(context_);
        if(!Writable())throw HfsPlus::Error("Classic destination is read-only");
        if(!merge)ValidateEntry(desired);
        time_t dates[2]={time_t(int64_t(desired.created)-2082844800LL),
                         time_t(int64_t(desired.modified)-2082844800LL)};
        const auto backup=static_cast<unsigned long>(desired.additionalDates.backup.value_or(0));
        DurableMutation([&](bool){
            RootScope scope(volume_);
            auto path=Path(id);
            BeforeWrite();
            Check(hfs_setdirmetadata(volume_,path.data(),long(expected.resolvedId),
                                     merge?nullptr:desired.finder.data(),
                                     desired.datesValid?dates:nullptr,
                                     desired.additionalDates.backup?&backup:nullptr)==0);
        });
    }
    bool BeginCopyTransaction(const std::string& move) override {
        LegacyEngine::Scope engine(context_);
        if(copyTransaction_)throw HfsPlus::Error("A classic HFS folder copy is already active");
        if(!CanRecoverMutation())return false;
        copyTransaction_=StartRecovery(move);
        copyWasDirty_=markedDirty_;
        return true;
    }
    void PrepareCopyTransaction() override {
        LegacyEngine::Scope engine(context_);
        if(!copyTransaction_)throw HfsPlus::Error("No classic HFS transaction is active");
        Check(hfs_mark_session_clean(volume_)==0);
        markedDirty_=false;
        copyTransaction_->Prepare();
    }
    void FinishCopyTransaction(bool commit) override {
        LegacyEngine::Scope engine(context_);
        if(!copyTransaction_)throw HfsPlus::Error("No classic HFS folder copy is active");
        auto recovery=std::move(copyTransaction_);
        try{
            if(!commit&&!recovery->PublishedMove()){
                RecoverCopy(*recovery);
                return;
            }
            Check(hfs_mark_session_clean(volume_)==0);
            markedDirty_=false;
            recovery->Commit();
        }catch(...){
            if(recovery->Active())RecoverCopy(*recovery);
            throw;
        }
    }
    void WithCopyTransaction(const std::function<void(bool)>& work,const std::string& move) override {
        LegacyEngine::Scope engine(context_);
        Volume::WithCopyTransaction(work,move);
    }
    uint32_t Put(uint32_t parent,const Entry& e,Reader read,bool replace)override {
        return PutFile(parent,e,std::move(read),replace,nullptr);
    }
    uint32_t PutResourceReplacement(uint32_t id,const Entry& entry,Reader reader,
                                    std::function<void(uint64_t,size_t)>)override {
        LegacyEngine::Scope engine(context_);
        hfsdirent original{};
        GetClassicProperties(id,original);
        return PutFile(entry.parent,entry,std::move(reader),true,&original);
    }
private:
    uint32_t PutFile(uint32_t parent,const Entry& e,Reader read,bool replace,const hfsdirent* original) {
        LegacyEngine::Scope engine(context_);
        ValidateEntry(e);
        if(!Writable())throw HfsPlus::Error("Classic HFS volume is read-only");
        if(e.forks[0].size>HFS_MAX_FORK_SIZE||e.forks[1].size>HFS_MAX_FORK_SIZE)throw HfsPlus::Error("Fork exceeds the classic HFS 2 GiB minus one byte limit");
        std::string path;
        bool exists;
        {
            RootScope scope(volume_);
            const auto folder=Path(parent);
            path=folder+(folder.back()==':'?"":":")+Encode(e.name);
            hfsdirent old{};
            exists=hfs_stat(volume_,path.data(),&old)==0;
            if(!exists && errno!=ENOENT)Check(false);
        }
        if(exists&&!replace)throw HfsPlus::Error("Destination already exists");
        uint32_t id=0;
        DurableMutation([&](bool durable){
            RootScope scope(volume_);
            BeforeWrite();
            char type[5]{},creator[5]{};memcpy(type,e.finder.data(),4);memcpy(creator,e.finder.data()+4,4);
            HfsReplacement state{};auto file=replacement_open(&state,volume_,const_cast<char*>(path.c_str()),type,creator);Check(file!=nullptr);
            int result=0;char* error=nullptr;std::exception_ptr failure;
            try {
                std::vector<uint8_t> bytes(1024*1024);
                for(unsigned fork=0;fork<2;++fork){Check(hfs_setfork(file,fork)==0);
                    for(uint64_t at=0;at<e.forks[fork].size;){size_t n=size_t((std::min)(uint64_t(bytes.size()),e.forks[fork].size-at));
                        read(fork,at,bytes.data(),n);Check(hfs_write(file,bytes.data(),(unsigned long)n)==n);at+=n;}}
                hfsdirent meta;Check(hfs_fstat(file,&meta)==0);meta.fdflags=HfsPlus::Disk::U16(e.finder.data()+8);
                if(e.datesValid){meta.crdate=time_t(int64_t(e.created)-2082844800LL);meta.mddate=time_t(int64_t(e.modified)-2082844800LL);}
                if(original)meta.bkdate=original->bkdate;
                else if(e.additionalDates.backup)meta.bkdate=d_toutime(static_cast<unsigned long>(*e.additionalDates.backup));
                const bool locked=original?(original->flags&HFS_ISLOCKED)!=0:e.protectedFile;
                meta.flags=(meta.flags&~HFS_ISLOCKED)|(locked?HFS_ISLOCKED:0);
                Check(hfs_fsetattr(file,&meta)==0);Check(hfs_fsetfinderinfo(file,e.finder.data())==0);
            }catch(...){result=-1;failure=std::current_exception();}
            if(hfs_close(file)<0){result=-1;error=hfs_error;}
            replacement_finish(&state,&result,&error);
            if(result){
                if(!exists&&!durable)hfs_note_rollback(hfs_delete(volume_,path.data())==0?1:3);
                if(failure)std::rethrow_exception(failure);
                throw HfsPlus::Error(error?error:"Classic HFS copy failed");
            }
            Check(hfs_flush(volume_)==0);
            id=Id(path);
        });
        return id;
    }
    bool CanRecoverMutation() {
        if(registration_&&!ndifRecovery_)return false;
        Check(Writable());
        return ClassicHfsRecovery::Transaction::Supported(volume_->fd,ndifRecovery_?&*ndifRecovery_:nullptr);
    }
    std::unique_ptr<ClassicHfsRecovery::Transaction> StartRecovery(const std::string& move={}) {
        Check(hfs_flush(volume_)==0);
        auto recovery=std::make_unique<ClassicHfsRecovery::Transaction>(volume_->fd,ndifRecovery_?&*ndifRecovery_:nullptr,move);
        if(!recovery->Active())throw HfsPlus::Error("Cannot start the classic HFS durable write transaction");
        return recovery;
    }
    void DurableMutation(const std::function<void(bool)>& work)
    {
        if(copyTransaction_){work(true);return;}
        if(!CanRecoverMutation()){work(false);return;}
        auto recovery=StartRecovery();
        const bool wasDirty=markedDirty_;
        try{
            work(true);
            recovery->Commit();
        }catch(...){
            if(recovery->Active())RecoverFailedMutation(*recovery,wasDirty);
            throw;
        }
    }
    void RecoverCopy(ClassicHfsRecovery::Transaction& recovery) {
        struct Report {
            ~Report(){Storage::OutcomeEvidence::NoteRollback(static_cast<Storage::Rollback>(hfs_take_rollback()));}
        } report;
        RecoverFailedMutation(recovery,copyWasDirty_);
    }
    void RecoverFailedMutation(ClassicHfsRecovery::Transaction& recovery,bool wasDirty)
    {
        const bool commitUncertain=recovery.CommitStarted();
        recovery.Detach();
        auto* failed=volume_;
        volume_=nullptr;
        const int partition=failed->pnum;
        const bool remount=!commitUncertain&&failed->refs==1;
        const int fd=remount&&!ndifRecovery_?_dup(failed->fd):-1;
        b_quarantine_volume(failed,"Classic HFS write needs recovery. Close all views and reopen the image.");
        hfs_umount(failed);
        if(fd>=0)volume_=hfs_remount_for_recovery(fd,partition,session_);
        else if(remount&&ndifRecovery_){
            try{
                if(ClassicHfsRecovery::RecoverNdif(*ndifRecovery_,true))
                    volume_=hfs_mount(image_.data(),partition,O_RDWR);
            }catch(const HfsPlus::Error&){}
        }
        if(volume_){
            markedDirty_=wasDirty;
            hfs_take_rollback();
            hfs_note_rollback(1);
            return;
        }
        hfs_note_rollback(3);
        closeFailed_=true;
        throw HfsPlus::Error("Classic HFS write recovery is pending. Close all views and reopen the image.");
    }
public:
    bool CanMoveFrom(const Volume& source)const override{return dynamic_cast<const Classic*>(&source)&&source.ResourceKey()==ResourceKey();}
    void MoveHere(Volume& source,uint32_t id,uint32_t parent,const Entry& entry)override{
        Storage::Job job;
        MoveHereForJob(source,id,parent,entry,job);
    }
    void MoveHereForJob(Volume& source,uint32_t id,uint32_t parent,const Entry& entry,Storage::Job& job)override{
        auto* from=dynamic_cast<Classic*>(&source);
        Check(from&&CanMoveFrom(source));
        RenamePairFrom(*from,id,parent,entry.name,job);
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{
        Storage::Job job;
        RenameForJob(id,parent,name,job);
    }
    void RenameForJob(uint32_t id,uint32_t parent,const std::u16string& name,Storage::Job& job)override{
        RenamePairFrom(*this,id,parent,name,job);
    }
    void RenamePairFrom(Classic& source,uint32_t id,uint32_t parent,const std::u16string& name,Storage::Job& job){
        LegacyEngine::Scope engine(context_);
        if(id==2)throw HfsPlus::Error("Cannot rename or move the classic HFS volume root");
        if(job.cancelled())throw Storage::Cancelled();
        const auto before=source.Stat(id);
        auto pair=ClassicCopyArchive::RequireVerifiedPair(source,before,job);
        if(!pair){RenameFrom(source,id,parent,name);return;}
        if(before.parent==parent&&before.name==name)return;
        if(ClassicCopyArchive::ReservedName(name))
            throw HfsPlus::Error("This name is reserved for classic HFS metadata companions");
        const auto oldPath=source.Path(id);
        const auto rename=[&](bool durable){
            if(!durable)throw HfsPlus::Error("Renaming archived files requires classic HFS write recovery");
            if(job.cancelled())throw Storage::Cancelled();
            RenameFrom(source,id,parent,name);
            const auto moved=Id(source.Path(id),before.resolvedId);
            Entry companion;
            if(!source.FindChild(before.parent,pair->companion.name,companion)||
               companion.identity!=pair->companion.identity||companion.resolvedId!=pair->companion.resolvedId)
                throw HfsPlus::Error("The archive companion changed before the move");
            Remove(Id(source.Path(companion.id),companion.resolvedId));
            if(before.name!=name)pair->metadata.entry.name=name;
            ClassicCopyArchive::Save(*this,moved,pair->metadata,job);
        };
        try{
            if(copyTransaction_)rename(true);
            else RunWriteBatch(job,rename);
        }catch(...){
            source.paths_[id]=oldPath;
            throw;
        }
    }
    void Remove(uint32_t id)override{
        LegacyEngine::Scope engine(context_);
        Check(id!=2);
        DurableMutation([&](bool){RemoveEntry(id);});
    }
    bool RemoveTreeAtomically(uint32_t id,Storage::Job& job,bool deletionProgress)override{
        LegacyEngine::Scope engine(context_);
        Check(id!=2);
        if(job.cancelled())throw Storage::Cancelled();
        bool handled=false;
        DurableMutation([&](bool active){
            if(!active)return;
            ApplyRemovalPlan(*this,id,job,deletionProgress,[&](uint32_t entry){
                RemoveEntry(entry);
#ifdef HFV_TESTING
                hfv_hfs_recovery_checkpoint(6);
#endif
            });
            if(job.cancelled())throw Storage::Cancelled();
            Check(hfs_mark_session_clean(volume_)==0);
            markedDirty_=false;
            handled=true;
        });
        if(handled)Storage::OutcomeEvidence::NoteRetainedChange();
        return handled;
    }
private:
    void RenameFrom(Classic& source,uint32_t id,uint32_t parent,const std::u16string& name){
        LegacyEngine::Scope engine(context_);
        if(id==2)throw HfsPlus::Error("Cannot rename or move the classic HFS volume root");
        std::string from,to;
        {
            RootScope scope(volume_);
            from=source.Path(id);
            to=Path(parent);
            if(to.back()!=':')to+=':';
            to+=Encode(name);
            hfsdirent current{},destination{};
            Check(hfs_stat(volume_,from.data(),&current)==0);
            if(hfs_stat(volume_,to.data(),&destination)==0){
                if(current.cnid!=destination.cnid)
                    throw HfsPlus::Error("Rename or move destination already exists");
            }else if(errno!=ENOENT)Check(false);
        }
        DurableMutation([&](bool){
            RootScope scope(volume_);
            BeforeWrite();
            Check(hfs_rename(volume_,from.data(),to.data())==0);
        });
        source.paths_[id]=std::move(to);
    }
    void RemoveEntry(uint32_t id){
        RootScope scope(volume_);
        auto path=Path(id);
        const auto entry=Stat(id);
        BeforeWrite();
        Check((entry.directory?hfs_rmdir(volume_,path.data()):hfs_delete(volume_,path.data()))==0);
    }
};
}
std::vector<ClassicCatalogEntry> ReadClassicCatalog(Volume& volume,Storage::Job* job) {
    auto classic=dynamic_cast<Classic*>(&volume);
    if(!classic)throw HfsPlus::Error("A classic HFS catalog is required");
    return classic->Catalog(job);
}
std::vector<ClassicAncestor> ReadClassicAncestors(Volume& volume,uint32_t id) {
    auto classic=dynamic_cast<Classic*>(&volume);
    if(!classic)throw HfsPlus::Error("A classic HFS catalog is required");
    return classic->Ancestors(id);
}
ClassicMountInfo ReadClassicMountInfo(Volume& volume) {
    return dynamic_cast<Classic&>(volume).MountInfo();
}
void SelectClassicCatalogFolder(Volume& volume,const std::string& path) {
    dynamic_cast<Classic&>(volume).SelectCatalogFolder(path);
}
std::vector<ClassicAncestor> ResolveClassicFolders(Volume& volume,const std::string& relative,bool showHidden) {
    return dynamic_cast<Classic&>(volume).ResolveFolders(relative,showHidden);
}
void ReadClassicArtwork(Volume& volume,uint32_t catalogId,uint64_t offset,void* data,size_t size) {
    dynamic_cast<Classic&>(volume).ReadArtwork(catalogId,offset,data,size);
}
bool RecognizesClassicImage(HANDLE opened) {
    const int fd=AttachImage(opened,O_RDONLY);
    const bool recognized=hfs_recognizes_image_fd(fd)!=0;
    _close(fd);
    return recognized;
}
std::string ClassicCopyNameKey(const std::u16string& name)
{
    auto key=Encode(name);
    // Match the catalog collation used by libhfs.
    for(auto& byte:key)byte=char(hfs_charorder[static_cast<unsigned char>(byte)]);
    return key;
}

std::u16string ProjectClassicCopyName(const std::u16string& name,unsigned suffix)
{
    auto wide=Wide(name);
    for(auto& ch:wide)if(ch==L':'||ch==0)ch=L'_';
    const auto count=WideCharToMultiByte(10000,WC_NO_BEST_FIT_CHARS,wide.data(),int(wide.size()),
                                         nullptr,0,"_",nullptr);
    std::string bytes(count,'_');
    if(count&&!WideCharToMultiByte(10000,WC_NO_BEST_FIT_CHARS,wide.data(),int(wide.size()),
                                  bytes.data(),count,"_",nullptr))
        throw HfsPlus::Error("Cannot project the filename to MacRoman");
    if(bytes.empty())bytes="Untitled";
    const auto tail=suffix?"~"+std::to_string(suffix):std::string{};
    if(bytes.size()+tail.size()>HFS_MAX_FLEN)bytes.resize(HFS_MAX_FLEN-tail.size());
    return Decode(bytes+tail);
}

void ValidateClassicCopyNames(const std::vector<Entry>& entries)
{
    std::set<std::string> names;
    for(const auto& entry:entries){
        if(!names.insert(ClassicCopyNameKey(entry.name)).second)
            throw HfsPlus::Error("Source names collide on the classic HFS destination. Rename one before copying.");
    }
}

std::shared_ptr<Volume> OpenClassicImage(const std::wstring& path,bool writable,const std::string& expectedImage) {
    return TrackWorkerVolume(std::make_shared<Classic>(ClassicImagePath::Encode(path),":",writable,true,path,expectedImage));
}
std::shared_ptr<Volume> OpenClassicSource(std::shared_ptr<HfsPlus::Source> source,const std::string& key,bool write,bool stable){return TrackWorkerVolume(std::make_shared<Classic>(std::move(source),key,write,stable));}
std::shared_ptr<Volume> OpenClassic(const std::string& image,const std::string& path,bool write,const std::string& expectedImage){
    return TrackWorkerVolume(std::make_shared<Classic>(image,path,write,false,std::wstring{},expectedImage));
}
std::shared_ptr<Volume> OpenClassicNdif(std::shared_ptr<HfsPlus::Source> source,const std::string& key,
                                      const ClassicHfsRecovery::ImageRange& range,bool recovered){
    const bool write=source->Writable();
    return TrackWorkerVolume(std::make_shared<Classic>(std::move(source),key,write,true,range,recovered));
}
}
