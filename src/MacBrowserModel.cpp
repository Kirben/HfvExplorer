// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include "HfsPlusCd.h"
#include "BrowserPresentation.h"
#include <set>
#include <atomic>
#include <algorithm>

namespace MacBrowser {
std::wstring Wide(const std::u16string &s) { return {s.begin(), s.end()}; }
std::u16string U16(const std::wstring &s) { return {s.begin(), s.end()}; }
namespace {
void Check(bool ok, const char *message) { if (!ok) throw HfsPlus::Error(message); }
class Plus final : public Volume {
    std::unique_ptr<HfsPlus::Source> source_;
    std::unique_ptr<HfsPlus::Volume> volume_;
    bool writable_;std::atomic<bool> usable_{true};
    std::u16string name_;bool caseSensitive_=false,hfsx_=false,journaled_=false,canWrite_=false;
    mutable std::recursive_mutex mutex_;
    struct Call {VolumeCoord::BackendRead lease;std::unique_lock<std::recursive_mutex> lock;explicit Call(const Plus& v):lease(v.ResourceKey()),lock(v.mutex_,std::try_to_lock){if(!lock.owns_lock())throw VolumeCoord::Busy();}};
    std::string resourceKey_;
    template<class F> auto Mutate(F work) -> decltype(work()) {
        Call call(*this);if (!usable_) throw HfsPlus::Error("Close and reopen this volume before further access");
        try { return work(); }
        catch (...) {
            auto failure = std::current_exception();
            // A failed copy-on-write or journal operation invalidates the engine's
            // cached trees. Reopen/replay before allowing the UI to reuse them.
            try { volume_.reset(new HfsPlus::Volume(*source_, writable_)); }
            catch (...) { usable_ = false; }
            std::rethrow_exception(failure);
        }
    }
    void Ready() { if (!usable_) throw HfsPlus::Error("Close and reopen this volume before further access"); }
    static Entry Describe(const HfsPlus::Entry &in) {
        Entry e; e.id=in.id; e.parent=in.parent; e.resolvedId=in.resolvedId;
        e.name=in.name; e.directory=in.directory; e.protectedFile=in.protectedFile;
        e.compressed=in.compressed; e.hardLink=in.hardLink; e.readError=in.readError;
        e.finder=in.finder; e.created=in.created; e.modified=in.modified;e.datesValid=true;e.identity=std::to_string(in.id);
        for(unsigned f=0;f<2;++f)e.forks[f].size=in.forks[f].size;return e;
    }
public:
    Plus(std::unique_ptr<HfsPlus::Source> source, bool write,std::string key)
        : source_(std::move(source)), volume_(new HfsPlus::Volume(*source_, write)), writable_(write),resourceKey_(std::move(key)) {name_=volume_->Name();caseSensitive_=volume_->CaseSensitive();hfsx_=volume_->IsHfsX();journaled_=volume_->Journaled();canWrite_=volume_->Writable();}
    std::string ResourceKey()const override{return resourceKey_.empty()?Volume::ResourceKey():resourceKey_;}
    std::u16string Name() const override { return name_; }
    const char *Kind() const override { return hfsx_ ? "HFSX" : "HFS+"; }
    std::string Description()const override {
        return std::string(Kind())+(CaseSensitive()?" (case-sensitive)":" (case-insensitive)")+
            (journaled_?", journaled":", non-journaled");
    }
    bool Writable() const override { return usable_ && canWrite_; }
    void ReleaseMedia()override{std::unique_lock<std::recursive_mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())throw VolumeCoord::Busy();Check(!writable_,"Only read-only physical media can be released this way");usable_=false;volume_.reset();source_.reset();}
    void Reload() override {
        Call call(*this);Check(bool(source_),"Media was released; select the drive again");try { volume_.reset(new HfsPlus::Volume(*source_, writable_)); usable_=true; }
        catch (...) { usable_=false; throw; }
    }
    bool SupportsCompression()const override{return true;}
    void SetCompression(uint32_t id,bool compressed,std::function<void(uint64_t,uint64_t)> progress)override{Mutate([&]{volume_->SetCompression(id,compressed,progress);});}
    bool SupportsAttributes()const override{return true;}
    HfsPlus::Attributes GetAttributes(uint32_t id)override{Call call(*this);Ready();auto attributes=volume_->GetAttributes(id);if(volume_->Stat(id).compressed)attributes.erase(u"com.apple.decmpfs");return attributes;}
    void SetAttributes(uint32_t id,const HfsPlus::Attributes& a)override{Mutate([&]{volume_->SetAttributes(id,a);});}
    void ValidateEntry(const Entry& e)const override{Volume::ValidateEntry(e);HfsPlus::ValidateAttributes(e.attributes);}
    uint32_t MakeDirectoryWithAttributes(uint32_t parent,const Entry& e)override{ValidateEntry(e);return Mutate([&]{return volume_->MakeDirectory(parent,e.name,e.attributesKnown?&e.attributes:nullptr);});}
    bool CaseSensitive() const override { return caseSensitive_; }
    uint64_t FreeBytes() const override { Call call(*this);if(!usable_)throw HfsPlus::Error("Media was released; select the drive again");return volume_->FreeBytes(); }
    std::vector<Entry> List(uint32_t id) override {
        Call call(*this);Ready();std::vector<Entry> out;for(const auto& e:volume_->List(id))out.push_back(Describe(e));return out;
    }
    Entry Stat(uint32_t id) override { Call call(*this);Ready(); return Describe(volume_->Stat(id)); }
    void Read(uint32_t id, unsigned fork, uint64_t offset, void *p, size_t n) override {
        Call call(*this);Ready(); Check(volume_->Read(id, fork, offset, p, n) == n, "Short Macintosh fork read");
    }
    uint32_t MakeDirectory(uint32_t p, const std::u16string &s) override {
        return Mutate([&] { return volume_->MakeDirectory(p, s); });
    }
    uint32_t Put(uint32_t p, const Entry &e, Reader read, bool replace) override {
        std::array<uint32_t,2> dates{e.created,e.modified};
        return Mutate([&] { return volume_->Put(p, e.name, e.forks[0].size, e.forks[1].size, read, &e.finder, replace,e.datesValid?&dates:nullptr,e.attributesKnown?&e.attributes:nullptr); });
    }
    void Rename(uint32_t id, uint32_t p, const std::u16string &s) override {
        Mutate([&] { volume_->Rename(id, p, s); });
    }
    void Remove(uint32_t id) override { Mutate([&] { volume_->Remove(id); }); }
    void ValidateRemoval(uint32_t id)override{Call call(*this);Ready();volume_->ValidateRemoval(id);}
    bool CanMoveFrom(const Volume& source)const override{return source.ResourceKey()==ResourceKey();}
    void MoveHere(Volume& source,uint32_t id,uint32_t parent,const Entry& entry)override{Check(CanMoveFrom(source),"Move must stay on the same volume");Rename(id,parent,entry.name);}
};

class Mfs final : public Volume {
    std::unique_ptr<MfsImageSource> source_;
    MfsImage image_;
    mutable std::recursive_mutex mutex_;
    struct Call {VolumeCoord::BackendRead lease;std::unique_lock<std::recursive_mutex> lock;explicit Call(const Mfs& v):lease(v.ResourceKey()),lock(v.mutex_,std::try_to_lock){if(!lock.owns_lock())throw VolumeCoord::Busy();}};
    std::string resourceKey_;
    std::map<uint32_t,size_t> entries_;
    std::map<uint32_t,uint32_t> uiIds_;
    uint32_t nextUi_=3;
    std::atomic<bool> usable_{true},writable_{false};
    std::atomic<uint64_t> free_{0};
    std::shared_ptr<const std::u16string> name_;
    void Sync(){
        entries_.clear();for(size_t i=0;i<image_.Entries().size();++i){auto id=image_.Entries()[i].id;auto found=uiIds_.find(id);if(found==uiIds_.end()){Check(nextUi_!=UINT32_MAX,"MFS browser IDs exhausted");found=uiIds_.emplace(id,nextUi_++).first;}entries_[found->second]=i;}
        std::atomic_store(&name_,std::make_shared<const std::u16string>(U16(MfsDisplayName(image_.VolumeName()))));free_=image_.FreeBytes();writable_=image_.Writable();
    }
    Entry EntryAt(size_t i){
        const auto& in=image_.Entries().at(i);Entry e;e.id=uiIds_.at(in.id);e.resolvedId=in.id;e.parent=2;e.name=U16(MfsDisplayName(in.name));e.version=in.version;e.protectedFile=(in.flags&1)!=0;
        e.created=in.created;e.modified=in.modified;e.datesValid=true;memcpy(e.finder.data(),in.finderInfo,16);for(unsigned f=0;f<2;++f)e.forks[f].size=in.forks[f].length;
        e.identity=std::to_string(in.id)+":"+std::to_string(in.created)+":"+std::to_string(in.version);return e;
    }
    void Ready()const{Check(usable_,"Reopen the MFS image after a failed operation");}
    void Mutate(std::function<bool()> work){
        Call call(*this);Ready();Check(writable_,"MFS image is read-only");
        try{if(!work())throw HfsPlus::Error(image_.Error());Sync();}
        catch(...){auto failure=std::current_exception();if(image_.RecoveryRequired())usable_=false;else{try{if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());Sync();}catch(...){usable_=false;}}std::rethrow_exception(failure);}
    }
    static std::string Encode(const std::u16string& name,unsigned maximum=255){std::string encoded,error;if(!MfsImage::EncodeName(Wide(name),encoded,error,maximum))throw HfsPlus::Error(error);return encoded;}
public:
    explicit Mfs(std::unique_ptr<MfsImageSource> source,std::string key):source_(std::move(source)),resourceKey_(std::move(key)){if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());Sync();}
    std::string ResourceKey()const override{return resourceKey_;}
    std::u16string Name()const override{return *std::atomic_load(&name_);}
    const char* Kind()const override{return "MFS";}
    bool Writable()const override{return usable_&&writable_;}
    Capabilities Supports()const override{Capabilities c;c.create=c.rename=c.remove=Writable();c.unicodeNames=false;c.createFolders=false;return c;}
    bool CaseSensitive()const override{return false;}
    uint64_t FreeBytes()const override{return free_;}
    void Reload()override{Call call(*this);Ready();try{if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());Sync();}catch(...){usable_=false;throw;}}
    std::vector<Entry> List(uint32_t parent)override{Call call(*this);Ready();Check(parent==2,"MFS has no subdirectories");std::vector<Entry> out;for(auto& pair:entries_)out.push_back(EntryAt(pair.second));return out;}
    Entry Stat(uint32_t id)override{Call call(*this);Ready();if(id==2){Entry e;e.id=2;e.parent=1;e.directory=true;e.name=Name();return e;}return EntryAt(entries_.at(id));}
    bool FindChild(uint32_t parent,const std::u16string& name,Entry& found)override{
        Call call(*this);Ready();Check(parent==2,"MFS has no subdirectories");auto encoded=Encode(name);bool exists=false;
        for(auto& p:entries_)if(MfsImage::EqualNames(image_.Entries()[p.second].name,encoded)){Check(!exists,"Several MFS versions have this name; select a specific version");found=EntryAt(p.second);exists=true;}return exists;
    }
    bool FindTransferChild(uint32_t parent,const Entry& source,Entry& found)override{Call call(*this);Ready();Check(parent==2,"MFS has no subdirectories");auto encoded=Encode(source.name);for(auto& p:entries_){auto& e=image_.Entries()[p.second];if(e.version==source.version&&MfsImage::EqualNames(e.name,encoded)){found=EntryAt(p.second);return true;}}return false;}
    void Read(uint32_t id,unsigned fork,uint64_t at,void* p,size_t n)override{Call call(*this);Ready();size_t got=0;if(!image_.ReadFork(entries_.at(id),fork,at,p,n,got))throw HfsPlus::Error(image_.Error());Check(got==n,"Short MFS fork read");}
    void ValidateEntry(const Entry& e)const override{
        Check(!e.directory,"MFS is flat and cannot store folders");Check(e.attributes.empty(),"MFS cannot preserve extended attributes or ACLs");Encode(e.name);Check(e.version<=255,"Invalid MFS file version");
        Check(e.forks[0].size<=20*1024*1024&&e.forks[1].size<=20*1024*1024,"File exceeds the MFS 20 MiB volume limit");
        Check(std::all_of(e.finder.begin()+16,e.finder.end(),[](uint8_t b){return b==0;}),"MFS can store only the first 16 bytes of Finder information");
    }
    void ValidateRemoval(uint32_t id)override{auto e=Stat(id);Check(Writable()&&id!=2&&!e.protectedFile,"MFS file is locked or read-only");}
    uint32_t MakeDirectory(uint32_t,const std::u16string&)override{throw HfsPlus::Error("MFS is flat and has no subdirectories");}
    uint32_t Put(uint32_t parent,const Entry& e,Reader reader,bool overwrite)override{
        Check(parent==2,"MFS has no subdirectories");ValidateEntry(e);MfsEntry raw;raw.name=Encode(e.name);raw.version=BYTE(e.version);raw.flags=BYTE(0x80|(e.protectedFile?1:0));raw.created=e.created;raw.modified=e.modified;raw.datesValid=e.datesValid;memcpy(raw.finderInfo,e.finder.data(),16);for(unsigned f=0;f<2;++f)raw.forks[f].length=uint32_t(e.forks[f].size);uint32_t id=0;
        Mutate([&]{return image_.Put(raw,reader,overwrite,id);});Call call(*this);return uiIds_.at(id);
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{Check(parent==2&&id!=2,"MFS files remain in the volume root");auto encoded=Encode(name);Mutate([&]{return image_.Rename(image_.Entries().at(entries_.at(id)).id,encoded);});}
    void Remove(uint32_t id)override{Check(id!=2,"Cannot remove an MFS volume root");Mutate([&]{return image_.Remove(image_.Entries().at(entries_.at(id)).id);});}
    bool SupportsBasicMetadata()const override{return true;}
    void SetBasicMetadata(uint32_t id,const Entry& e)override{Check(id!=2&&!e.directory,"Select an MFS file");ValidateEntry(e);auto name=Encode(e.name);Mutate([&]{auto raw=image_.Entries().at(entries_.at(id));raw.name=name;raw.version=BYTE(e.version);raw.flags=BYTE((raw.flags&~1)|(e.protectedFile?1:0));raw.created=e.created;raw.modified=e.modified;memcpy(raw.finderInfo,e.finder.data(),16);return image_.SetMetadata(raw.id,raw);});}
};
struct Planned { Entry entry; std::vector<Planned> children; };
Planned Scan(Volume &v, uint32_t id, Storage::Job &job, Storage::Progress &p,
             std::set<uint32_t> &ancestors, unsigned depth) {
    if (job.cancelled()) throw Storage::Cancelled();
    Check(depth <= 128 && ancestors.insert(id).second, "Recursive folder link or excessive folder depth");
    Planned out; out.entry = v.Stat(id);
    Check(out.entry.readError.empty(), out.entry.readError.c_str());
    out.entry.attributes=v.GetAttributes(id);out.entry.attributesKnown=true;
    // Logical exports decode compression and resolve links rather than cloning
    // their private backing topology or compressed resource-fork storage.
    if(out.entry.compressed)out.entry.attributes.erase(u"com.apple.decmpfs");
    if(out.entry.hardLink)out.entry.attributes.erase(u"com.apple.system.hfs.firstlink");
    if (out.entry.directory) for (auto &child : v.List(id)) out.children.push_back(Scan(v, child.id, job, p, ancestors, depth + 1));
    else { v.Read(id,0,0,nullptr,0);if(out.entry.forks[1].size)v.Read(id,1,0,nullptr,0);
        ++p.totalFiles; Check(UINT64_MAX - p.totalBytes >= out.entry.forks[0].size,
        "Copy size overflow"); p.totalBytes += out.entry.forks[0].size;
        Check(UINT64_MAX - p.totalBytes >= out.entry.forks[1].size, "Copy size overflow");
        p.totalBytes += out.entry.forks[1].size; }
    ancestors.erase(id); job.progress(p); return out;
}
bool Transfer(Volume &src, const Planned &plan, Volume &dst, uint32_t parent,
              Storage::Job &job, Storage::Progress &progress) {
    if (job.cancelled()) throw Storage::Cancelled();
    const auto &entry = plan.entry; Entry existing; bool found = false;
    found=dst.FindTransferChild(parent,entry,existing);
    if (entry.directory) {
        Check(!found || existing.directory, "A file blocks the destination folder");
        uint32_t id = found ? existing.id : dst.MakeDirectoryWithAttributes(parent, entry);
        bool complete = true;
        for (const auto &child : plan.children) if (!Transfer(src, child, dst, id, job, progress)) complete = false;
        return complete;
    }
    Check(!found || !existing.directory, "A folder blocks the destination file");
    if (found) {
        auto choice = job.replace(BrowserPresentation::Utf8(entry.name));
        if (choice == Storage::ConflictChoice::Cancel) throw Storage::Cancelled();
        if (choice == Storage::ConflictChoice::Skip) { ++progress.skipped;progress.processed+=entry.forks[0].size+entry.forks[1].size;job.progress(progress);return false; }
    }
    progress.path = BrowserPresentation::Utf8(entry.name); progress.fileBytes = 0;
    progress.fileTotal = entry.forks[0].size + entry.forks[1].size; job.progress(progress);
    uint64_t readThrough[2]={};
    dst.Put(parent, entry, [&](unsigned fork, uint64_t offset, void *p, size_t n) {
        if (job.cancelled()) throw Storage::Cancelled();
        src.Read(entry.id, fork, offset, p, n);
        if(job.cancelled())throw Storage::Cancelled();
        auto end=offset+n,added=end>readThrough[fork]?end-readThrough[fork]:0;readThrough[fork]=(std::max)(readThrough[fork],end);progress.fileBytes += added; progress.bytes += added;progress.processed+=added;job.progress(progress);
    }, found);
    ++progress.files; job.progress(progress); return true;
}
}
std::shared_ptr<Volume> FromHfsPlus(std::unique_ptr<HfsPlus::Source> s, bool write,std::string key) {
    return std::make_shared<Plus>(std::move(s), write,std::move(key));
}
std::shared_ptr<Volume> OpenImage(const std::wstring &path) {
    DWORD attributes=GetFileAttributesW(path.c_str());if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))return {};
    auto key=VolumeCoord::ImageKey(path);return VolumeCoord::SharedImage(key,[&]()->std::shared_ptr<Volume>{
    auto source = HfsPlus::OpenImage(path.c_str(), false);
    if (HfsPlus::Volume::Recognizes(*source)) {
        source.reset();
        try { return FromHfsPlus(HfsPlus::OpenImage(path.c_str(), true), true,key); }
        catch (...) { return FromHfsPlus(HfsPlus::OpenImage(path.c_str(), false), false,key); }
    }
    source.reset(); std::unique_ptr<MfsImageSource> mfs(new MfsImageSource); std::string error;
    if (!mfs->Open(path.c_str(), error)) throw HfsPlus::Error(error);
    if (!MfsImage::Recognizes(*mfs)) return {};
    mfs.reset(new MfsImageSource);
    if(!mfs->Open(path.c_str(),error,true)){mfs.reset(new MfsImageSource);if(!mfs->Open(path.c_str(),error))throw HfsPlus::Error(error);}
    return std::make_shared<Mfs>(std::move(mfs),key);
    });
}
std::shared_ptr<Volume> OpenCd(int drive, std::shared_ptr<::MediaImageSource> media) {
    auto source = HfsPlus::OpenCd(drive, std::move(media));
    if (!HfsPlus::Volume::Recognizes(*source)) return {};
    try { return FromHfsPlus(std::move(source), false,VolumeCoord::DeviceKey(drive)); }
    catch (const std::exception &e) { throw DetectedVolumeError(e.what()); }
}
std::shared_ptr<Volume> OpenCdRoot(const char *root,
    std::function<UINT(const char *)> driveType,
    std::function<std::shared_ptr<Volume>(int)> open) {
    int drive=HfsPlus::CdDriveFromRoot(root); if(drive<0)return {};
    char path[]={char('A'+drive),':',char(92),0};
    if((driveType?driveType(path):GetDriveTypeA(path))!=DRIVE_CDROM)return {};
    try { return open?open(drive):OpenCd(drive); }
    catch(const DetectedVolumeError&){throw;}
    catch(const std::exception&){return {};}
}
void RemoveTree(Volume &v, uint32_t id, Storage::Job &job,bool deletionProgress) {
    Check(id != 2 && v.Writable(), "Cannot delete the volume root or a read-only item");
    std::set<uint32_t> parents; Storage::Progress p;if(deletionProgress){p.phase="Deleting";p.utf8=true;}auto plan = Scan(v, id, job, p, parents, 0);
    std::function<void(const Planned &)> remove = [&](const Planned &n) {
        if (job.cancelled()) throw Storage::Cancelled();
        // A directory hard link is a link, not ownership of its backing children.
        Check(!n.entry.hardLink, "Deleting hard links is not supported");
        for (const auto &c : n.children) remove(c);
        if(job.cancelled())throw Storage::Cancelled();
        if(deletionProgress){p.path=BrowserPresentation::Utf8(n.entry.name);p.scanning=false;job.progress(p);}
        v.Remove(n.entry.id);
        if(deletionProgress){++p.files;job.progress(p);}
    };
    std::function<void(const Planned &)> validate=[&](const Planned& n){if(job.cancelled())throw Storage::Cancelled();v.ValidateRemoval(n.entry.id);for(const auto& c:n.children)validate(c);};
    validate(plan);
    remove(plan);
}
std::string Snapshot(Volume& v,uint32_t id,Storage::Job& job,bool requireRemovable){
    uint64_t hash=14695981039346656037ULL,count=0;std::set<uint32_t> ancestors;
    auto bytes=[&](const void* p,size_t n){auto b=static_cast<const uint8_t*>(p);while(n--){hash^=*b++;hash*=1099511628211ULL;}};
    std::function<void(uint32_t,unsigned)> walk=[&](uint32_t at,unsigned depth){
        if(job.cancelled())throw Storage::Cancelled();Check(depth<=128&&ancestors.insert(at).second,"Cyclic source folder");
        auto e=v.Stat(at);Check(!e.identity.empty(),"Cannot verify source identity; source items were retained");++count;
        if(requireRemovable)v.ValidateRemoval(at);
        auto attributes=v.GetAttributes(at);for(const auto& a:attributes){auto size=a.second.size();bytes(a.first.data(),a.first.size()*2);bytes(&size,sizeof(size));bytes(a.second.data(),a.second.size());}
        bytes(e.identity.data(),e.identity.size());bytes(e.name.data(),e.name.size()*2);
        bytes(&e.created,sizeof(e.created));bytes(&e.modified,sizeof(e.modified));bytes(e.finder.data(),e.finder.size());
        bytes(&e.forks[0].size,sizeof(uint64_t));bytes(&e.forks[1].size,sizeof(uint64_t));
        if(e.directory){auto children=v.List(at);std::sort(children.begin(),children.end(),[](const Entry& a,const Entry& b){return a.name==b.name?a.identity<b.identity:a.name<b.name;});for(auto& child:children)walk(child.id,depth+1);}
        else{v.Read(at,0,0,nullptr,0);if(e.forks[1].size)v.Read(at,1,0,nullptr,0);}
        ancestors.erase(at);
    };
    walk(id,0);return std::to_string(count)+":"+std::to_string(hash);
}
namespace {
void ValidateCopy(Volume &src,uint32_t id,Volume &dst,uint32_t parent){
    Check(dst.Writable(),"The destination is read-only");Check(id!=2,"Select files or folders rather than a volume root");
    src.ValidateCopyTo(id,dst,parent);
    if(&src==&dst){std::set<uint32_t> visited;for(uint32_t p=parent;p!=2;){
        Check(p!=id&&visited.insert(p).second,"Cannot copy a folder into itself or its descendants");p=dst.Stat(p).parent;}
        Check(src.Stat(id).parent!=parent,"Source and destination are the same folder");}
}
void ValidatePlan(Volume& dst,const Planned& plan){dst.ValidateEntry(plan.entry);for(const auto& child:plan.children)ValidatePlan(dst,child);}
}
bool CopyMany(const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>>& sources,Volume& dst,uint32_t parent,Storage::Job& job){
    Check(dst.Writable(),"The destination is read-only");Storage::Progress progress;progress.fileCopy=progress.utf8=true;progress.phase="Scanning selected files";
    std::vector<Planned> plans;
    for(const auto& source:sources){ValidateCopy(*source.first,source.second,dst,parent);std::set<uint32_t> ancestors;
        plans.push_back(Scan(*source.first,source.second,job,progress,ancestors,0));ValidatePlan(dst,plans.back());}
    progress.scanning=false;progress.byteTransfer=true;progress.phase="Copying";job.progress(progress);bool complete=true;
    for(size_t i=0;i<plans.size();++i)if(!Transfer(*sources[i].first,plans[i],dst,parent,job,progress))complete=false;
    return complete;
}
bool Copy(Volume &src,uint32_t id,Volume &dst,uint32_t parent,Storage::Job& job,bool move){
    Check(!move||src.Writable(),"The source is read-only");
    // The public single-item helper borrows its caller-owned adapter.
    std::shared_ptr<Volume> borrowed(&src,[](Volume*){});
    bool complete=CopyMany({{borrowed,id}},dst,parent,job);
    if(move&&complete)RemoveTree(src,id,job);return complete;
}
}
