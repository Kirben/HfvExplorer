// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacBrowserModel.h"
#include "StorageHandoff.h"
#include "VolumeSession.h"
#include "VolumeCoordinator.h"
#include "HfsPlusCd.h"
#include "OpticalVolume.h"
#include "CueImage.h"
#include "HfsPlusDisk.h"
#include "DiskImageOpen.h"
#include "NdifImage.h"
#include "ApfsVolume.h"
#include "DiskPartitions.h"
#include "BrowserPresentation.h"
#include "HfsPlusTime.h"
#include <set>
#include <atomic>
#include <algorithm>

namespace MacBrowser {
std::wstring Wide(const std::u16string &s) { return {s.begin(), s.end()}; }
std::u16string U16(const std::wstring &s) { return {s.begin(), s.end()}; }
namespace {
void Check(bool ok, const char *message) { if (!ok) throw HfsPlus::Error(message); }
class Plus final : public Volume, public WorkerEndpoint {
    std::unique_ptr<HfsPlus::Source> source_;
    std::unique_ptr<HfsPlus::Volume> volume_;
    std::atomic<bool> writable_;std::atomic<bool> usable_{true};
    const bool processLocal_;
    std::u16string name_;bool caseSensitive_=false,hfsx_=false,journaled_=false,external_=false,expanded_=false;std::atomic<bool> canWrite_{false};
    mutable std::recursive_mutex mutex_;
    struct Call {VolumeCoord::BackendRead lease;std::unique_lock<std::recursive_mutex> lock;explicit Call(const Plus& v):lease(v.ResourceKey()),lock(v.mutex_,std::try_to_lock){if(!lock.owns_lock())throw VolumeCoord::Busy();}};
    std::wstring externalJournal_;
    std::string resourceKey_;std::unique_ptr<VolumeCoord::ImageDependency> journalDependency_;
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
        e.compressed=in.compressed; e.hardLink=in.hardLink;e.linkCount=in.linkCount; e.readError=in.readError;
        e.finder=in.finder; e.created=in.created; e.modified=in.modified;e.datesValid=true;e.identity=std::to_string(in.id);
        for(unsigned f=0;f<2;++f)e.forks[f].size=in.forks[f].size;return e;
    }
public:
    Plus(std::unique_ptr<HfsPlus::Source> source, bool write,std::string key,std::wstring externalJournal)
        : source_(std::move(source)), volume_(new HfsPlus::Volume(*source_, write)), writable_(write),processLocal_(source_->ProcessLocal()),resourceKey_(std::move(key)),externalJournal_(externalJournal) {name_=volume_->Name();caseSensitive_=volume_->CaseSensitive();hfsx_=volume_->IsHfsX();journaled_=volume_->Journaled();external_=volume_->ExternalJournal();expanded_=volume_->ExpandedTimes();canWrite_=volume_->Writable();if(!externalJournal.empty()&&volume_->ExternalJournal())journalDependency_.reset(new VolumeCoord::ImageDependency(ResourceKey(),externalJournal));}
    WorkerExecution Execution()const override{WorkerExecution result;result.stableCatalogIds=true;result.processLocal=processLocal_;return result;}
    WorkerOrigin WorkerDescription()const override{WorkerOrigin out;auto files=source_->WorkerFiles();if(!files.empty()){out.kind=WorkerOrigin::Image;out.path=files.front().first;out.journal=externalJournal_;}else if(source_->MediaDrive()>=0){out.kind=WorkerOrigin::Cd;out.drive=source_->MediaDrive();uint32_t generation=0;out.generationKnown=source_->MediaGeneration(generation);out.generation=generation;}out.writable=writable_;return out;}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return source_->WorkerFiles();}
    void SuspendWorker()override{Call call(*this);usable_=false;volume_.reset();}
    void ResumeWorker()override{Reload();}
    void WorkerUncleanExit()override{if(!journaled_){writable_=false;canWrite_=false;}}
    std::string ResourceKey()const override{return resourceKey_.empty()?Volume::ResourceKey():resourceKey_;}
    std::u16string Name() const override { return name_; }
    const char *Kind() const override { return hfsx_ ? "HFSX" : "HFS+"; }
    std::string Description()const override {
        return std::string(Kind())+(CaseSensitive()?" (case-sensitive)":" (case-insensitive)")+
            (journaled_?(external_?", external journal":", internal journal"):", non-journaled")+(expanded_?", expanded timestamps":"");
    }
    bool Writable() const override { return usable_ && canWrite_; }
    bool Journaled() const override { return journaled_; }
    void ValidateMedia()override{Call call(*this);Ready();source_->CheckMedia();}
    void ReleaseMedia()override{std::unique_lock<std::recursive_mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())throw VolumeCoord::Busy();Check(!writable_,"Only read-only physical media can be released this way");usable_=false;volume_.reset();source_.reset();}
    void Reload() override {
        Call call(*this);Check(bool(source_),"Media was released; select the drive again");try { volume_.reset(new HfsPlus::Volume(*source_, writable_)); usable_=true; }
        catch (...) { usable_=false; throw; }
    }
    bool SupportsHardLinks()const override{return true;}
    uint32_t MakeHardLink(uint32_t source,uint32_t parent,const std::u16string& name,bool replace=false)override{return Mutate([&]{return volume_->MakeHardLink(source,parent,name,replace);});}
    bool SupportsCompression()const override{return true;}
    void SetCompressionMethod(uint32_t id,HfsPlus::Volume::CompressionMode method,std::function<void(uint64_t,uint64_t)> progress)override{Mutate([&]{volume_->SetCompression(id,method,progress);});}
    void SetCompression(uint32_t id,bool compressed,std::function<void(uint64_t,uint64_t)> progress)override{Mutate([&]{volume_->SetCompression(id,compressed,progress);});}
    bool SupportsAttributes()const override{return true;}
    HfsPlus::Attributes GetAttributes(uint32_t id)override{Call call(*this);Ready();auto attributes=volume_->GetAttributes(id);attributes.erase(u"com.apple.system.hfs.firstlink");if(volume_->Stat(id).compressed)attributes.erase(u"com.apple.decmpfs");return attributes;}
    void SetAttributes(uint32_t id,const HfsPlus::Attributes& a)override{Mutate([&]{volume_->SetAttributes(id,a);});}
    void ValidateEntry(const Entry& e)const override{Volume::ValidateEntry(e);if(e.datesValid){(void)HfsPlus::Time::Encode(e.created,expanded_);(void)HfsPlus::Time::Encode(e.modified,expanded_);}HfsPlus::ValidateAttributes(e.attributes);}
    uint32_t MakeDirectoryWithAttributes(uint32_t parent,const Entry& e)override{ValidateEntry(e);return Mutate([&]{return volume_->MakeDirectory(parent,e.name);});}
    void FinalizeDirectory(uint32_t id,const Entry& desired,const Entry& expected,bool merge)override{
        Mutate([&]{auto now=volume_->Stat(id);Check(now.directory&&now.id==expected.id&&now.parent==expected.parent&&now.name==expected.name,"Destination folder changed during copying");std::array<uint64_t,2> dates{desired.created,desired.modified};volume_->SetDirectoryMetadata(id,merge?nullptr:&desired.finder,desired.datesValid?&dates:nullptr,!merge&&desired.attributesKnown?&desired.attributes:nullptr);});
    }
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
        std::array<uint64_t,2> dates{e.created,e.modified};
        return Mutate([&] { return volume_->Put(p, e.name, e.forks[0].size, e.forks[1].size, read, &e.finder, replace,nullptr,e.attributesKnown?&e.attributes:nullptr,HfsPlus::Volume::CompressionMode::Preserve,e.datesValid?&dates:nullptr); });
    }
    void Rename(uint32_t id, uint32_t p, const std::u16string &s) override {
        Mutate([&] { volume_->Rename(id, p, s); });
    }
    void Remove(uint32_t id) override { Mutate([&] { volume_->Remove(id); }); }
    void ValidateCopyTo(uint32_t id,Volume& target,uint32_t parent)override{if(target.ResourceKey()!=ResourceKey())return;Call call(*this);Ready();volume_->ValidateCopyTo(id,parent);}
    void ValidateRemoval(uint32_t id)override{Call call(*this);Ready();volume_->ValidateRemoval(id);}
    bool CanMoveFrom(const Volume& source)const override{return source.ResourceKey()==ResourceKey();}
    void MoveHere(Volume& source,uint32_t id,uint32_t parent,const Entry& entry)override{Check(CanMoveFrom(source),"Move must stay on the same volume");Rename(id,parent,entry.name);}
};

class Mfs final : public Volume, public WorkerEndpoint {
    std::unique_ptr<MfsSource> source_;
    MfsImage image_;
    mutable std::recursive_mutex mutex_;
    struct Call {VolumeCoord::BackendRead lease;std::unique_lock<std::recursive_mutex> lock;explicit Call(const Mfs& v):lease(v.ResourceKey()),lock(v.mutex_,std::try_to_lock){if(!lock.owns_lock())throw VolumeCoord::Busy();}};
    std::string resourceKey_;
    std::map<uint32_t,size_t> entries_;
    std::shared_ptr<MfsCatalogIds> ids_;
    std::atomic<bool> usable_{true},writable_{false};
    std::atomic<uint64_t> free_{0};
    std::shared_ptr<const std::u16string> name_;
    void Sync(){
        entries_.clear();for(size_t i=0;i<image_.Entries().size();++i){auto id=image_.Entries()[i].id;auto found=ids_->byFile.find(id);if(found==ids_->byFile.end()){Check(ids_->next!=UINT32_MAX,"MFS browser IDs exhausted");found=ids_->byFile.emplace(id,ids_->next++).first;}entries_[found->second]=i;}
        std::atomic_store(&name_,std::make_shared<const std::u16string>(U16(MfsDisplayName(image_.VolumeName()))));free_=image_.FreeBytes();writable_=image_.Writable();
    }
    Entry EntryAt(size_t i){
        const auto& in=image_.Entries().at(i);Entry e;e.id=ids_->byFile.at(in.id);e.resolvedId=in.id;e.parent=2;e.name=U16(MfsDisplayName(in.name));e.version=in.version;e.protectedFile=(in.flags&1)!=0;
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
    WorkerExecution Execution()const override{WorkerExecution result;result.processLocal=source_->ProcessLocal();return result;}
    explicit Mfs(std::unique_ptr<MfsSource> source,std::string key,std::shared_ptr<MfsCatalogIds> ids):source_(std::move(source)),resourceKey_(std::move(key)),ids_(ids?std::move(ids):std::make_shared<MfsCatalogIds>()){if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());Sync();}
    WorkerOrigin WorkerDescription()const override{WorkerOrigin out;auto files=source_->WorkerFiles();if(!files.empty()){out.kind=WorkerOrigin::Image;out.path=files.front().first;}out.writable=source_->Writable();return out;}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return source_->WorkerFiles();}
    void WorkerUncleanExit()override{source_->DisableWrites();writable_=false;}
    void SuspendWorker()override{Call call(*this);usable_=false;image_=MfsImage();}
    void ResumeWorker()override{Call call(*this);if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());Sync();usable_=true;}
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
        Check(!e.datesValid||(e.created<=UINT32_MAX&&e.modified<=UINT32_MAX),"MFS cannot preserve dates after February 2040");Check(!e.directory,"MFS is flat and cannot store folders");Check(e.attributes.empty(),"MFS cannot preserve extended attributes or ACLs");Encode(e.name);Check(e.version<=255,"Invalid MFS file version");
        Check(e.forks[0].size<=20*1024*1024&&e.forks[1].size<=20*1024*1024,"File exceeds the MFS 20 MiB volume limit");
        Check(std::all_of(e.finder.begin()+16,e.finder.end(),[](uint8_t b){return b==0;}),"MFS can store only the first 16 bytes of Finder information");
    }
    void ValidateRemoval(uint32_t id)override{auto e=Stat(id);Check(Writable()&&id!=2&&!e.protectedFile,"MFS file is locked or read-only");}
    uint32_t MakeDirectory(uint32_t,const std::u16string&)override{throw HfsPlus::Error("MFS is flat and has no subdirectories");}
    uint32_t Put(uint32_t parent,const Entry& e,Reader reader,bool overwrite)override{
        Check(parent==2,"MFS has no subdirectories");ValidateEntry(e);MfsEntry raw;raw.name=Encode(e.name);raw.version=BYTE(e.version);raw.flags=BYTE(0x80|(e.protectedFile?1:0));raw.created=uint32_t(e.created);raw.modified=uint32_t(e.modified);raw.datesValid=e.datesValid;memcpy(raw.finderInfo,e.finder.data(),16);for(unsigned f=0;f<2;++f)raw.forks[f].length=uint32_t(e.forks[f].size);uint32_t id=0;
        Mutate([&]{return image_.Put(raw,reader,overwrite,id);});Call call(*this);return ids_->byFile.at(id);
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{Check(parent==2&&id!=2,"MFS files remain in the volume root");auto encoded=Encode(name);Mutate([&]{return image_.Rename(image_.Entries().at(entries_.at(id)).id,encoded);});}
    void Remove(uint32_t id)override{Check(id!=2,"Cannot remove an MFS volume root");Mutate([&]{return image_.Remove(image_.Entries().at(entries_.at(id)).id);});}
    bool SupportsBasicMetadata()const override{return true;}
    void SetBasicMetadata(uint32_t id,const Entry& e)override{Check(id!=2&&!e.directory,"Select an MFS file");ValidateEntry(e);auto name=Encode(e.name);Mutate([&]{auto raw=image_.Entries().at(entries_.at(id));raw.name=name;raw.version=BYTE(e.version);raw.flags=BYTE((raw.flags&~1)|(e.protectedFile?1:0));raw.created=uint32_t(e.created);raw.modified=uint32_t(e.modified);memcpy(raw.finderInfo,e.finder.data(),16);return image_.SetMetadata(raw.id,raw);});}
};
struct Planned { Entry entry; std::vector<Planned> children; Reader reader; };
Planned Scan(Volume &v, uint32_t id, Storage::Job &job, Storage::Progress &p,
             std::set<uint32_t> &ancestors, unsigned depth,bool removal,bool reportProgress,const TransferAdapter& adapter={}) {
    if (job.cancelled()) throw Storage::Cancelled();
    Planned out; out.entry = v.Stat(id);auto identity=out.entry.resolvedId?out.entry.resolvedId:id;
    Check(depth <= 128 && ancestors.insert(identity).second, "Recursive folder link or excessive folder depth");
    Check(out.entry.readError.empty(), out.entry.readError.c_str());
    out.entry.attributes=v.GetAttributes(id);out.entry.attributesKnown=true;
    // Logical exports decode compression and resolve links rather than cloning
    // their private backing topology or compressed resource-fork storage.
    if(out.entry.compressed)out.entry.attributes.erase(u"com.apple.decmpfs");
    if(out.entry.hardLink)out.entry.attributes.erase(u"com.apple.system.hfs.firstlink");
    if (out.entry.directory){
        if (adapter) out.reader=adapter(v,out.entry,job,depth==0);if(!removal||!out.entry.hardLink||out.entry.linkCount<=1)for(auto &child:v.List(id))out.children.push_back(Scan(v,child.id,job,p,ancestors,depth+1,removal,reportProgress,adapter));}
    else { if(!removal){v.Read(id,0,0,nullptr,0);if(out.entry.forks[1].size)v.Read(id,1,0,nullptr,0);}
        if (adapter) out.reader=adapter(v,out.entry,job,depth==0);
        ++p.totalFiles; Check(UINT64_MAX - p.totalBytes >= out.entry.forks[0].size,
        "Copy size overflow"); p.totalBytes += out.entry.forks[0].size;
        Check(UINT64_MAX - p.totalBytes >= out.entry.forks[1].size, "Copy size overflow");
        p.totalBytes += out.entry.forks[1].size; }
    ancestors.erase(identity);
    // Cut cleanup scans quietly so the transfer's completed totals remain visible.
    if(reportProgress)job.progress(p);
    return out;
}
using CopiedLinks=std::map<std::pair<std::string,uint32_t>,std::pair<uint32_t,std::u16string>>;
void LinkedProgress(const Planned& plan,Storage::Progress& p){if(plan.entry.directory){for(auto& child:plan.children)LinkedProgress(child,p);}else{++p.files;p.processed+=plan.entry.forks[0].size+plan.entry.forks[1].size;}}
bool Transfer(Volume &src, const Planned &plan, Volume &dst, uint32_t parent,
              Storage::Job &job, Storage::Progress &progress,CopiedLinks& copies,ForkSource route) {
    if (job.cancelled()) throw Storage::Cancelled();
    const auto &entry = plan.entry; Entry existing; bool found = false;
    found=dst.FindTransferChild(parent,entry,existing);
    bool track=entry.hardLink&&entry.linkCount>1&&dst.SupportsHardLinks();auto key=std::make_pair(src.ResourceKey(),entry.resolvedId);auto prior=copies.find(key);
    if(track&&prior!=copies.end()&&entry.directory&&!found){Entry original;Check(dst.FindChild(prior->second.first,prior->second.second,original),"Copied hard-link source disappeared");dst.MakeHardLink(original.id,parent,entry.name);LinkedProgress(plan,progress);job.progress(progress);return true;}
    if (entry.directory) {
        Check(!found || existing.directory, "A file blocks the destination folder");
        uint32_t id = found ? existing.id : dst.MakeDirectoryWithAttributes(parent, entry);
        if(!found)Storage::OutcomeEvidence::NoteRetainedChange();
        auto expected=dst.Stat(id);bool complete=true;std::exception_ptr failure;
        try{for(const auto& child:plan.children)if(!Transfer(src,child,dst,id,job,progress,copies,route))complete=false;}catch(...){failure=std::current_exception();}
        // Existing directory metadata belongs to the destination. In particular,
        // child creates/renames can change its dates even on Skip/cancellation.
        try{if(found)dst.FinalizeDirectory(id,existing,expected,true);else if(!failure&&complete)dst.FinalizeDirectory(id,entry,expected,false);}
        catch(const std::exception& restore){if(failure){std::string original="Copy failed";try{std::rethrow_exception(failure);}catch(const std::exception& e){original=e.what();}catch(...){}throw HfsPlus::Error(original+"; folder metadata restoration failed: "+restore.what());}throw;}
        if(failure)std::rethrow_exception(failure);if(complete&&track)copies.emplace(key,std::make_pair(parent,dst.Stat(id).name));return complete;
    }
    Check(!found || !existing.directory, "A folder blocks the destination file");
    if (found) {
        auto choice = job.replace(BrowserPresentation::Utf8(entry.name)+(existing.hardLink?" (shared by "+std::to_string(existing.linkCount)+" hard links)":""));
        if (choice == Storage::ConflictChoice::Cancel) throw Storage::Cancelled();
        if (choice == Storage::ConflictChoice::Skip) { ++progress.skipped;progress.processed+=entry.forks[0].size+entry.forks[1].size;job.progress(progress);return false; }
    }
    if(found){auto now=dst.Stat(existing.id);Check(now.identity==existing.identity&&now.name==existing.name&&now.directory==existing.directory&&now.modified==existing.modified&&now.forks[0].size==existing.forks[0].size&&now.forks[1].size==existing.forks[1].size,"Destination changed during overwrite confirmation; source retained");}
    progress.path = BrowserPresentation::Utf8(entry.name); progress.fileBytes = 0;
    progress.fileTotal = entry.forks[0].size + entry.forks[1].size; job.progress(progress);
    if(track&&prior!=copies.end()){Entry original;Check(dst.FindChild(prior->second.first,prior->second.second,original),"Copied hard-link source disappeared");dst.MakeHardLink(original.id,parent,entry.name,found);LinkedProgress(plan,progress);progress.fileBytes=progress.fileTotal;job.progress(progress);return true;}
    uint64_t readThrough[2]={};
    auto outgoing=entry;outgoing.expectedDestinationIdentity=found?existing.identity:std::string{};
    const auto advance=[&](unsigned fork,uint64_t offset,size_t count){
        if(job.cancelled())throw Storage::Cancelled();
        const auto end=offset+count;
        const auto added=end>readThrough[fork]?end-readThrough[fork]:0;
        readThrough[fork]=(std::max)(readThrough[fork],end);
        progress.fileBytes+=added;
        progress.bytes+=added;
        progress.processed+=added;
        job.progress(progress);
        if(job.cancelled())throw Storage::Cancelled();
    };
    uint32_t published=0;
    if(route==ForkSource::MountedWorker)
        published=VolumeSession::PutFromMountedSource(dst,parent,outgoing,found,advance);
    else {
        Reader reader=[&](unsigned fork,uint64_t offset,void* p,size_t count){
            if(job.cancelled())throw Storage::Cancelled();
            if(plan.reader)plan.reader(fork,offset,p,count);
            else src.Read(entry.id,fork,offset,p,count);
            advance(fork,offset,count);
        };
        auto hostPath=plan.reader?std::wstring{}:src.HostPath(entry.id);
        if(hostPath.empty())published=dst.Put(parent,outgoing,std::move(reader),found);
        else published=dst.PutFromHost(parent,outgoing,std::move(reader),found,hostPath,job);
    }
    if(track)copies.emplace(key,std::make_pair(parent,dst.Stat(published).name));++progress.files; job.progress(progress); return true;
}
}
std::shared_ptr<Volume> FromMfs(std::unique_ptr<MfsSource> source,const std::string& key,std::shared_ptr<MfsCatalogIds> ids){return TrackWorkerVolume(std::make_shared<Mfs>(std::move(source),key,std::move(ids)));}
std::shared_ptr<Volume> FromHfsPlus(std::unique_ptr<HfsPlus::Source> s, bool write,std::string key,std::wstring externalJournal) {
    return TrackWorkerVolume(std::make_shared<Plus>(std::move(s), write,std::move(key),std::move(externalJournal)));
}
std::shared_ptr<Volume> OpenMfsImage(const std::wstring& path,bool writable) {
    auto source=std::make_unique<MfsImageSource>();
    std::string error;
    if(!source->Open(path.c_str(),error,writable) &&
       (!writable || !source->Open(path.c_str(),error,false)))throw HfsPlus::Error(error);
    return FromMfs(std::move(source),VolumeCoord::ImageKey(path));
}
namespace {
class MfsDiskSource final : public MfsSource {
    std::shared_ptr<HfsPlus::Source> source_;
public:
    bool readOnly_;
    explicit MfsDiskSource(std::shared_ptr<HfsPlus::Source> source,bool readOnly=true)
        :source_(std::move(source)),readOnly_(readOnly){}
    uint64_t Length()const override{return source_->Size();}
    bool Writable()const override{return !readOnly_&&source_->Writable();}
    void DisableWrites()override{readOnly_=true;}
    bool WriteAt(uint64_t at,const void* bytes,size_t count)override{
        if(!Writable())return false;
        try{source_->Write(at,bytes,count);return true;}catch(const HfsPlus::Error&){return false;}
    }
    bool Flush()override{
        try{source_->Flush();return true;}catch(const HfsPlus::Error&){return false;}
    }
    bool ProcessLocal()const override{return source_->ProcessLocal();}
    bool ReadAt(uint64_t at,void* bytes,size_t count)override{source_->Read(at,bytes,count);return true;}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return source_->WorkerFiles();}
};

class DiskSlice final : public HfsPlus::Source {
    std::shared_ptr<HfsPlus::Source> disk_;
    const uint64_t start_,size_;
    const bool write_;
public:
    DiskSlice(std::shared_ptr<HfsPlus::Source> disk,uint64_t start,uint64_t size,bool write=false)
        :disk_(std::move(disk)),start_(start),size_(size),write_(write){}
    uint64_t Size()const override{return size_;}
    bool Writable()const override{return write_&&disk_->Writable();}
    bool ProcessLocal()const override{return disk_->ProcessLocal();}
    void CheckMedia()override{disk_->CheckMedia();}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return disk_->WorkerFiles();}
    void Read(uint64_t at,void* bytes,size_t count)override{
        Check(HfsPlus::Disk::Range(at,count,size_),"Read outside the disk partition");
        disk_->Read(start_+at,bytes,count);
    }
    void Write(uint64_t at,const void* bytes,size_t count)override{
        Check(Writable(),"The disk partition is read-only");
        Check(HfsPlus::Disk::Range(at,count,size_),"Write outside the disk partition");
        disk_->Write(start_+at,bytes,count);
    }
    void Flush()override{if(Writable())disk_->Flush();}
};

std::shared_ptr<Volume> OpenLegacyDmgVolume(const std::shared_ptr<HfsPlus::Source>& source,const std::string& key)
{
    auto mfs=std::make_unique<MfsDiskSource>(source);
    if(MfsImage::Recognizes(*mfs))return FromMfs(std::move(mfs),key);
    if(source->Size()>=1536&&HfsPlus::Disk::U16(HfsPlus::Disk::Read(*source,1024,2).data())==0x4244)
        return OpenClassicSource(source,key);
    return {};
}

std::shared_ptr<Volume> OpenReadOnlyImage(std::unique_ptr<HfsPlus::Source> source,const std::string& key,
                                const std::wstring& externalJournal,const char* unsupported="The DMG contains no supported filesystem")
{
    if(HfsPlus::Volume::Recognizes(*source))
        return FromHfsPlus(std::move(source),false,key,externalJournal);
    if(!externalJournal.empty())return {};
    if(auto apfs=Apfs::Open(source,key))return apfs;
    std::shared_ptr<HfsPlus::Source> disk(std::move(source));
    if(disk->Size()>=2048){
        const auto boot=HfsPlus::Disk::Read(*disk,0,512);
        auto partitions=DiskPartitions::ProbeGptImage(*disk,boot);
        if(!partitions)partitions=DiskPartitions::ProbeApm(*disk,boot);
        if(partitions){
            for(const auto& partition:*partitions){
                auto slice=std::make_shared<DiskSlice>(disk,partition.offset,partition.length);
                if(auto legacy=OpenLegacyDmgVolume(slice,key))return legacy;
            }
        }else if(auto legacy=OpenLegacyDmgVolume(disk,key))return legacy;
    }
    std::unique_ptr<HfsPlus::Source> opticalSource=std::make_unique<DiskSlice>(disk,0,disk->Size());
    if(auto optical=Optical::Open(opticalSource,key))return optical;
    throw HfsPlus::Error(unsupported);
}
}


// Resolve the filesystem inside an editable image before choosing its worker.
struct NdifVolume {
    VolumeSession::ImageFormat format;
    std::optional<DiskPartitions::MacPartition> partition;
};

static std::optional<NdifVolume> FindNdifVolume(HfsPlus::Source& source)
{
    using F=VolumeSession::ImageFormat;
    using namespace HfsPlus::Disk;
    if(!Ndif::IsEditable(source))return {};
    if(HfsPlus::Volume::Recognizes(source))return NdifVolume{F::NdifHfsPlus,{}};
    const auto legacy=[&](uint64_t start,uint64_t length)->std::optional<F>{
        if(length<1536)return {};
        const auto magic=HfsPlus::Disk::U16(Read(source,start+1024,2).data());
        if(magic==0x4244)return F::NdifClassic;
        if(magic==0xd2d7)return F::NdifMfs;
        return {};
    };
    if(source.Size()>=2048){
        const auto boot=Read(source,0,512);
        auto partitions=DiskPartitions::ProbeGptImage(source,boot);
        if(!partitions)partitions=DiskPartitions::ProbeApm(source,boot);
        if(partitions){
            for(const auto& partition:*partitions)
                if(const auto format=legacy(partition.offset,partition.length))return NdifVolume{*format,partition};
            return {};
        }
    }
    if(const auto format=legacy(0,source.Size()))return NdifVolume{*format,{}};
    return {};
}

std::shared_ptr<Volume> OpenEditableNdif(std::unique_ptr<HfsPlus::Source> source,const std::string& key)
{
    const auto selected=FindNdifVolume(*source);
    Check(selected.has_value(),"The selected image no longer contains an editable NDIF volume");
    if(selected->format==VolumeSession::ImageFormat::NdifHfsPlus){
        const bool writable=source->Writable();
        return FromHfsPlus(std::move(source),writable,key);
    }
    std::shared_ptr<HfsPlus::Source> disk(std::move(source));
    if(const auto& part=selected->partition)
        disk=std::make_shared<DiskSlice>(disk,part->offset,part->length,!part->readOnly);
    if(selected->format==VolumeSession::ImageFormat::NdifClassic){
        const bool writable=disk->Writable();
        return OpenClassicSource(std::move(disk),key,writable,true);
    }
    return FromMfs(std::make_unique<MfsDiskSource>(std::move(disk),false),key);
}

static std::shared_ptr<Volume> OpenTrackImages(
    std::vector<std::unique_ptr<HfsPlus::Source>> tracks,const std::string& key)
{
    std::shared_ptr<Volume> optical;
    std::exception_ptr invalid;
    for(auto it=tracks.rbegin();it!=tracks.rend();++it){
        auto& track=*it;
        try{
            if(HfsPlus::Volume::Recognizes(*track))
                return FromHfsPlus(std::move(track),false,key);
            if(track->Size()>=1536){
                using namespace HfsPlus::Disk;
                const auto boot=Read(*track,0,512);
                const auto mdb=Read(*track,1024,2);
                if(HfsPlus::Disk::U16(boot.data())==0x4552||HfsPlus::Disk::U16(mdb.data())==0x4244)
                    return OpenClassicSource(std::move(track),key);
            }
            if(!optical)optical=Optical::Open(track,key);
        }catch(const HfsPlus::Error&){
            if(!invalid)invalid=std::current_exception();
        }
    }
    if(optical)return optical;
    if(invalid)std::rethrow_exception(invalid);
    throw HfsPlus::Error("No supported data filesystem was found in the optical track image");
}

static std::shared_ptr<Volume> OpenImageImpl(const std::wstring &path,const std::wstring &externalJournal,bool persistent,const Dmg::Password* password=nullptr,const Dmg::PrivateKey* privateKey=nullptr) {
    using DiskImage::Container;
    auto key=VolumeCoord::ImageKey(path);return VolumeCoord::SharedImage(key,[&]()->std::shared_ptr<Volume>{
    auto opened=DiskImage::Open(path,{false,externalJournal,password,privateKey});
    auto& source=opened.source;
    if(!source)return {};
    if(opened.container==Container::SparseBundle)
        return OpenReadOnlyImage(std::move(source),key,{},"The sparse bundle contains no supported filesystem");
    if(opened.container==Container::SparseImage)
        return OpenReadOnlyImage(std::move(source),key,externalJournal,"The sparse image contains no supported filesystem");
    if(opened.container==Container::Udif||opened.container==Container::EncryptedDisk)
        return OpenReadOnlyImage(std::move(source),key,externalJournal);
    if(opened.container==Container::Ndif){
        const auto selected=FindNdifVolume(*source);
        if(!selected)return OpenReadOnlyImage(std::move(source),key,externalJournal,"The NDIF image contains no supported filesystem");
        source.reset();
        if(persistent)return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,{},selected->format));
        try{source=DiskImage::Reopen(path,Container::Ndif,{true});}
        catch(const HfsPlus::Error&){source=DiskImage::Reopen(path,Container::Ndif,{});}
        Check(VolumeCoord::ImageKey(path)==key,
              "The selected NDIF image changed while it was being opened");
        const auto reopened=FindNdifVolume(*source);
        Check(reopened&&reopened->format==selected->format,"The NDIF filesystem changed while it was being opened");
        return OpenEditableNdif(std::move(source),key);
    }
    if(externalJournal.empty())
        if(auto tracks=CdMedia::OpenTrackImage(path,source,key))return OpenTrackImages(std::move(*tracks),key);
    if (HfsPlus::Volume::Recognizes(*source)) {
        source.reset();
        if(persistent)
            return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,externalJournal));
        try { return FromHfsPlus(DiskImage::Reopen(path,Container::Raw,{true,externalJournal}),true,key,externalJournal); }
        catch (...) { return FromHfsPlus(DiskImage::Reopen(path,Container::Raw,{false,externalJournal}),false,key,externalJournal); }
    }
    if(externalJournal.empty()){
        if(auto apfs=Apfs::Open(source,key))return apfs;
        if(auto optical=Optical::Open(source,key))return optical;
    }
    source.reset(); std::unique_ptr<MfsImageSource> mfs(new MfsImageSource); std::string error;
    if (!mfs->Open(path.c_str(), error)) throw HfsPlus::Error(error);
    if (!MfsImage::Recognizes(*mfs)) {
        mfs.reset();
        if (!externalJournal.empty() || !RecognizesClassicImage(path)) return {};
        if (persistent) return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,{},VolumeSession::ImageFormat::Classic));
        return OpenClassicImage(path,true);
    }
    mfs.reset();
    if(persistent)return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,{},VolumeSession::ImageFormat::Mfs));
    return OpenMfsImage(path,true);
    });
}
std::shared_ptr<Volume> OpenImage(const std::wstring &path,const std::wstring &externalJournal) {
    return OpenImageImpl(path,externalJournal,false);
}
std::shared_ptr<Volume> OpenBrowserImage(const std::wstring &path,const std::wstring &externalJournal,
                                         const Dmg::Password* password,const Dmg::PrivateKey* privateKey) {
    return OpenImageImpl(path,externalJournal,true,password,privateKey);
}
std::shared_ptr<Volume> OpenCd(int drive, std::shared_ptr<::MediaImageSource> media) {
    uint16_t signature=0;
    auto source = HfsPlus::OpenCd(drive, std::move(media), &signature);
    const auto key=VolumeCoord::DeviceKey(drive);
    try {
        if (HfsPlus::Volume::Recognizes(*source)) return FromHfsPlus(std::move(source),false,key);
        if (signature==0x4244) return OpenClassicSource(std::move(source),key);
        return Optical::Open(source,key);
    } catch (const std::exception &e) { throw DetectedVolumeError(e.what()); }
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
    std::set<uint32_t> parents; Storage::Progress p;if(deletionProgress){p.phase="Deleting";p.utf8=true;}auto plan = Scan(v, id, job, p, parents, 0,true,deletionProgress);
    std::function<void(const Planned &)> remove = [&](const Planned &n) {
        if (job.cancelled()) throw Storage::Cancelled();
        // A directory hard link is a link, not ownership of its backing children.
        Check(!n.entry.hardLink||v.SupportsHardLinks(), "Deleting hard links is not supported by this backend");
        if(n.entry.directory&&n.entry.hardLink&&n.children.empty()&&v.Stat(n.entry.id).linkCount==1&&!v.List(n.entry.id).empty()){RemoveTree(v,n.entry.id,job,deletionProgress);return;}
        for (const auto &c : n.children) remove(c);
        if(job.cancelled())throw Storage::Cancelled();
        if(deletionProgress){p.path=BrowserPresentation::Utf8(n.entry.name);p.scanning=false;job.progress(p);}
        v.Remove(n.entry.id);
        Storage::OutcomeEvidence::NoteRetainedChange();
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
        auto attributes=v.GetAttributes(at);for(const auto& a:attributes){auto size=a.second.size();bytes(a.first.data(),a.first.size()*2);bytes(&size,sizeof(size));HfsPlus::Bytes buffer(65536);for(uint64_t at=0;at<a.second.size();){auto n=size_t((std::min)(uint64_t(buffer.size()),a.second.size()-at));a.second.Read(at,buffer.data(),n);bytes(buffer.data(),n);at+=n;}}
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
void ValidatePlans(Volume& dst,const std::vector<Planned>& plans,uint32_t parent,bool parentExists){
 std::vector<Entry> names;for(const auto& plan:plans)names.push_back(plan.entry);dst.ValidateCopyNames(parent,names,parentExists);
 for(const auto& plan:plans){Entry found;bool exists=parentExists&&dst.FindTransferChild(parent,plan.entry,found);
  if(plan.entry.directory&&exists){Check(found.directory,"A file blocks the destination folder");Check(!found.protectedFile&&found.readError.empty(),"The destination folder cannot be merged");ValidatePlans(dst,plan.children,found.id,true);}
  else {dst.ValidateEntry(plan.entry);if(plan.entry.directory)ValidatePlans(dst,plan.children,0,false);}
 }
}
}
bool CopyMany(const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>>& sources,Volume& dst,uint32_t parent,Storage::Job& job,const TransferAdapter& adapter,const std::vector<ForkSource>& routes){
    Check(routes.empty()||(!adapter&&routes.size()==sources.size()),"Invalid transfer routes");
    Check(dst.Writable(),"The destination is read-only");Storage::Progress progress;progress.fileCopy=progress.utf8=true;progress.phase="Scanning selected files";
    std::vector<Planned> plans;
    for(const auto& source:sources){ValidateCopy(*source.first,source.second,dst,parent);std::set<uint32_t> ancestors;
        plans.push_back(Scan(*source.first,source.second,job,progress,ancestors,0,false,true,adapter));}
    ValidatePlans(dst,plans,parent,true);
    progress.scanning=false;progress.byteTransfer=true;progress.phase="Copying";job.progress(progress);bool complete=true;
    CopiedLinks copies;
    for(size_t i=0;i<plans.size();++i){
        const auto route=routes.empty()?ForkSource::Stream:routes[i];
        if(!Transfer(*sources[i].first,plans[i],dst,parent,job,progress,copies,route))complete=false;
    }
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
