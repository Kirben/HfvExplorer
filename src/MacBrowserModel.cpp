// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacBrowserModel.h"
#include "StorageHandoff.h"
#include "StorageLocation.h"
#include "floppy.h"
#include "VolumeSession.h"
#include "VolumeCoordinator.h"
#include "HfsPlusCd.h"
#include "OpticalVolume.h"
#include "CueImage.h"
#include "HfsPlusDisk.h"
#include "HfsPlusCompression.h"
#include "DiskImageOpen.h"
#include "ClassicHfsRecovery.h"
#include "DiskSlice.h"
#include "MfsDiskSource.h"
#include "MfsRecovery.h"
#include "NdifImage.h"
#include "ApfsVolume.h"
#include "ApfsCatalogWrite.h"
#include "ClassicCopyArchive.h"
#include "DiskPartitions.h"
#include "BrowserPresentation.h"
#include "HfsPlusTime.h"
#include <set>
#include <atomic>
#include <algorithm>

namespace MacBrowser {
std::wstring Wide(const std::u16string &s) { return {s.begin(), s.end()}; }
std::u16string U16(const std::wstring &s) { return {s.begin(), s.end()}; }

void ValidateHfsPlusCopyNames(const std::vector<Entry>& entries, bool caseSensitive)
{
    const auto less = [caseSensitive](const std::u16string& left, const std::u16string& right) {
        return HfsPlus::CompareNames(left, right, caseSensitive) < 0;
    };
    std::map<std::u16string, const Entry*, decltype(less)> names(less);
    for (const auto& entry : entries) {
        const auto [previous, inserted] = names.emplace(HfsPlus::Normalize(entry.name, caseSensitive), &entry);
        if (!inserted)
            throw HfsPlus::Error("Source names collide on the HFS+/HFSX destination: \"" +
                                 BrowserPresentation::Utf8(previous->second->name) + "\" and \"" +
                                 BrowserPresentation::Utf8(entry.name) + "\". Rename one before copying.");
    }
}

namespace {
std::string EncodeMfsName(const std::u16string& name)
{
    std::string encoded,error;
    if(!MfsImage::EncodeName(Wide(name),encoded,error,255))throw HfsPlus::Error(error);
    return encoded;
}
}
void ValidateMfsCopyNames(const std::vector<Entry>& entries)
{
    std::set<std::pair<std::string,uint32_t>> names;
    for(const auto& entry:entries){
        auto key=MfsImage::NameKey(EncodeMfsName(entry.name));
        if(!names.emplace(std::move(key),entry.version).second)
            throw HfsPlus::Error("Source names collide on the MFS destination at the same version. Rename one before copying.");
    }
}

SparseForks ValidateApfsSparseLayout(const Entry& entry,const Reader& read,Storage::Job& job)
{
    const auto attribute=entry.attributes.find(ApfsSparseLayoutAttribute);
    if(attribute==entry.attributes.end()||!read)return {};
    const auto& data=attribute->second;
    const std::array<uint8_t,8> signature{'H','F','V','S',1,0,0,0};
    if(data.size()<40||(data.size()-40)%16)return {};
    std::array<uint8_t,8> header{};
    data.Read(0,header.data(),header.size());
    if(header!=signature)return {};

    uint64_t cursor=header.size();
    const auto number=[&]{
        std::array<uint8_t,8> bytes{};
        data.Read(cursor,bytes.data(),bytes.size());
        cursor+=bytes.size();
        uint64_t value=0;
        for(unsigned byte=0;byte<bytes.size();++byte)value|=uint64_t(bytes[byte])<<(8*byte);
        return value;
    };
    SparseForks holes;
    for(unsigned fork=0;fork<holes.size();++fork){
        if(job.cancelled())throw Storage::Cancelled();
        if(data.size()-cursor<16)return {};
        const auto length=number(),count=number();
        if(length!=entry.forks[fork].size||count>(data.size()-cursor)/16)return {};
        uint64_t end=0;
        for(uint64_t i=0;i<count;++i){
            if(job.cancelled())throw Storage::Cancelled();
            const auto offset=number(),size=number();
            if(offset<end||offset>length||!size||size>length-offset)return {};
            holes[fork].push_back({offset,size});
            end=offset+size;
        }
    }
    if(cursor!=data.size())return {};

    // Marker contents are untrusted. A file edited on HFS+ may have data
    // in a former hole. Treat the whole layout as stale if any byte changed.
    HfsPlus::Bytes buffer(256*1024);
    for(unsigned fork=0;fork<holes.size();++fork){
        for(const auto& hole:holes[fork]){
            for(uint64_t done=0;done<hole.length;){
                if(job.cancelled())throw Storage::Cancelled();
                const auto size=size_t((std::min)(uint64_t(buffer.size()),hole.length-done));
                read(fork,hole.offset+done,buffer.data(),size);
                if(std::any_of(buffer.begin(),buffer.begin()+size,[](uint8_t byte){return byte!=0;}))return {};
                done+=size;
            }
        }
    }
    if(job.cancelled())throw Storage::Cancelled();
    return holes;
}

std::vector<SharedCloneRange> MatchApfsCloneRanges(const Entry& entry,const Reader& read,
                                                  const Entry& partner,const Reader& readPartner,Storage::Job& job)
{
    struct Run { uint64_t offset,length,physical; };
    struct Layout {
        std::array<uint8_t,56> scope{};
        std::vector<Run> runs;
    };
    const auto little=[](const uint8_t* bytes){
        uint64_t value=0;
        for(unsigned i=0;i<8;++i)value|=uint64_t(bytes[i])<<(8*i);
        return value;
    };
    const auto parse=[&](const Entry& file)->std::optional<Layout>{
        const auto found=file.attributes.find(ApfsCloneLayoutAttribute);
        if(found==file.attributes.end()||file.directory||file.symbolicLink||file.compressed)return {};
        const auto& attribute=found->second;
        constexpr std::array<uint8_t,8> signature{'H','F','V','C',1,0,0,0};
        std::array<uint8_t,64> header{};
        if(attribute.size()<=header.size()||(attribute.size()-header.size())%24)return {};
        attribute.Read(0,header.data(),header.size());
        if(!std::equal(signature.begin(),signature.end(),header.begin()))return {};
        const auto block=little(header.data()+48),size=little(header.data()+56);
        if(!little(header.data()+40)||block<512||block>65536||(block&(block-1))||
           size!=file.forks[0].size)return {};
        Layout layout;
        std::copy_n(header.begin(),layout.scope.size(),layout.scope.begin());
        uint64_t end=0;
        for(uint64_t at=header.size();at<attribute.size();at+=24){
            if(job.cancelled())throw Storage::Cancelled();
            std::array<uint8_t,24> bytes{};
            attribute.Read(at,bytes.data(),bytes.size());
            const auto offset=little(bytes.data()),length=little(bytes.data()+8),sector=little(bytes.data()+16);
            if(offset<end||offset%block||offset>size||!length||length>size-offset||
               (length%block&&length!=size-offset)||!sector||sector>UINT64_MAX/block)return {};
            const auto physical=sector*block;
            if(length>UINT64_MAX-physical)return {};
            layout.runs.push_back({offset,length,physical});
            end=offset+length;
        }
        std::sort(layout.runs.begin(),layout.runs.end(),[](const Run& a,const Run& b){return a.physical<b.physical;});
        end=0;
        for(const auto& run:layout.runs){
            if(run.physical<end)return {};
            end=run.physical+run.length;
        }
        return layout;
    };

    if(!read||!readPartner)return {};
    const auto left=parse(entry),right=parse(partner);
    if(!left||!right||left->scope!=right->scope)return {};
    constexpr uint64_t block=4096;
    HfsPlus::Bytes bytes(256*1024),other(bytes.size());
    std::vector<SharedCloneRange> matches;
    size_t a=0,b=0;
    while(a<left->runs.size()&&b<right->runs.size()){
        const auto& first=left->runs[a];
        const auto& second=right->runs[b];
        const auto begin=(std::max)(first.physical,second.physical);
        const auto end=(std::min)(first.physical+first.length,second.physical+second.length);
        if(begin<end){
            auto offset=first.offset+(begin-first.physical);
            auto partnerOffset=second.offset+(begin-second.physical);
            if(offset%block==partnerOffset%block){
                const auto skip=(block-offset%block)%block;
                uint64_t remaining=skip<end-begin?(end-begin-skip)/block*block:0;
                offset+=skip;
                partnerOffset+=skip;
                while(remaining){
                    if(job.cancelled())throw Storage::Cancelled();
                    const auto count=size_t((std::min)(remaining,uint64_t(bytes.size())));
                    read(0,offset,bytes.data(),count);
                    readPartner(0,partnerOffset,other.data(),count);
                    for(size_t at=0;at<count;at+=block){
                        if(!std::equal(bytes.begin()+at,bytes.begin()+at+block,other.begin()+at))continue;
                        if(!matches.empty()&&matches.back().offset+matches.back().length==offset+at&&
                           matches.back().partnerOffset+matches.back().length==partnerOffset+at)
                            matches.back().length+=block;
                        else matches.push_back({offset+at,partnerOffset+at,block});
                    }
                    remaining-=count;
                    offset+=count;
                    partnerOffset+=count;
                }
            }
        }
        if(first.physical+first.length<=second.physical+second.length)++a;
        else ++b;
    }
    if(job.cancelled())throw Storage::Cancelled();
    std::sort(matches.begin(),matches.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
    return matches;
}

bool IsHfsPlusSymlinkReplacementFile(const Entry& entry)
{
    return !entry.directory&&!entry.symbolicLink&&!entry.protectedFile&&entry.readError.empty();
}

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
        e.name=in.name; e.directory=in.directory; e.protectedFile=in.protectedFile||(in.posix&&in.posix->protection);
        e.compressed=in.compressed; e.hardLink=in.hardLink;e.linkCount=in.linkCount; e.readError=in.readError;
        e.finder=in.finder; e.created=in.created; e.modified=in.modified;e.additionalDates=in.additionalDates;e.datesValid=true;e.identity=EntryIdentity(std::to_string(in.id));
        e.posix=in.posix;
        e.symbolicLink=in.symbolicLink;
        if (e.symbolicLink) {
            if (e.readError.empty()) e.readError="This transfer must preserve the HFS+ symbolic link";
        } else {
            for(unsigned f=0;f<2;++f)e.forks[f].size=in.forks[f].size;
        }
        return e;
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
    Capabilities Supports() const override {
        auto capabilities = Volume::Supports();
        capabilities.hardLinks = capabilities.compression = capabilities.attributes = true;
        return capabilities;
    }
    uint32_t MakeHardLink(uint32_t source,uint32_t parent,const std::u16string& name,bool replace=false)override{return Mutate([&]{return volume_->MakeHardLink(source,parent,name,replace);});}
    uint32_t AddCopiedHardLink(uint32_t source,uint32_t parent,const std::u16string& name,
                               bool replace,Storage::Job& job)override{
        return Mutate([&]{
            if(job.cancelled())throw Storage::Cancelled();
            return volume_->MakeHardLink(source,parent,name,replace,true);
        });
    }
    void SetCompressionMethod(uint32_t id,HfsPlus::Volume::CompressionMode method,std::function<void(uint64_t,uint64_t)> progress)override{Mutate([&]{volume_->SetCompression(id,method,progress);});}
    void SetCompression(uint32_t id,bool compressed,std::function<void(uint64_t,uint64_t)> progress)override{Mutate([&]{volume_->SetCompression(id,compressed,progress);});}
    HfsPlus::Attributes GetAttributes(uint32_t id)override{Call call(*this);Ready();auto attributes=volume_->GetAttributes(id);attributes.erase(u"com.apple.system.hfs.firstlink");if(volume_->Stat(id).compressed)attributes.erase(u"com.apple.decmpfs");return attributes;}
    void SetAttributes(uint32_t id,const HfsPlus::Attributes& a)override{Mutate([&]{volume_->SetAttributes(id,a);});}
    std::optional<std::pair<uint64_t,uint64_t>> HfsPlusDateRange()const override{
        return expanded_?std::make_pair(HfsPlus::Time::Epoch,HfsPlus::Time::Maximum):
                         std::make_pair(uint64_t(0),uint64_t(UINT32_MAX));
    }
    void ValidateEntry(const Entry& e)const override{
        Volume::ValidateEntry(e);
        Check(!e.posix||e.posix->mode<=PosixMetadata::ModeMask,"Invalid HFS+ POSIX permissions");
        if(e.datesValid){(void)HfsPlus::Time::Encode(e.created,expanded_);(void)HfsPlus::Time::Encode(e.modified,expanded_);}
        if(e.additionalDates.accessed)(void)HfsPlus::Time::Encode(*e.additionalDates.accessed,expanded_);
        if(e.additionalDates.changed)(void)HfsPlus::Time::Encode(*e.additionalDates.changed,expanded_);
        HfsPlus::ValidateAttributes(e.attributes);
    }
    void ValidateCopyNames(uint32_t,const std::vector<Entry>& entries,bool)override{
        ValidateHfsPlusCopyNames(entries,CaseSensitive());
    }
    uint32_t MakeDirectoryWithAttributes(uint32_t parent,const Entry& e)override{
        ValidateEntry(e);
        return Mutate([&]{return volume_->MakeDirectory(parent,e.name,e.attributes.empty()?nullptr:&e.attributes);});
    }
    void FinalizeDirectory(uint32_t id,const Entry& desired,const Entry& expected,bool merge,bool replaceAttributes)override{
        Mutate([&]{
            const auto now=volume_->Stat(id);
            Check(now.directory&&now.id==expected.id&&now.parent==expected.parent&&now.name==expected.name,
                  "Destination folder changed during copying");
            const std::array<uint64_t,2> dates{desired.created,desired.modified};
            volume_->SetDirectoryMetadata(id,merge?nullptr:&desired.finder,desired.datesValid?&dates:nullptr,
                                          replaceAttributes&&desired.attributesKnown?&desired.attributes:nullptr,
                                          merge?std::nullopt:desired.posix,desired.additionalDates);
        });
    }
    bool CaseSensitive() const override { return caseSensitive_; }
    uint64_t FreeBytes() const override { Call call(*this);if(!usable_)throw HfsPlus::Error("Media was released; select the drive again");return volume_->FreeBytes(); }
    std::vector<Entry> List(uint32_t id) override {
        Call call(*this);Ready();std::vector<Entry> out;for(const auto& e:volume_->List(id))out.push_back(Describe(e));return out;
    }
    bool FindChild(uint32_t parent, const std::u16string& name, Entry& found) override {
        Call call(*this); Ready();
        HfsPlus::Entry entry;
        if (!volume_->FindChild(parent,name,entry)) return false;
        found=Describe(entry);
        return true;
    }
    bool FindTransferChild(uint32_t parent,const Entry& entry,Entry& found) override {
        return FindChild(parent,HfsPlus::Normalize(entry.name,CaseSensitive()),found);
    }
    Entry Stat(uint32_t id) override {
        Call call(*this);Ready();
        auto entry=Describe(volume_->Stat(id));
        if(entry.compressed&&entry.readError.empty()){
            const auto attributes=volume_->GetAttributes(id);
            const auto found=attributes.find(u"com.apple.decmpfs");
            if(found!=attributes.end()&&found->second.size()<=HfsPlus::InlineCompressionLimit){
                HfsPlus::Bytes bytes(found->second);
                if(HfsPlus::IsInlineCompression(bytes,entry.forks[0].size))entry.inlineCompression=std::move(bytes);
                else if(bytes.size()==16){
                    entry.compressionResourceSize=volume_->CompressionResourceSize(id);
                    if(entry.compressionResourceSize)entry.resourceCompressionHeader=std::move(bytes);
                }
            }
        }
        return entry;
    }
    std::optional<HfsPlus::Bytes> SymbolicLinkTarget(uint32_t id) override {
        Call call(*this);Ready();return volume_->SymbolicLinkTarget(id);
    }
    bool CanReplaceFileWithSymbolicLink(const Entry& entry) const override {
        return Writable()&&IsHfsPlusSymlinkReplacementFile(entry);
    }
    void ValidateSymbolicLink(const Entry& entry,const HfsPlus::Bytes& target) const override {
        Check(Writable(),"HFS+ image is read-only");
        Check(entry.symbolicLink&&!entry.directory&&!entry.hardLink&&!entry.compressed&&
              !entry.forks[0].size&&!entry.forks[1].size&&!entry.version,
              "Unsupported HFS+ symbolic link metadata");
        HfsPlus::Volume::ValidateSymbolicLinkTarget(target);
        ValidateEntry(entry);
    }
    void PutSymbolicLink(uint32_t parent,const Entry& entry,const HfsPlus::Bytes& target,Storage::Job& job) override {
        ValidateSymbolicLink(entry,target);
        Mutate([&]{
            if(job.cancelled())throw Storage::Cancelled();
            const bool replace=!entry.expectedDestinationIdentity.empty();
            if(replace){
                HfsPlus::Entry existing;
                const auto name=HfsPlus::Normalize(entry.name,CaseSensitive());
                Check(volume_->FindChild(parent,name,existing)&&
                      Describe(existing).identity==entry.expectedDestinationIdentity,
                      "The HFS+ symbolic link destination changed after overwrite approval");
            }
            const std::array<uint64_t,2> dates{entry.created,entry.modified};
            volume_->PutSymbolicLink(parent,entry.name,target,entry.finder,replace,entry.attributes,
                                     entry.datesValid?&dates:nullptr,entry.posix,entry.additionalDates);
        });
    }
    void Read(uint32_t id, unsigned fork, uint64_t offset, void *p, size_t n) override {
        Call call(*this);Ready(); Check(volume_->Read(id, fork, offset, p, n) == n, "Short Macintosh fork read");
    }
    void ReadCompressionResource(uint32_t id,uint64_t offset,void* output,size_t count) override {
        Call call(*this);Ready();volume_->ReadCompressionResource(id,offset,output,count);
    }
    uint32_t MakeDirectory(uint32_t p, const std::u16string &s) override {
        return Mutate([&] { return volume_->MakeDirectory(p, s); });
    }
    uint32_t Put(uint32_t parent,const Entry& e,Reader read,bool replace) override {
        using Mode=HfsPlus::Volume::CompressionMode;
        const auto compression=e.archivedCompressionType&&e.inlineCompression.empty()?
            HfsPlus::CompressionModeForType(e.archivedCompressionType):
            e.compressed?Mode::CopyCompressed:Mode::Preserve;
        const std::array<uint64_t,2> dates{e.created,e.modified};
        return Mutate([&]{
            auto posix=e.posix;
            if(e.protectedFile&&!posix){
                // Classic HFS supplies a single lock bit without POSIX metadata.
                HfsPlus::Entry old;
                if(replace&&volume_->FindChild(parent,HfsPlus::Normalize(e.name,CaseSensitive()),old))
                    posix=old.posix;
                if(!posix)posix=PosixMetadata{0,0,0644};
                posix->protection|=PosixMetadata::UserImmutable;
            }
            const bool storedResource=e.WritesStoredCompressionResource();
            const auto* compressionBytes=storedResource?&e.resourceCompressionHeader:
                e.compressed&&!e.inlineCompression.empty()?&e.inlineCompression:nullptr;
            return volume_->Put(parent,e.name,e.forks[0].size,e.forks[1].size,read,&e.finder,replace,nullptr,
                                e.attributesKnown?&e.attributes:nullptr,compression,
                                e.datesValid?&dates:nullptr,posix,e.hfsOverwriteScope,e.additionalDates,
                                compressionBytes,storedResource);
        });
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

class RecoverableMfsImageSource final : public MfsImageSource {
public:
    std::unique_ptr<CatalogUndo> StageCatalogUndo(const std::vector<BYTE>& original) override {
        return MfsRecovery::Stage(NativeHandle(),original);
    }
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
        e.identity=EntryIdentity(std::to_string(in.id),(uint64_t(in.created)<<8)|in.version);return e;
    }
    void Ready()const{Check(usable_,"Reopen the MFS image after a failed operation");}
    void Mutate(std::function<bool()> work){
        Call call(*this);Ready();Check(writable_,"MFS image is read-only");
        try{if(!work())throw HfsPlus::Error(image_.Error());Sync();}
        catch(...){auto failure=std::current_exception();if(image_.RecoveryRequired())usable_=false;else{try{if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());Sync();}catch(...){usable_=false;}}std::rethrow_exception(failure);}
    }

public:
    WorkerExecution Execution()const override{WorkerExecution result;result.processLocal=source_->ProcessLocal();return result;}
    explicit Mfs(std::unique_ptr<MfsSource> source,std::string key,std::shared_ptr<MfsCatalogIds> ids):source_(std::move(source)),resourceKey_(std::move(key)),ids_(ids?std::move(ids):std::make_shared<MfsCatalogIds>()){if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());Sync();}
    WorkerOrigin WorkerDescription()const override{WorkerOrigin out;auto files=source_->WorkerFiles();if(!files.empty()){out.kind=WorkerOrigin::Image;out.path=files.front().first;}out.writable=source_->Writable();return out;}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return source_->WorkerFiles();}
    void WorkerUncleanExit()override{
        const auto disk=dynamic_cast<RecoverableMfsImageSource*>(source_.get());
        if(disk&&disk->Writable()&&MfsRecovery::Pending(disk->NativeHandle()))return;
        source_->DisableWrites();writable_=false;
    }
    void SuspendWorker()override{Call call(*this);usable_=false;image_=MfsImage();}
    void ResumeWorker()override{
        Call call(*this);
        if(const auto disk=dynamic_cast<RecoverableMfsImageSource*>(source_.get()))
            MfsRecovery::Recover(disk->NativeHandle(),disk->Writable());
        if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());
        Sync();usable_=true;
    }
    std::string ResourceKey()const override{return resourceKey_;}
    std::u16string Name()const override{return *std::atomic_load(&name_);}
    const char* Kind()const override{return "MFS";}
    bool Writable()const override{return usable_&&writable_;}
    Capabilities Supports() const override {
        auto capabilities = Volume::Supports();
        capabilities.unicodeNames = capabilities.createFolders = false;
        capabilities.basicMetadata = true;
        return capabilities;
    }
    bool CaseSensitive()const override{return false;}
    uint64_t FreeBytes()const override{return free_;}
    void Reload()override{Call call(*this);Ready();try{if(!image_.Open(*source_))throw HfsPlus::Error(image_.Error());Sync();}catch(...){usable_=false;throw;}}
    std::vector<Entry> List(uint32_t parent)override{Call call(*this);Ready();Check(parent==2,"MFS has no subdirectories");std::vector<Entry> out;for(auto& pair:entries_)out.push_back(EntryAt(pair.second));return out;}
    Entry Stat(uint32_t id)override{Call call(*this);Ready();if(id==2){Entry e;e.id=2;e.parent=1;e.directory=true;e.name=Name();return e;}return EntryAt(entries_.at(id));}
    bool FindChild(uint32_t parent,const std::u16string& name,Entry& found)override{
        Call call(*this);Ready();Check(parent==2,"MFS has no subdirectories");auto encoded=EncodeMfsName(name);bool exists=false;
        for(auto& p:entries_)if(MfsImage::EqualNames(image_.Entries()[p.second].name,encoded)){Check(!exists,"Several MFS versions have this name; select a specific version");found=EntryAt(p.second);exists=true;}return exists;
    }
    bool FindTransferChild(uint32_t parent,const Entry& source,Entry& found)override{Call call(*this);Ready();Check(parent==2,"MFS has no subdirectories");auto encoded=EncodeMfsName(source.name);for(auto& p:entries_){auto& e=image_.Entries()[p.second];if(e.version==source.version&&MfsImage::EqualNames(e.name,encoded)){found=EntryAt(p.second);return true;}}return false;}
    void Read(uint32_t id,unsigned fork,uint64_t at,void* p,size_t n)override{Call call(*this);Ready();size_t got=0;if(!image_.ReadFork(entries_.at(id),fork,at,p,n,got))throw HfsPlus::Error(image_.Error());Check(got==n,"Short MFS fork read");}
    void ValidateCopyNames(uint32_t,const std::vector<Entry>& entries,bool)override{
        ValidateMfsCopyNames(entries);
    }
    void ValidateEntry(const Entry& e)const override{
        Check(!e.datesValid||(e.created<=UINT32_MAX&&e.modified<=UINT32_MAX),"MFS cannot preserve dates after February 2040");Check(!e.directory,"MFS is flat and cannot store folders");Check(e.attributes.empty(),"MFS cannot preserve extended attributes or ACLs");EncodeMfsName(e.name);Check(e.version<=255,"Invalid MFS file version");
        Check(e.forks[0].size<=20*1024*1024&&e.forks[1].size<=20*1024*1024,"File exceeds the MFS 20 MiB volume limit");
        Check(std::all_of(e.finder.begin()+16,e.finder.end(),[](uint8_t b){return b==0;}),"MFS can store only the first 16 bytes of Finder information");
    }
    void ValidateRemoval(uint32_t id)override{auto e=Stat(id);Check(Writable()&&id!=2&&!e.protectedFile,"MFS file is locked or read-only");}
    uint32_t MakeDirectory(uint32_t,const std::u16string&)override{throw HfsPlus::Error("MFS is flat and has no subdirectories");}
    uint32_t Put(uint32_t parent,const Entry& e,Reader reader,bool overwrite)override{
        Check(parent==2,"MFS has no subdirectories");ValidateEntry(e);MfsEntry raw;raw.name=EncodeMfsName(e.name);raw.version=BYTE(e.version);raw.flags=BYTE(0x80|(e.protectedFile?1:0));raw.created=uint32_t(e.created);raw.modified=uint32_t(e.modified);raw.datesValid=e.datesValid;memcpy(raw.finderInfo,e.finder.data(),16);for(unsigned f=0;f<2;++f)raw.forks[f].length=uint32_t(e.forks[f].size);uint32_t id=0;
        Mutate([&]{return image_.Put(raw,reader,overwrite,id);});Call call(*this);return ids_->byFile.at(id);
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{Check(parent==2&&id!=2,"MFS files remain in the volume root");auto encoded=EncodeMfsName(name);Mutate([&]{return image_.Rename(image_.Entries().at(entries_.at(id)).id,encoded);});}
    void Remove(uint32_t id)override{Check(id!=2,"Cannot remove an MFS volume root");Mutate([&]{return image_.Remove(image_.Entries().at(entries_.at(id)).id);});}
    void SetBasicMetadata(uint32_t id,const Entry& e)override{Check(id!=2&&!e.directory,"Select an MFS file");ValidateEntry(e);auto name=EncodeMfsName(e.name);Mutate([&]{auto raw=image_.Entries().at(entries_.at(id));raw.name=name;raw.version=BYTE(e.version);raw.flags=BYTE((raw.flags&~1)|(e.protectedFile?1:0));raw.created=uint32_t(e.created);raw.modified=uint32_t(e.modified);memcpy(raw.finderInfo,e.finder.data(),16);return image_.SetMetadata(raw.id,raw);});}
};
struct Planned { Entry entry; std::optional<Entry> keptFolder; bool mergedFolder=false, keptCompanion=false; std::vector<Planned> children; Reader reader; std::optional<HfsPlus::Bytes> linkTarget; bool restoredDirectoryLink=false; std::optional<ClassicCopyArchive::Metadata> archivedMetadata; bool restoredMetadata=false; bool projectedName=false; bool restoredNameChanged=false; std::string archiveLinkGroup; uint64_t archivedDescriptionSize=0; };
Planned Scan(Volume &v, uint32_t id, Storage::Job &job, Storage::Progress &p,
             std::set<uint32_t> &ancestors, unsigned depth,bool removal,bool reportProgress,const TransferAdapter& adapter={}) {
    if (job.cancelled()) throw Storage::Cancelled();
    Planned out; out.entry = v.Stat(id);auto identity=out.entry.resolvedId?out.entry.resolvedId:id;
    Check(depth <= 128 && ancestors.insert(identity).second, "Recursive folder link or excessive folder depth");
    if(!adapter&&!out.entry.readError.empty())out.linkTarget=v.SymbolicLinkTarget(id);
    Check(out.entry.readError.empty()||out.linkTarget.has_value(), out.entry.readError.c_str());
    out.entry.attributes=v.GetAttributes(id);out.entry.attributesKnown=true;
    // Logical exports decode compression and resolve links rather than cloning
    // their private backing topology or compressed resource-fork storage.
    if(out.entry.compressed)out.entry.attributes.erase(u"com.apple.decmpfs");
    if(out.entry.hardLink)out.entry.attributes.erase(u"com.apple.system.hfs.firstlink");
    if (out.entry.directory){
        if (adapter) out.reader=adapter(v,out.entry,job,depth==0);if(!removal||!out.entry.hardLink||out.entry.linkCount<=1)for(auto &child:v.List(id))out.children.push_back(Scan(v,child.id,job,p,ancestors,depth+1,removal,reportProgress,adapter));}
    else { if(!removal&&!out.linkTarget&&(!out.entry.posix||!out.entry.posix->IsSpecial())){v.Read(id,0,0,nullptr,0);if(out.entry.forks[1].size)v.Read(id,1,0,nullptr,0);}
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
// The file remains an ordinary HFS+ file. This attribute records the
// APFS layout without affecting logical fork contents or occupying either fork.
void RecordApfsSparseRanges(Entry& entry,const SparseForks& holes)
{
    if(entry.attributes.count(ApfsSparseLayoutAttribute))return; // Preserve an existing source attribute.
    HfsPlus::Bytes value{'H','F','V','S',1,0,0,0};
    const auto append=[&](uint64_t number){
        for(unsigned byte=0;byte<8;++byte)value.push_back(uint8_t(number>>(8*byte)));
    };
    for(unsigned fork=0;fork<2;++fork){
        append(entry.forks[fork].size);
        append(holes[fork].size());
        for(const auto& hole:holes[fork]){
            append(hole.offset);
            append(hole.length);
        }
    }
    entry.attributes.emplace(ApfsSparseLayoutAttribute,std::move(value));
}
int64_t ApfsTimeInHfsSeconds(uint64_t raw)
{
    const auto time=int64_t(raw);
    constexpr int64_t second=1000000000;
    return time/second-(time%second<0)+int64_t(HfsPlus::Time::Epoch);
}

void PrepareBackupDate(Entry& entry,const Volume& destination)
{
    constexpr auto key=u"org.hfvexplorer.hfs-backup-date";
    constexpr std::array<uint8_t,8> header{'H','F','V','B',1,0,0,0};
    const auto range=destination.HfsPlusDateRange();
    const std::string_view kind=destination.Kind();
    if(kind!="APFS"&&kind!="HFS"&&!range)return;
    auto& backup=entry.additionalDates.backup;
    const auto marker=entry.attributes.find(key);
    const auto fits=[&](uint64_t date){
        return range?date>range->first&&date<=range->second:kind=="HFS"&&date<=UINT32_MAX;
    };
    if((kind=="HFS"||range)&&!backup.value_or(0)&&marker!=entry.attributes.end()&&marker->second.size()==16){
        const HfsPlus::Bytes value(marker->second);
        if(std::equal(header.begin(),header.end(),value.begin())){
            uint64_t date=0;
            for(unsigned i=0;i<8;++i)date|=uint64_t(value[8+i])<<(8*i);
            if(date&&fits(date)){
                backup=date;
                entry.attributes.erase(marker);
                return;
            }
        }
    }
    if(!backup.value_or(0)||fits(*backup)||kind=="HFS")return;
    HfsPlus::Bytes value(header.begin(),header.end());
    for(unsigned i=0;i<8;++i)value.push_back(uint8_t(*backup>>(8*i)));
    Check(marker==entry.attributes.end()||marker->second==value,
          "An existing attribute conflicts with the HFS backup date");
    entry.attributes.emplace(key,std::move(value));
    if(range)backup=0;
}

void PrepareCopyTimestamps(Planned& plan,const Volume& destination,bool fromApfs,bool fromHfsPlus)
{
    auto& entry=plan.entry;
    PrepareBackupDate(entry,destination);
    if(entry.datesValid&&entry.additionalDates.changed&&entry.additionalDates.accessed){
        std::array<uint64_t,4> seconds{entry.created,entry.modified,*entry.additionalDates.changed,*entry.additionalDates.accessed};
        const auto range=destination.HfsPlusDateRange();
        auto projected=seconds;
        if(range&&entry.apfsTimestamps)
            for(size_t i=0;i<projected.size();++i)
                projected[i]=uint64_t(std::clamp(ApfsTimeInHfsSeconds((*entry.apfsTimestamps)[i]),
                                                int64_t(range->first),int64_t(range->second)));
        if((fromApfs||entry.apfsTimestamps)&&range&&(entry.fractionalDates||projected!=seconds)&&entry.apfsTimestamps&&
           !entry.attributes.count(ApfsTimesAttribute)){
            HfsPlus::Bytes value{'H','F','V','T',1,0,0,0};
            for(const auto& values:{projected,*entry.apfsTimestamps})
                for(const auto number:values)
                    for(unsigned byte=0;byte<8;++byte)value.push_back(uint8_t(number>>(byte*8)));
            entry.attributes.emplace(ApfsTimesAttribute,std::move(value));
            entry.fractionalDates=true;
            entry.created=projected[0];entry.modified=projected[1];
            entry.additionalDates.accessed=projected[3];
            entry.additionalDates.changed=projected[2];
        }else if(fromHfsPlus&&std::string_view(destination.Kind())=="APFS"){
            const auto marker=entry.attributes.find(ApfsTimesAttribute);
            if(marker!=entry.attributes.end()){
                const auto& value=marker->second;
                constexpr std::array<uint8_t,8> header{'H','F','V','T',1,0,0,0};
                if(value.size()==72&&std::equal(header.begin(),header.end(),value.begin())){
                    const auto word=[&](size_t offset){
                        uint64_t number=0;
                        for(unsigned byte=0;byte<8;++byte)number|=uint64_t(value[offset+byte])<<(byte*8);
                        return number;
                    };
                    std::array<uint64_t,4> raw{};
                    for(size_t i=0;i<raw.size();++i){
                        raw[i]=seconds[i]==word(8+i*8)?word(40+i*8):Apfs::CatalogTimestamp(seconds[i]);
                        seconds[i]=uint64_t(ApfsTimeInHfsSeconds(raw[i]));
                    }
                    entry.created=seconds[0];entry.modified=seconds[1];
                    entry.additionalDates.accessed=seconds[3];
                    entry.additionalDates.changed=seconds[2];
                    entry.apfsTimestamps=raw;
                    entry.attributes.erase(marker);
                }
            }
        }
    }
    for(auto& child:plan.children)PrepareCopyTimestamps(child,destination,fromApfs,fromHfsPlus);
}
using CopiedLinks=std::map<std::pair<std::string,uint32_t>,Entry>;
void LinkedProgress(const Planned& plan,Storage::Progress& p){if(plan.entry.directory){for(auto& child:plan.children)LinkedProgress(child,p);}else{++p.files;p.processed+=plan.entry.forks[0].size+plan.entry.forks[1].size;}}
void ValidateArchiveNames(const Planned& plan)
{
    Check(!ClassicCopyArchive::ReservedName(plan.entry.name),
          "A selected name is reserved for classic HFS metadata companions");
    for(const auto& child:plan.children)ValidateArchiveNames(child);
}

void ProjectArchiveNames(std::vector<Planned>& plans,Volume& destination,uint32_t parent,bool parentExists,
                         const std::u16string& path,std::string& preview,Storage::Job& job)
{
    std::set<std::string> occupied,existing;
    if(parentExists)
        for(const auto& entry:destination.List(parent))existing.insert(ClassicCopyNameKey(entry.name));
    const auto display=[](std::u16string name){
        for(auto& ch:name)if(ch<32)ch=char16_t(0x2400+ch);
        return BrowserPresentation::Utf8(name);
    };
    std::vector<bool> unchanged(plans.size());
    for(size_t i=0;i<plans.size();++i){
        const auto& name=plans[i].entry.name;
        if(ProjectClassicCopyName(name)!=name||ClassicCopyArchive::ReservedName(name))continue;
        unchanged[i]=occupied.insert(ClassicCopyNameKey(name)).second;
    }
    for(size_t i=0;i<plans.size();++i){
        if(job.cancelled())throw Storage::Cancelled();
        auto& plan=plans[i];
        auto& name=plan.entry.name;
        if(!unchanged[i]){
            const auto original=name;
            for(unsigned suffix=0;;++suffix){
                Check(suffix<UINT32_MAX,"No available classic HFS name");
                auto candidate=ProjectClassicCopyName(original,suffix);
                const auto key=ClassicCopyNameKey(candidate);
                if(ClassicCopyArchive::ReservedName(candidate)||existing.count(key)||!occupied.insert(key).second)
                    continue;
                name=std::move(candidate);
                plan.projectedName=true;
                break;
            }
            preview+=display(path+original)+"  ->  "+display(path+name)+"\r\n";
            Check(preview.size()<900*1024,"Too many renamed entries for one classic HFS preview");
        }
        if(plan.entry.directory){
            Entry current;
            const bool merge=unchanged[i]&&parentExists&&destination.FindChild(parent,name,current)&&current.directory;
            ProjectArchiveNames(plan.children,destination,merge?current.id:parent,merge,path+name+u"/",preview,job);
        }
    }
}

void PrepareClassicArchive(Planned& plan)
{
    plan.archivedMetadata.emplace(plan.entry);
    plan.archivedMetadata->symbolicLinkTarget=plan.linkTarget;
    auto& entry=plan.entry;
    // Links and special nodes are represented by ordinary catalog records.
    // Their descriptors are carried only in the verified companion.
    if(plan.linkTarget||(entry.posix&&entry.posix->IsSpecial())){
        std::string description;
        if(plan.linkTarget){
            description="Symbolic link target: ";
            const auto end=plan.linkTarget->end()-(!plan.linkTarget->empty()&&plan.linkTarget->back()==0);
            description.append(plan.linkTarget->begin(),end);
        }else{
            description="Special file type "+std::to_string(unsigned(entry.posix->special));
            if(IsDevice(entry.posix->special))description+="; device "+std::to_string(entry.posix->device);
        }
        description+="\r";
        plan.archivedDescriptionSize=description.size();
        plan.reader=[description=std::move(description)](unsigned fork,uint64_t at,void* out,size_t count){
            Check(!fork&&at<=description.size()&&count<=description.size()-at,"Invalid archive description read");
            memcpy(out,description.data()+at,count);
        };
        plan.linkTarget.reset();
        entry.symbolicLink=false;
        entry.readError.clear();
        entry.forks[0]={};
        entry.forks[1]={};
        entry.forks[0].size=plan.archivedDescriptionSize;
        entry.finder={};
        memcpy(entry.finder.data(),"TEXTHFVE",8);
        entry.protectedFile=false;
    }
    entry.posix.reset();
    entry.hardLink=false;
    entry.linkCount=1;
    entry.compressed=false;
    entry.inlineCompression.clear();
    entry.resourceCompressionHeader.clear();
    entry.compressionResourceSize=0;
    entry.attributes.clear();
    entry.created=(std::min)(entry.created,uint64_t(UINT32_MAX));
    entry.modified=(std::min)(entry.modified,uint64_t(UINT32_MAX));
    if(entry.additionalDates.backup.value_or(0)>UINT32_MAX)entry.additionalDates.backup=0;
    for(auto& child:plan.children)PrepareClassicArchive(child);
}

using ArchiveLinkGroups=std::map<std::pair<std::string,uint32_t>,std::string>;

void RecordArchiveLayout(Volume& source,Planned& plan,ArchiveLinkGroups& groups,Storage::Progress& progress,Storage::Job& job)
{
    if(job.cancelled())throw Storage::Cancelled();
    auto& archive=*plan.archivedMetadata;
    auto& entry=archive.entry;
    if(plan.archivedDescriptionSize){
        progress.totalBytes-=entry.forks[0].size+entry.forks[1].size;
        Check(plan.archivedDescriptionSize<=UINT64_MAX-progress.totalBytes,"Copy size overflow");
        progress.totalBytes+=plan.archivedDescriptionSize;
    }
    if(entry.compressed){
        const auto& header=entry.inlineCompression.empty()?entry.resourceCompressionHeader:entry.inlineCompression;
        Check(!header.empty(),"Cannot identify the original compression method for archiving");
        archive.compressionType=HfsPlus::Compression(header,0,{}).Type();
    }
    if(entry.hardLink&&entry.linkCount>1){
        auto [group,inserted]=groups.try_emplace({source.ResourceKey(),entry.resolvedId});
        if(inserted){
            GUID id{};
            Check(SUCCEEDED(CoCreateGuid(&id)),"Cannot allocate a classic archive link group identifier");
            group->second.assign(reinterpret_cast<const char*>(&id),sizeof(id));
        }
        archive.linkGroup=group->second;
    }
    if(!entry.directory&&!entry.symbolicLink&&(!entry.posix||!entry.posix->IsSpecial())){
        const auto holes=source.SparseRanges(entry.id);
        if(!holes[0].empty()||!holes[1].empty())RecordApfsSparseRanges(entry,holes);
        if(entry.apfsCloneMarked&&!entry.attributes.count(ApfsCloneLayoutAttribute)){
            auto layout=source.ApfsCloneLayout(entry.id);
            if(!layout.empty())entry.attributes.emplace(ApfsCloneLayoutAttribute,std::move(layout));
        }
    }
    for(auto& child:plan.children)RecordArchiveLayout(source,child,groups,progress,job);
}

struct ArchiveSpace {
    uint64_t forks=0, companions=0, padding=0, catalog=0;
    uint64_t block;
    uint64_t available;
    uint64_t mergeCompanions=0;

    static void Add(uint64_t& total,uint64_t value)
    {
        Check(value<=UINT64_MAX-total,"Archive space estimate overflow");
        total+=value;
    }

    void Payload(uint64_t& total,uint64_t size)
    {
        Add(total,size);
        if(size%block)Add(padding,block-size%block);
    }

    void Count(Volume& destination,std::optional<uint32_t> parent,const Planned& plan,Storage::Job& job)
    {
        if(job.cancelled())throw Storage::Cancelled();
        Entry existing;
        const bool merge=plan.entry.directory&&parent&&
            destination.FindTransferChild(*parent,plan.entry,existing)&&existing.directory;
        for(const auto& fork:plan.entry.forks)Payload(forks,fork.size);
        const auto companionSize=ClassicCopyArchive::EncodedSize(*plan.archivedMetadata,job);
        if(merge){
            Add(mergeCompanions,companionSize);
            if(companionSize%block)Add(mergeCompanions,block-companionSize%block);
        }else{
            Payload(companions,companionSize);
            // Allow for growing catalog records for the item and its companion.
            Add(catalog,block);
        }
        for(const auto& child:plan.children)
            Count(destination,merge?std::optional<uint32_t>(existing.id):std::nullopt,child,job);
    }

    bool Fits()const
    {
        uint64_t minimum=forks;
        Add(minimum,companions);
        Add(minimum,padding);
        return minimum<=available;
    }

    std::string Details()const
    {
        return "\r\n\r\nArchive space (bytes)\r\nDense forks: "+std::to_string(forks)+
            "   Companions: "+std::to_string(companions)+
            "\r\nAllocation padding: "+std::to_string(padding)+
            "   Catalog allowance: "+std::to_string(catalog)+
            "\r\nFree: "+std::to_string(available)+(Fits()?"":"   Insufficient archive space")+
            (mergeCompanions?"\r\nAdditional companions if source folder metadata is used: "+std::to_string(mergeCompanions):"");
    }
};

void RestoreClassicArchive(Volume& source,Planned& plan,Storage::Progress& progress,Storage::Job& job,bool carry=false)
{
    auto& entry=plan.entry;
    if(auto archive=ClassicCopyArchive::Load(source,entry,job)){
        if(carry)plan.archivedMetadata=std::move(*archive);
        else{
            auto* metadata=&archive->entry;
            plan.archiveLinkGroup=std::move(archive->linkGroup);
            if(archive->compressionType){
                entry.compressed=true;
                entry.archivedCompressionType=archive->compressionType;
                entry.inlineCompression=std::move(metadata->inlineCompression);
            }
            if(!metadata->name.empty()){
                plan.restoredNameChanged=metadata->name!=entry.name;
                entry.name=std::move(metadata->name);
            }
            entry.datesValid=metadata->datesValid;
            entry.created=metadata->created;
            entry.modified=metadata->modified;
            entry.additionalDates=metadata->additionalDates;
            entry.apfsTimestamps=metadata->apfsTimestamps;
            if(entry.apfsTimestamps)
                for(auto raw:*entry.apfsTimestamps)entry.fractionalDates|=int64_t(raw)%1000000000!=0;
            entry.posix=metadata->posix;
            if(entry.posix)entry.protectedFile|=(entry.posix->protection&PosixMetadata::ProtectionMask)!=0;
            if(archive->symbolicLinkTarget||(entry.posix&&entry.posix->IsSpecial())){
                progress.totalBytes-=entry.forks[0].size+entry.forks[1].size;
                entry.forks[0]={};
                entry.forks[1]={};
                entry.finder=metadata->finder;
                if(archive->symbolicLinkTarget){
                    plan.linkTarget=std::move(archive->symbolicLinkTarget);
                    entry.symbolicLink=true;
                }
            }
            entry.attributes=std::move(metadata->attributes);
            entry.attributesKnown=true;
        }
        plan.restoredMetadata=true;
    }
    for(auto& child:plan.children)RestoreClassicArchive(source,child,progress,job,carry);
    std::set<std::u16string> consumed;
    for(const auto& child:plan.children)
        if(child.restoredMetadata)consumed.insert(ClassicCopyArchive::CompanionName(child.entry.resolvedId));
    plan.children.erase(std::remove_if(plan.children.begin(),plan.children.end(),[&](const auto& child){
        if(child.restoredMetadata||!consumed.count(child.entry.name))return false;
        --progress.totalFiles;
        progress.totalBytes-=child.entry.forks[0].size;
        return true;
    }),plan.children.end());
}

constexpr auto ArchiveRestoreGroupAttribute=u"org.hfvexplorer.classic-archive-group";
constexpr std::array<uint8_t,8> ArchiveGroupSignature{'H','F','V','G',1,0,0,0};

bool ArchiveGroupMarker(const HfsPlus::AttributeData& value,const std::string* group=nullptr)
{
    std::array<uint8_t,24> bytes{};
    if(value.size()!=bytes.size())return false;
    value.Read(0,bytes.data(),bytes.size());
    return std::equal(ArchiveGroupSignature.begin(),ArchiveGroupSignature.end(),bytes.begin())&&
           (!group||(group->size()==16&&!memcmp(bytes.data()+ArchiveGroupSignature.size(),group->data(),group->size())));
}

void NoteArchiveRestoreGroup(Entry& entry,const std::string& group)
{
    if(group.empty())return;
    const auto old=entry.attributes.find(ArchiveRestoreGroupAttribute);
    if(old!=entry.attributes.end()&&!ArchiveGroupMarker(old->second))return;
    HfsPlus::Bytes value(ArchiveGroupSignature.begin(),ArchiveGroupSignature.end());
    value.insert(value.end(),group.begin(),group.end());
    entry.attributes.insert_or_assign(ArchiveRestoreGroupAttribute,std::move(value));
}

bool MatchesArchivedEntry(Volume& source,const Planned& plan,Volume& destination,const Entry& candidate,
                          Storage::Job& job,unsigned depth,bool validatePartner,bool verifyMove=false)
{
    if(job.cancelled())throw Storage::Cancelled();
    const auto& entry=verifyMove&&plan.keptFolder?*plan.keptFolder:plan.entry;
    const bool special=plan.linkTarget||(entry.posix&&entry.posix->IsSpecial());
    if(depth>128||(validatePartner&&depth&&(candidate.hardLink||!plan.archiveLinkGroup.empty()))||
       (!special&&!candidate.readError.empty())||entry.directory!=candidate.directory||
       entry.symbolicLink!=candidate.symbolicLink||entry.finder!=candidate.finder||
       ((validatePartner||entry.posix)&&!(entry.posix==candidate.posix))||entry.protectedFile!=candidate.protectedFile||
       (entry.datesValid&&(!candidate.datesValid||entry.created!=candidate.created||entry.modified!=candidate.modified||
        (entry.apfsTimestamps&&entry.apfsTimestamps!=candidate.apfsTimestamps)||
        (entry.additionalDates.accessed&&entry.additionalDates.accessed!=candidate.additionalDates.accessed)||
        (entry.additionalDates.changed&&entry.additionalDates.changed!=candidate.additionalDates.changed)||
        entry.additionalDates.backup.value_or(0)!=candidate.additionalDates.backup.value_or(0)))||
       (!special&&(entry.forks[0].size!=candidate.forks[0].size||entry.forks[1].size!=candidate.forks[1].size))||
       entry.compressed!=candidate.compressed)return false;

    auto attributes=destination.GetAttributes(candidate.id);
    auto expected=entry.attributes;
    const auto forgetGroup=[](HfsPlus::Attributes& values){
        const auto marker=values.find(ArchiveRestoreGroupAttribute);
        if(marker!=values.end()&&ArchiveGroupMarker(marker->second))values.erase(marker);
    };
    forgetGroup(expected);
    forgetGroup(attributes);
    if(candidate.hardLink)attributes.erase(u"com.apple.system.hfs.firstlink");
    if(candidate.compressed)attributes.erase(u"com.apple.decmpfs");
    if(expected!=attributes)return false;

    if(entry.directory){
        if((!verifyMove||!plan.mergedFolder)&&destination.List(candidate.id).size()!=plan.children.size())return false;
        for(const auto& child:plan.children){
            Entry actual;
            if(!destination.FindTransferChild(candidate.id,child.entry,actual)||((!verifyMove||!child.mergedFolder)&&actual.name!=child.entry.name)||
               !MatchesArchivedEntry(source,child,destination,destination.Stat(actual.id),job,depth+1,validatePartner,verifyMove))return false;
        }
        return true;
    }
    if(plan.linkTarget)return destination.SymbolicLinkTarget(candidate.id)==plan.linkTarget;
    if(entry.posix&&entry.posix->IsSpecial())return true;
    if(entry.compressed){
        const auto& header=candidate.inlineCompression.empty()?candidate.resourceCompressionHeader:candidate.inlineCompression;
        if(header.empty()||HfsPlus::Compression(header,0,{}).Type()!=entry.archivedCompressionType)return false;
    }
    HfsPlus::Bytes expectedBytes(64*1024),actualBytes(expectedBytes.size());
    for(unsigned fork=0;fork<2;++fork){
        for(uint64_t at=0;at<entry.forks[fork].size;){
            if(job.cancelled())throw Storage::Cancelled();
            const auto count=size_t((std::min)(uint64_t(expectedBytes.size()),entry.forks[fork].size-at));
            source.Read(entry.id,fork,at,expectedBytes.data(),count);
            destination.Read(candidate.id,fork,at,actualBytes.data(),count);
            if(memcmp(expectedBytes.data(),actualBytes.data(),count))return false;
            at+=count;
        }
    }
    return true;
}

std::optional<Entry> FindArchiveRestorePartner(Volume& source,const Planned& plan,Volume& destination,
                                              uint32_t parent,Storage::Job& job)
{
    if(plan.archiveLinkGroup.empty()||!destination.Supports().CanCreateHardLinks()||
       (plan.entry.directory&&!destination.HfsPlusDateRange()))return {};
    std::vector<uint32_t> pending{2};
    std::set<uint32_t> visited;
    std::set<uint32_t> ancestors;
    if(plan.entry.directory){
        for(auto current=destination.Stat(parent);ancestors.insert(current.resolvedId).second&&current.id!=2;
            current=destination.Stat(current.parent)){}
    }
    std::optional<Entry> match;
    while(!pending.empty()){
        if(job.cancelled())throw Storage::Cancelled();
        const auto folder=pending.back();
        pending.pop_back();
        for(const auto& child:destination.List(folder)){
            if(job.cancelled())throw Storage::Cancelled();
            if(child.directory&&visited.insert(child.resolvedId).second)pending.push_back(child.id);
            if(child.directory!=plan.entry.directory||ancestors.count(child.resolvedId)||
               child.forks[0].size!=plan.entry.forks[0].size||child.forks[1].size!=plan.entry.forks[1].size)continue;
            if(match&&match->resolvedId==child.resolvedId)continue;
            const auto attributes=destination.GetAttributes(child.id);
            const auto stamp=attributes.find(ArchiveRestoreGroupAttribute);
            if(stamp==attributes.end()||!ArchiveGroupMarker(stamp->second,&plan.archiveLinkGroup))continue;
            auto current=destination.Stat(child.id);
            if(!MatchesArchivedEntry(source,plan,destination,current,job,0,true))continue;
            if(match)return {}; // Divergent copies can leave more than one matching inode.
            match=std::move(current);
        }
    }
    return match;
}

void VerifyMovedArchive(Volume& source,const Planned& plan,Volume& destination,uint32_t parent,
                        bool classic,Storage::Job& job)
{
    if(job.cancelled())throw Storage::Cancelled();
    Entry actual;
    Check(destination.FindTransferChild(parent,plan.entry,actual),
          "The archive destination is missing after publication. Source retained.");
    actual=destination.Stat(actual.id);
    if(!classic){
        Check(MatchesArchivedEntry(source,plan,destination,actual,job,0,false,true),
              ("Restored archive readback differs for "+BrowserPresentation::Utf8(plan.entry.name)+". Source retained.").c_str());
        return;
    }
    Check(actual.directory==plan.entry.directory&&(plan.mergedFolder||actual.name==plan.entry.name),
          "Published classic archive identity differs. Source retained.");
    if(plan.keptFolder){
        const auto& expected=*plan.keptFolder;
        const auto pair=ClassicCopyArchive::RequireVerifiedPair(destination,actual,job);
        Check(actual.finder==expected.finder&&actual.created==expected.created&&actual.modified==expected.modified&&
              actual.additionalDates.backup==expected.additionalDates.backup&&bool(pair)==plan.keptCompanion,
              "Merged classic folder did not retain its metadata. Source retained.");
    }else if(plan.archivedMetadata){
        const auto pair=ClassicCopyArchive::RequireVerifiedPair(destination,actual,job);
        Check(pair&&pair->metadata.entry.name==plan.archivedMetadata->entry.name,
              "Published classic archive lost the original name. Source retained.");
    }
    if(actual.directory){
        for(const auto& child:plan.children)VerifyMovedArchive(source,child,destination,actual.id,true,job);
        return;
    }
    HfsPlus::Bytes expected(64*1024),readback(expected.size());
    for(unsigned fork=0;fork<2;++fork){
        Check(actual.forks[fork].size==plan.entry.forks[fork].size,
              "Published classic archive fork size differs. Source retained.");
        for(uint64_t at=0;at<actual.forks[fork].size;){
            if(job.cancelled())throw Storage::Cancelled();
            const auto count=size_t((std::min)(uint64_t(expected.size()),actual.forks[fork].size-at));
            if(plan.reader)plan.reader(fork,at,expected.data(),count);
            else source.Read(plan.entry.id,fork,at,expected.data(),count);
            destination.Read(actual.id,fork,at,readback.data(),count);
            Check(!memcmp(expected.data(),readback.data(),count),
                  "Published classic archive fork differs. Source retained.");
            at+=count;
        }
    }
}

void VerifyRestoredLinks(Volume& destination,uint32_t parent,const Planned& plan,const std::string& sourceKey,
                         std::map<std::pair<std::string,uint32_t>,uint32_t>& groups)
{
    Entry actual;
    Check(destination.FindTransferChild(parent,plan.entry,actual),
          "An archive item disappeared during link verification. Source retained.");
    if(plan.entry.hardLink&&!plan.archiveLinkGroup.empty()){
        const auto [group,added]=groups.try_emplace({sourceKey,plan.entry.resolvedId},actual.resolvedId);
        Check(added||(actual.hardLink&&actual.resolvedId==group->second),
              "Restored archive aliases do not share the same inode. Source retained.");
    }
    for(const auto& child:plan.children)VerifyRestoredLinks(destination,actual.id,child,sourceKey,groups);
}

bool Transfer(Volume &src, Planned &plan, Volume &dst, uint32_t parent,
              Storage::Job &job, Storage::Progress &progress,CopiedLinks& copies,ForkSource route,bool durable,bool verifyMove) {
    if (job.cancelled()) throw Storage::Cancelled();
    const auto &entry = plan.entry; Entry existing; bool found = false;
    const std::string_view destinationKind=dst.Kind();
    const bool hfsPlus=destinationKind=="HFS+"||destinationKind=="HFSX";
    const auto noteDatePrecision=[&]{
        if(hfsPlus&&entry.datesValid&&entry.fractionalDates)progress.timestampPrecisionReduced=true;
    };
    found=dst.FindTransferChild(parent,entry,existing);
    Check(!plan.projectedName||!found,"An approved classic archive name is now occupied");
    bool track=entry.hardLink&&entry.linkCount>1&&dst.Supports().CanCreateHardLinks();auto key=std::make_pair(src.ResourceKey(),entry.resolvedId);auto prior=copies.find(key);
    std::optional<Entry> previousRestore;
    const Entry* partner=track&&prior!=copies.end()?&prior->second:nullptr;
    if(!partner&&(!entry.directory||!found)){
        previousRestore=FindArchiveRestorePartner(src,plan,dst,parent,job);
        if(previousRestore)partner=&*previousRestore;
    }
    const auto linkCopy=[&](const Entry* approvedDestination){
        if(!approvedDestination||approvedDestination->resolvedId!=partner->resolvedId)
            dst.MakeCopiedHardLinkForTransfer(*partner,parent,entry.name,approvedDestination,job);
        if(track&&prior==copies.end())copies.emplace(key,*partner);
        LinkedProgress(plan,progress);
    };
    if(partner&&entry.directory&&!found){linkCopy(nullptr);job.progress(progress);return true;}
    if (entry.directory) {
        Check(!found || existing.directory, "A file blocks the destination folder");
        auto outgoing=entry;
        bool applySource=false;
        std::optional<ClassicCopyArchive::Pair> previousFolderArchive;
        if(found&&(plan.archivedMetadata||plan.restoredMetadata)){
            const auto choice=job.requestChoice(Storage::CopyPrompt::ArchiveFolderMerge,BrowserPresentation::Utf8(entry.name));
            if(choice!=Storage::ConflictChoice::Replace&&choice!=Storage::ConflictChoice::Skip)
                throw Storage::Cancelled();
            applySource=choice==Storage::ConflictChoice::Replace;
            if(applySource){
                Check(!existing.hardLink,"Applying folder metadata would change other hard-link names");
                if(plan.archivedMetadata){
                    Check(durable,"Replacing archived folder metadata requires classic HFS write recovery");
                    previousFolderArchive=ClassicCopyArchive::RequireVerifiedPair(dst,existing,job);
                }
            }
        }
        if(found&&verifyMove){
            Check(!existing.hardLink&&!entry.hardLink&&plan.archiveLinkGroup.empty(),
                  "Archive Cut/Paste cannot merge linked folders. Source retained.");
            plan.mergedFolder=true;
            if(!applySource){
                plan.keptFolder=existing;
                plan.keptFolder->attributes=dst.GetAttributes(existing.id);
                if(destinationKind=="HFS")
                    plan.keptCompanion=ClassicCopyArchive::RequireVerifiedPair(dst,existing,job).has_value();
            }
        }
        if(!found&&dst.HfsPlusDateRange())NoteArchiveRestoreGroup(outgoing,plan.archiveLinkGroup);
        uint32_t id = found ? existing.id : dst.MakeDirectoryWithAttributes(parent, outgoing);
        if(!found)Storage::OutcomeEvidence::NoteRetainedChange();
        auto expected=dst.Stat(id);bool complete=true;std::exception_ptr failure;
        try{for(auto& child:plan.children)if(!Transfer(src,child,dst,id,job,progress,copies,route,durable,verifyMove))complete=false;}catch(...){failure=std::current_exception();}
        if(!failure&&job.cancelled())failure=std::make_exception_ptr(Storage::Cancelled());
        // Child writes can change directory dates even on Skip or cancellation.
        try{
            const bool apply=!failure&&complete&&(!found||applySource);
            if(found&&!apply)dst.FinalizeDirectory(id,existing,expected,true);
            if(apply){
                dst.FinalizeDirectory(id,outgoing,expected,false,applySource);
                if(previousFolderArchive)ClassicCopyArchive::RemoveCompanion(dst,previousFolderArchive->companion);
                if(plan.archivedMetadata)ClassicCopyArchive::Save(dst,id,*plan.archivedMetadata,job);
                noteDatePrecision();
                job.progress(progress);
            }
        }
        catch(const std::exception& restore){if(failure){std::string original="Copy failed";try{std::rethrow_exception(failure);}catch(const std::exception& e){original=e.what();}catch(...){}throw HfsPlus::Error(original+"; folder metadata restoration failed: "+restore.what());}throw;}
        if(failure)std::rethrow_exception(failure);if(complete&&track&&(!plan.restoredDirectoryLink||!found))copies.emplace(key,dst.Stat(id));return complete;
    }

    Check(!found || !existing.directory, "A folder blocks the destination file");
    using Scope=HfsPlus::Volume::OverwriteScope;
    auto hfsScope=Scope::SharedInode;
    if(found&&!plan.linkTarget&&hfsPlus&&
       (plan.restoredMetadata||std::string_view(src.Kind())=="APFS"||(entry.posix&&entry.posix->IsSpecial()))){
        if(existing.hardLink)hfsScope=Scope::SelectedName;
        else if(existing.symbolicLink)hfsScope=Scope::SymbolicLink;
    }
    std::optional<ClassicCopyArchive::Pair> superseded;
    if(found&&destinationKind=="HFS"&&!plan.archivedMetadata)
        superseded=ClassicCopyArchive::RequireVerifiedPair(dst,existing,job);
    if (found) {
        auto label=BrowserPresentation::Utf8(entry.name);
        if(destinationKind=="MFS")label+=" [version "+std::to_string(entry.version)+"]";
        if(hfsScope==Scope::SelectedName||
           (existing.hardLink&&(destinationKind=="APFS"||(hfsPlus&&plan.linkTarget))))
            label+=" (replace this hard-link name only)";
        else if(hfsScope==Scope::SymbolicLink)label+=" (replace the symbolic link itself)";
        else if(existing.hardLink)label+=" (shared by "+std::to_string(existing.linkCount)+" hard links)";
        auto choice = superseded?job.requestChoice(Storage::CopyPrompt::DiscardClassicArchive,label):job.replace(label);
        if(superseded&&choice!=Storage::ConflictChoice::Replace&&choice!=Storage::ConflictChoice::Skip)
            throw Storage::Cancelled();
        if (choice == Storage::ConflictChoice::Cancel) throw Storage::Cancelled();
        if (choice == Storage::ConflictChoice::Skip) { ++progress.skipped;progress.processed+=entry.forks[0].size+entry.forks[1].size;job.progress(progress);return false; }
    }
    if(found){auto now=dst.Stat(existing.id);Check(now.identity==existing.identity&&now.name==existing.name&&now.directory==existing.directory&&now.modified==existing.modified&&now.forks[0].size==existing.forks[0].size&&now.forks[1].size==existing.forks[1].size,"Destination changed during overwrite confirmation; source retained");}
    if(found&&plan.archivedMetadata)superseded=ClassicCopyArchive::RequireVerifiedPair(dst,existing,job);
    progress.path = BrowserPresentation::Utf8(entry.name); progress.fileBytes = 0;
    progress.fileTotal = entry.forks[0].size + entry.forks[1].size; job.progress(progress);
    if (job.cancelled()) throw Storage::Cancelled();
    if(partner){linkCopy(found?&existing:nullptr);progress.fileBytes=progress.fileTotal;job.progress(progress);return true;}
    const bool storedResource=!plan.reader&&(hfsPlus||destinationKind=="APFS")&&entry.compressed&&
                              entry.compressionResourceSize&&!entry.forks[1].size;
    uint64_t readThrough[2]={};
    auto outgoing=entry;
    if(dst.Supports().CanCreateHardLinks())NoteArchiveRestoreGroup(outgoing,plan.archiveLinkGroup);
    const auto sparse=hfsPlus&&!plan.reader?src.SparseRanges(entry.id):SparseForks{};
    const bool materializesHoles=!sparse[0].empty()||!sparse[1].empty();
    if(materializesHoles)RecordApfsSparseRanges(outgoing,sparse);
    if(hfsPlus&&!plan.reader&&entry.apfsCloneMarked&&!outgoing.attributes.count(ApfsCloneLayoutAttribute)){
        auto layout=src.ApfsCloneLayout(entry.id);
        if(!layout.empty())outgoing.attributes.emplace(ApfsCloneLayoutAttribute,std::move(layout));
    }
    if(storedResource)outgoing.forks[1].size=entry.compressionResourceSize;
    else {outgoing.compressionResourceSize=0;outgoing.resourceCompressionHeader.clear();}outgoing.expectedDestinationIdentity=found?existing.identity:EntryIdentity{};
    outgoing.hfsOverwriteScope=hfsScope;
    if(plan.linkTarget){
        dst.PutSymbolicLink(parent,outgoing,*plan.linkTarget,job);
        noteDatePrecision();
        ++progress.files;
        job.progress(progress);
        return true;
    }
    const auto advance=[&](unsigned fork,uint64_t offset,size_t count){
        if(job.cancelled())throw Storage::Cancelled();
        // The stored stream is validated and copied separately from the logical forks.
        const bool encodedFork=storedResource&&fork==1;
        if(encodedFork)fork=0;
        const auto end=encodedFork?(std::min)(offset+count,entry.forks[0].size):offset+count;
        const auto added=end>readThrough[fork]?end-readThrough[fork]:0;
        readThrough[fork]=(std::max)(readThrough[fork],end);
        progress.fileBytes+=added;
        progress.bytes+=added;
        progress.processed+=added;
        job.progress(progress);
        if(job.cancelled())throw Storage::Cancelled();
    };
    uint32_t published=0;
    if(!found&&!plan.reader&&route==ForkSource::Stream)
        published=dst.TryCloneFileForTransfer(src,outgoing,parent,job);
    if(published){
        LinkedProgress(plan,progress);
        progress.bytes+=progress.fileTotal;
        progress.fileBytes=progress.fileTotal;
        if(track)copies.emplace(key,dst.Stat(published));
        job.progress(progress);
        return true;
    }
    if(hfsPlus&&(std::string_view(src.Kind())=="APFS"||entry.archivedCompressionType)&&
       entry.compressed&&entry.forks[1].size&&entry.inlineCompression.empty()){
        // Check before handing the write to a persistent HFS worker. Both
        // encoders see the same logical fork under the transfer's read lease.
        const HfsPlus::EncodedCompression inlineEncoding(entry.forks[0].size,
            [&](uint64_t offset,void* bytes,size_t count){
                if(plan.reader)plan.reader(0,offset,bytes,count);
                else src.Read(entry.id,0,offset,bytes,count);
            },entry.archivedCompressionType?HfsPlus::CompressionModeForType(entry.archivedCompressionType):
                    HfsPlus::Volume::CompressionMode::Zlib,HfsPlus::EncodedCompression::Layout::InlineOnly);
        if(inlineEncoding.Attribute().empty()&&
           job.requestChoice(Storage::CopyPrompt::Uncompressed,progress.path)!=Storage::ConflictChoice::Replace)
            throw Storage::Cancelled();
        if(job.cancelled())throw Storage::Cancelled();
    }
    const auto publish=[&](bool recovery){
        Check(!superseded||recovery,"Replacing an archive companion requires classic HFS write recovery");
        if(route==ForkSource::MountedWorker&&!plan.reader)
            published=VolumeSession::PutFromMountedSource(dst,parent,outgoing,found,advance);
        else {
            Reader reader=[&](unsigned fork,uint64_t offset,void* p,size_t count){
                if(job.cancelled())throw Storage::Cancelled();
                if(plan.reader)plan.reader(fork,offset,p,count);
                else if(storedResource&&fork==1)src.ReadCompressionResource(entry.id,offset,p,count);
                else src.Read(entry.id,fork,offset,p,count);
                advance(fork,offset,count);
            };
            auto hostPath=plan.reader?std::wstring{}:src.HostPath(entry.id);
            if(hostPath.empty())published=dst.PutForTransfer(parent,outgoing,std::move(reader),found,job);
            else published=dst.PutFromHost(parent,outgoing,std::move(reader),found,hostPath,job);
        }
        if(superseded)ClassicCopyArchive::RemoveCompanion(dst,superseded->companion);
        if(plan.archivedMetadata)ClassicCopyArchive::Save(dst,published,*plan.archivedMetadata,job);
    };
    if(superseded&&!durable)dst.RunWriteBatch(job,publish);
    else publish(durable);
    noteDatePrecision();
    progress.sparseHolesAllocated|=materializesHoles;
    progress.apfsCloneCopiedIndependently|=hfsPlus&&!plan.reader&&entry.apfsCloneMarked;
    const auto remaining=progress.fileTotal-progress.fileBytes;
    progress.fileBytes+=remaining;
    progress.bytes+=remaining;
    progress.processed+=remaining;
    if(track)copies.emplace(key,dst.Stat(published));++progress.files; job.progress(progress); return true;
}
}
std::shared_ptr<Volume> FromMfs(std::unique_ptr<MfsSource> source,const std::string& key,std::shared_ptr<MfsCatalogIds> ids){return TrackWorkerVolume(std::make_shared<Mfs>(std::move(source),key,std::move(ids)));}
std::shared_ptr<Volume> FromHfsPlus(std::unique_ptr<HfsPlus::Source> s, bool write,std::string key,std::wstring externalJournal) {
    return TrackWorkerVolume(std::make_shared<Plus>(std::move(s), write,std::move(key),std::move(externalJournal)));
}
static std::shared_ptr<Volume> OpenMfsImage(const std::wstring& path,bool writable,const std::string& key) {
    auto source=std::make_unique<RecoverableMfsImageSource>();
    std::string error;
    if(!source->Open(path.c_str(),error,writable) &&
       (!writable || !source->Open(path.c_str(),error,false)))throw HfsPlus::Error(error);
    StorageLocation::ValidateImageFile(source->NativeHandle(),key);
    MfsRecovery::Recover(source->NativeHandle(),source->Writable());
    return FromMfs(std::move(source),key);
}
namespace {
std::shared_ptr<Volume> OpenReadOnlyImage(std::unique_ptr<HfsPlus::Source> source,const std::string& key,
                                const std::wstring& externalJournal,const char* unsupported="The DMG contains no supported filesystem")
{
    if(HfsPlus::Volume::Recognizes(*source))
        return FromHfsPlus(std::move(source),false,key,externalJournal);
    if(!externalJournal.empty())return {};
    if(auto apfs=Apfs::Open(source,key))return apfs;
    std::shared_ptr<HfsPlus::Source> disk(std::move(source));
    if(const auto selected=DiskPartitions::FindLegacy(*disk,true)){
        const auto& part=selected->partition;
        auto slice=std::make_shared<DiskImage::Slice>(disk,part.offset,part.length);
        if(selected->filesystem==DiskPartitions::Filesystem::Mfs)
            return FromMfs(std::make_unique<MfsDiskSource>(std::move(slice)),key);
        return OpenClassicSource(std::move(slice),key);
    }
    std::unique_ptr<HfsPlus::Source> opticalSource=std::make_unique<DiskImage::Slice>(disk,0,disk->Size());
    if(auto optical=Optical::Open(opticalSource,key))return optical;
    throw HfsPlus::Error(unsupported);
}
}


// Resolve the filesystem inside an editable image before choosing its worker.
struct NdifVolume {
    VolumeSession::ImageFormat format;
    DiskPartitions::Selection selection;
};

static std::optional<NdifVolume> FindNdifVolume(HfsPlus::Source& source)
{
    using F=VolumeSession::ImageFormat;
    if(!Ndif::IsEditable(source))return {};
    if(const auto plus=DiskPartitions::FindHfsPlus(source))return NdifVolume{F::NdifHfsPlus,*plus};
    if(const auto legacy=DiskPartitions::FindLegacy(source)){
        const auto format=legacy->filesystem==DiskPartitions::Filesystem::Classic?F::NdifClassic:F::NdifMfs;
        return NdifVolume{format,*legacy};
    }
    return {};
}

std::shared_ptr<Volume> OpenEditableNdif(std::unique_ptr<HfsPlus::Source> source,const std::string& key,
                                          const NdifVolume& expected)
{
    const bool classic=expected.format==VolumeSession::ImageFormat::NdifClassic;
    auto backing=classic?Ndif::RecoveryRange(*source):std::nullopt;
    const bool recovered=backing&&ClassicHfsRecovery::RecoverNdif(*backing,source->Writable());
    const auto selected=FindNdifVolume(*source);
    if(!selected||selected->format!=expected.format||selected->selection!=expected.selection)
        throw Storage::MediaChanged("The selected NDIF filesystem or partition changed");
    if(selected->format==VolumeSession::ImageFormat::NdifHfsPlus){
        const bool writable=source->Writable();
        return FromHfsPlus(std::move(source),writable,key);
    }
    std::shared_ptr<HfsPlus::Source> disk(std::move(source));
    const auto& part=selected->selection.partition;
    if(part.index)
        disk=std::make_shared<DiskImage::Slice>(disk,part.offset,part.length,!part.readOnly);
    if(classic){
        if(backing){
            if(part.index){backing->offset+=part.offset;backing->length=part.length;}
            return OpenClassicNdif(std::move(disk),key,*backing,recovered);
        }
        const bool writable=disk->Writable();
        return OpenClassicSource(std::move(disk),key,writable,true);
    }
    return FromMfs(std::make_unique<MfsDiskSource>(std::move(disk),true),key);
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

static std::shared_ptr<Volume> OpenImageImpl(const std::wstring& path,const std::wstring& externalJournal,
                                             bool persistent,const Dmg::Password* password=nullptr,
                                             const Dmg::PrivateKey* privateKey=nullptr,
                                             const std::string& expectedImage={});

std::shared_ptr<Volume> OpenWorkerImage(const WorkerOrigin& origin)
{
    if(!origin.imageMount)
        return OpenImageImpl(origin.path,origin.journal,false,nullptr,nullptr,origin.identity);

    const auto& mount=*origin.imageMount;
    const auto key=origin.identity.empty()?VolumeCoord::ImageKey(origin.path):origin.identity;
    if(mount.format==ImageFormat::Mfs)
        return OpenMfsImage(origin.path,origin.writable,key);
    if(mount.format==ImageFormat::Classic){
        floppy_module_global_init();
        return OpenClassicImage(origin.path,origin.writable,key);
    }

    const bool ndif=mount.format!=ImageFormat::HfsPlus;
    if(ndif)floppy_module_global_init();
    const auto open=[&](bool write){
        auto disk=DiskImage::Reopen(origin.path,ndif?DiskImage::Container::Ndif:DiskImage::Container::Raw,
                                    {write,origin.journal,nullptr,nullptr,nullptr,key});
        if(ndif)
            return OpenEditableNdif(std::move(disk),key,{mount.format,*mount.selection});
        if(DiskPartitions::FindHfsPlus(*disk)!=mount.selection)
            throw Storage::MediaChanged("The selected HFS+ filesystem or partition changed");
        return FromHfsPlus(std::move(disk),write,key,origin.journal);
    };
    try{return open(origin.writable);}
    catch(const HfsPlus::Error&){
        if(!origin.writable)throw;
        return open(false);
    }
}

static std::shared_ptr<Volume> OpenImageImpl(const std::wstring& path,const std::wstring& externalJournal,
                                             bool persistent,const Dmg::Password* password,
                                             const Dmg::PrivateKey* privateKey,const std::string& expectedImage)
{
    using DiskImage::Container;
    const auto key=VolumeCoord::ImageKey(path);
    if(!expectedImage.empty()&&key!=expectedImage)
        throw Storage::MediaChanged("The selected image was replaced.");
    return VolumeCoord::SharedImage(key,[&]()->std::shared_ptr<Volume>{
    const auto reopen=[&](Container type,bool write){
        return DiskImage::Reopen(path,type,{write,externalJournal,nullptr,nullptr,nullptr,key});
    };
    const bool pendingNdif=ClassicHfsRecovery::Pending(path,true);
    const bool pendingClassic=externalJournal.empty()&&ClassicHfsRecovery::Pending(path);
    if(externalJournal.empty()&&MfsRecovery::Pending(path)){
        Check(!pendingClassic&&!pendingNdif,"The image has conflicting filesystem recovery records");
        // Recover before probing the MFS signature or Disk Copy header.
        if(persistent)return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,{},VolumeSession::ImageFormat::Mfs));
        return OpenMfsImage(path,true,key);
    }
    if(pendingClassic){
        Check(!pendingNdif,"The image has conflicting classic HFS recovery records");
        if(persistent)return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,{},VolumeSession::ImageFormat::Classic));
        return OpenClassicImage(path,true,key);
    }
    auto opened=DiskImage::Open(path,{false,externalJournal,password,privateKey,nullptr,key});
    auto& source=opened.source;
    Check(!pendingNdif||opened.container==Container::Ndif,
          "Pending classic HFS recovery requires the original NDIF wrapper and companions");
    if(!source)return {};
    if(opened.container==Container::SparseBundle)
        return OpenReadOnlyImage(std::move(source),key,{},"The sparse bundle contains no supported filesystem");
    if(opened.container==Container::SparseImage)
        return OpenReadOnlyImage(std::move(source),key,externalJournal,"The sparse image contains no supported filesystem");
    if(opened.container==Container::Udif||opened.container==Container::EncryptedDisk)
        return OpenReadOnlyImage(std::move(source),key,externalJournal);
    if(opened.container==Container::Ndif){
        if(pendingNdif){
            // Replay before probing partition/MDB bytes. Leave the record until
            // the owning mount recovers, so it can reopen an unclean volume.
            source.reset();
            source=reopen(Container::Ndif,true);
            const auto backing=Ndif::RecoveryRange(*source);
            Check(backing.has_value(),"Pending classic HFS recovery requires a writable stored NDIF payload");
            ClassicHfsRecovery::RecoverNdif(*backing,source->Writable(),true);
        }
        const auto selected=FindNdifVolume(*source);
        if(pendingNdif)Check(selected&&selected->format==VolumeSession::ImageFormat::NdifClassic,
                         "Pending NDIF recovery did not identify a classic HFS volume");
        if(!selected)return OpenReadOnlyImage(std::move(source),key,externalJournal,"The NDIF image contains no supported filesystem");
        source.reset();
        if(persistent)return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,{},selected->format,selected->selection));
        try{source=reopen(Container::Ndif,true);}
        catch(const HfsPlus::Error&){
            if(pendingNdif)throw;
            source=reopen(Container::Ndif,false);
        }
        return OpenEditableNdif(std::move(source),key,*selected);
    }
    if(externalJournal.empty())
        if(auto tracks=CdMedia::OpenTrackImage(path,source,key))return OpenTrackImages(std::move(*tracks),key);
    if (const auto selected=DiskPartitions::FindHfsPlus(*source)) {
        source.reset();
        if(persistent)
            return TrackWorkerVolume(VolumeSession::MountPersistentImage(
                path,key,externalJournal,VolumeSession::ImageFormat::HfsPlus,*selected));
        const auto open=[&](bool write){
            auto disk=reopen(Container::Raw,write);
            if(DiskPartitions::FindHfsPlus(*disk)!=selected)
                throw Storage::MediaChanged("The selected HFS+ filesystem or partition changed");
            return FromHfsPlus(std::move(disk),write,key,externalJournal);
        };
        try { return open(true); }
        catch(const Storage::MediaChanged&){throw;}
        catch (...) { return open(false); }
    }
    if(externalJournal.empty()){
        if(persistent){
            const auto openApfs=[&](bool writable){return reopen(Container::Raw,writable);};
            if(auto apfs=Apfs::OpenRawBrowserImage(source,key,openApfs))return apfs;
        }else if(auto apfs=Apfs::Open(source,key))return apfs;
        if(auto optical=Optical::Open(source,key))return optical;
    }
    source.reset(); std::unique_ptr<MfsImageSource> mfs(new MfsImageSource); std::string error;
    if (!mfs->Open(path.c_str(), error)) throw HfsPlus::Error(error);
    StorageLocation::ValidateImageFile(reinterpret_cast<HANDLE>(mfs->WorkerFiles().front().second),key);
    if (!MfsImage::Recognizes(*mfs)) {
        if (!externalJournal.empty() || !RecognizesClassicImage(mfs->NativeHandle())) return {};
        mfs.reset();
        if (persistent) return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,{},VolumeSession::ImageFormat::Classic));
        return OpenClassicImage(path,true,key);
    }
    mfs.reset();
    if(persistent)return TrackWorkerVolume(VolumeSession::MountPersistentImage(path,key,{},VolumeSession::ImageFormat::Mfs));
    return OpenMfsImage(path,true,key);
    });
}
std::shared_ptr<Volume> OpenImage(const std::wstring &path,const std::wstring &externalJournal) {
    return OpenImageImpl(path,externalJournal,false);
}
std::shared_ptr<Volume> OpenBrowserImage(const std::wstring &path,const std::wstring &externalJournal,
                                         const Dmg::Password* password,const Dmg::PrivateKey* privateKey,
                                         const std::string& expectedImage) {
    return OpenImageImpl(path,externalJournal,true,password,privateKey,expectedImage);
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
    Check(id != 2 && v.Supports().remove, "Cannot delete the volume root or a read-only item");
    if(v.RemoveTreeAtomically(id,job,deletionProgress))return;
    ApplyRemovalPlan(v,id,job,deletionProgress,[&](uint32_t entry){
        v.Remove(entry);
        Storage::OutcomeEvidence::NoteRetainedChange();
    });
}
void ApplyRemovalPlan(Volume& v,uint32_t id,Storage::Job& job,bool deletionProgress,
                      const std::function<void(uint32_t)>& removeEntry) {
    std::set<uint32_t> parents; Storage::Progress p;if(deletionProgress){p.phase="Deleting";p.utf8=true;}auto plan = Scan(v, id, job, p, parents, 0,true,deletionProgress);
    std::function<void(const Planned &)> remove = [&](const Planned &n) {
        if (job.cancelled()) throw Storage::Cancelled();
        // A directory hard link is a link, not ownership of its backing children.
        Check(!n.entry.hardLink||v.Supports().hardLinks, "Deleting hard links is not supported by this backend");
        if(n.entry.directory&&n.entry.hardLink&&n.children.empty()&&v.Stat(n.entry.id).linkCount==1&&!v.List(n.entry.id).empty()){RemoveTree(v,n.entry.id,job,deletionProgress);return;}
        for (const auto &c : n.children) remove(c);
        if(job.cancelled())throw Storage::Cancelled();
        if(deletionProgress){p.path=BrowserPresentation::Utf8(n.entry.name);p.scanning=false;job.progress(p);}
        removeEntry(n.entry.id);
        if(deletionProgress){++p.files;job.progress(p);}
    };
    std::function<void(const Planned &)> validate=[&](const Planned& n){if(job.cancelled())throw Storage::Cancelled();v.ValidateRemoval(n.entry.id);for(const auto& c:n.children)validate(c);};
    validate(plan);
    remove(plan);
}
std::string Snapshot(Volume& v,uint32_t id,Storage::Job& job,bool requireRemovable,const SnapshotVisitor& visit){
    uint64_t hash=14695981039346656037ULL,count=0;std::set<uint32_t> ancestors;
    auto bytes=[&](const void* p,size_t n){auto b=static_cast<const uint8_t*>(p);while(n--){hash^=*b++;hash*=1099511628211ULL;}};
    std::function<void(uint32_t,unsigned)> walk=[&](uint32_t at,unsigned depth){
        if(job.cancelled())throw Storage::Cancelled();Check(depth<=128&&ancestors.insert(at).second,"Cyclic source folder");
        auto e=v.Stat(at);Check(!e.identity.empty(),"Cannot verify source identity; source items were retained");++count;
        if(visit)visit(v,e);
        if(requireRemovable)v.ValidateRemoval(at);
        auto attributes=v.GetAttributes(at);for(const auto& a:attributes){auto size=a.second.size();bytes(a.first.data(),a.first.size()*2);bytes(&size,sizeof(size));HfsPlus::Bytes buffer(65536);for(uint64_t at=0;at<a.second.size();){auto n=size_t((std::min)(uint64_t(buffer.size()),a.second.size()-at));a.second.Read(at,buffer.data(),n);bytes(buffer.data(),n);at+=n;}}
        const auto object=[&](const ObjectIdentity& value){
            const auto length=uint64_t(value.key.size());
            bytes(&length,sizeof(length));bytes(value.key.data(),value.key.size());
            const auto version=value.version.value_or(0);
            bytes(&version,sizeof(version));
        };
        object(e.identity.primary);object(e.identity.companion);
        if(e.identity.media){
            object({e.identity.media->device,e.identity.media->generation});
        }else object({});
        const auto session=e.identity.session.value_or(0);
        bytes(&session,sizeof(session));bytes(e.name.data(),e.name.size()*2);
        bytes(&e.created,sizeof(e.created));bytes(&e.modified,sizeof(e.modified));bytes(e.finder.data(),e.finder.size());
        const auto backup=e.additionalDates.backup.value_or(0);
        bytes(&backup,sizeof(backup));
        const bool hasPosix=e.posix.has_value();
        bytes(&hasPosix,sizeof(hasPosix));
        if(e.posix){
            bytes(&e.posix->uid,sizeof(e.posix->uid));
            bytes(&e.posix->gid,sizeof(e.posix->gid));
            bytes(&e.posix->mode,sizeof(e.posix->mode));
        }
        bytes(&e.forks[0].size,sizeof(uint64_t));bytes(&e.forks[1].size,sizeof(uint64_t));
        if(e.directory){auto children=v.List(at);std::sort(children.begin(),children.end(),[](const Entry& a,const Entry& b){return a.name==b.name?a.identity<b.identity:a.name<b.name;});for(auto& child:children)walk(child.id,depth+1);}
        else if(const auto target=e.readError.empty()?std::optional<HfsPlus::Bytes>{}:v.SymbolicLinkTarget(at)){
            const auto length=uint64_t(target->size());
            bytes(&length,sizeof(length));bytes(target->data(),target->size());
        }else{v.Read(at,0,0,nullptr,0);if(e.forks[1].size)v.Read(at,1,0,nullptr,0);}
        ancestors.erase(at);
    };
    walk(id,0);return std::to_string(count)+":"+std::to_string(hash);
}
namespace {
void ValidateCopy(Volume &src,uint32_t id,Volume &dst,uint32_t parent){
    Check(dst.Supports().create,"The destination cannot create files");Check(id!=2,"Select files or folders rather than a volume root");
    src.ValidateCopyTo(id,dst,parent);
    if(&src==&dst){std::set<uint32_t> visited;for(uint32_t p=parent;p!=2;){
        Check(p!=id&&visited.insert(p).second,"Cannot copy a folder into itself or its descendants");p=dst.Stat(p).parent;}
        Check(src.Stat(id).parent!=parent,"Source and destination are the same folder");}
}
constexpr auto DirectoryLinkAttribute=u"org.hfvexplorer.hfs-directory-link";
using DirectoryLinkGroups=std::map<std::pair<std::string,uint32_t>,GUID>;

void MaterializeDirectoryLinks(Planned& plan,const std::string& volumeKey,
                               DirectoryLinkGroups& groups,std::string& first)
{
    auto& entry=plan.entry;
    if(entry.directory&&entry.hardLink){
        if(first.empty())first=BrowserPresentation::Utf8(entry.name);
        if(entry.resolvedId&&entry.linkCount>1&&!entry.attributes.count(DirectoryLinkAttribute)){
            const auto group=groups.try_emplace({volumeKey,entry.resolvedId});
            if(group.second)Check(SUCCEEDED(CoCreateGuid(&group.first->second)),
                                  "Cannot allocate a directory hard-link group identifier");
            const auto* guid=reinterpret_cast<const uint8_t*>(&group.first->second);
            HfsPlus::Bytes marker{'H','F','V','D',1,0,0,0};
            marker.insert(marker.end(),guid,guid+sizeof(GUID));
            entry.attributes.emplace(DirectoryLinkAttribute,std::move(marker));
        }
        entry.hardLink=false;
        entry.linkCount=1;
    }
    for(auto& child:plan.children)MaterializeDirectoryLinks(child,volumeKey,groups,first);
}
bool EquivalentDirectoryCopies(Volume& source,const Planned& lhs,const Planned& rhs,
                               Storage::Job& job,bool groupRoot)
{
    if(job.cancelled())throw Storage::Cancelled();
    const auto& a=lhs.entry;
    const auto& b=rhs.entry;
    if((!groupRoot&&a.name!=b.name)||a.directory!=b.directory||a.symbolicLink!=b.symbolicLink||
       a.protectedFile!=b.protectedFile||a.finder!=b.finder||!(a.posix==b.posix)||
       a.datesValid!=b.datesValid||a.apfsTimestamps!=b.apfsTimestamps||a.created!=b.created||
       a.modified!=b.modified||a.additionalDates.changed!=b.additionalDates.changed||
       a.additionalDates.accessed!=b.additionalDates.accessed||
       a.additionalDates.backup.value_or(0)!=b.additionalDates.backup.value_or(0)||a.compressed!=b.compressed||
       a.inlineCompression!=b.inlineCompression||a.resourceCompressionHeader!=b.resourceCompressionHeader||
       a.compressionResourceSize!=b.compressionResourceSize||a.archivedCompressionType!=b.archivedCompressionType||
       a.attributes!=b.attributes||
       lhs.linkTarget!=rhs.linkTarget||
       (!groupRoot&&(a.hardLink||b.hardLink)&&a.resolvedId!=b.resolvedId))return false;
    if(a.directory){
        if(lhs.children.size()!=rhs.children.size())return false;
        for(size_t i=0;i<lhs.children.size();++i)
            if(!EquivalentDirectoryCopies(source,lhs.children[i],rhs.children[i],job,false))return false;
        return true;
    }
    if(lhs.linkTarget||(a.posix&&a.posix->IsSpecial()))return true;
    if(a.resolvedId!=b.resolvedId&&(a.apfsCloneMarked||b.apfsCloneMarked))return false;
    if(source.SparseRanges(a.id)!=source.SparseRanges(b.id))return false;
    const auto readMatches=[&](uint64_t size,const auto& read){
        if(!size)return true;
        HfsPlus::Bytes first(64*1024),second(first.size());
        for(uint64_t offset=0;offset<size;){
            if(job.cancelled())throw Storage::Cancelled();
            const auto count=size_t(std::min<uint64_t>(first.size(),size-offset));
            read(a.id,offset,first.data(),count);
            read(b.id,offset,second.data(),count);
            if(memcmp(first.data(),second.data(),count))return false;
            offset+=count;
        }
        return true;
    };
    for(unsigned fork=0;fork<2;++fork){
        if(a.forks[fork].size!=b.forks[fork].size||
           !readMatches(a.forks[fork].size,[&](auto id,auto offset,auto buffer,auto count){
               source.Read(id,fork,offset,buffer,count);
           }))return false;
    }
    return readMatches(a.compressionResourceSize,[&](auto id,auto offset,auto buffer,auto count){
        source.ReadCompressionResource(id,offset,buffer,count);
    });
}

using RestoredArchiveGroups=std::map<std::pair<std::string,std::string>,Planned*>;

void RestoreArchiveLinks(Volume& source,Planned& plan,RestoredArchiveGroups& groups,Storage::Job& job)
{
    if(job.cancelled())throw Storage::Cancelled();
    if(!plan.archiveLinkGroup.empty()){
        const auto [group,inserted]=groups.try_emplace({source.ResourceKey(),plan.archiveLinkGroup},&plan);
        auto& first=*group->second;
        if(!inserted&&EquivalentDirectoryCopies(source,first,plan,job,true)){
            first.restoredDirectoryLink=plan.restoredDirectoryLink=plan.entry.directory;
            first.entry.hardLink=plan.entry.hardLink=true;
            first.entry.linkCount=plan.entry.linkCount=2;
            plan.entry.resolvedId=first.entry.resolvedId;
        }
    }
    for(auto& child:plan.children)RestoreArchiveLinks(source,child,groups,job);
}

using SavedDirectoryLinks=std::map<std::pair<std::string,HfsPlus::Bytes>,Planned*>;

void RestoreDirectoryLinks(Volume& source,Planned& plan,SavedDirectoryLinks& groups,Storage::Job& job)
{
    auto& entry=plan.entry;
    const auto marker=entry.attributes.find(DirectoryLinkAttribute);
    constexpr std::array<uint8_t,8> header{'H','F','V','D',1,0,0,0};
    if(entry.directory&&marker!=entry.attributes.end()&&marker->second.size()==header.size()+sizeof(GUID)){
        const HfsPlus::Bytes value(marker->second);
        if(std::equal(header.begin(),header.end(),value.begin())){
            const auto group=groups.try_emplace({source.ResourceKey(),value},&plan);
            auto& first=*group.first->second;
            if(!group.second&&EquivalentDirectoryCopies(source,first,plan,job,true)){
                first.restoredDirectoryLink=plan.restoredDirectoryLink=true;
                first.entry.hardLink=entry.hardLink=true;
                first.entry.linkCount=entry.linkCount=2;
                entry.resolvedId=first.entry.resolvedId;
            }
        }
    }
    for(auto& child:plan.children)RestoreDirectoryLinks(source,child,groups,job);
}

struct ClassicCopyLoss {
    bool permissions=false, protection=false, dates=false, links=false, compression=false, allocation=false, attributes=false, special=false;

    void Inspect(Volume& source,const Planned& plan,Storage::Job& job){
        if(job.cancelled())throw Storage::Cancelled();
        const auto& entry=plan.entry;
        permissions|=entry.posix.has_value();
        protection|=entry.directory&&entry.protectedFile;
        if(entry.posix)
            protection|=(entry.posix->protection&~PosixMetadata::UserImmutable)!=0||entry.posix->flags!=0;
        dates|=entry.fractionalDates||entry.additionalDates.accessed.has_value()||entry.additionalDates.changed.has_value()||
               entry.created>UINT32_MAX||entry.modified>UINT32_MAX||entry.additionalDates.backup.value_or(0)>UINT32_MAX;
        links|=entry.hardLink;
        special|=entry.symbolicLink||(entry.posix&&entry.posix->IsSpecial());
        compression|=entry.compressed;
        attributes|=!entry.attributes.empty();
        if(!allocation&&!entry.directory&&!entry.symbolicLink){
            const auto holes=source.SparseRanges(entry.id);
            allocation=entry.apfsCloneMarked||!holes[0].empty()||!holes[1].empty();
        }
        for(const auto& child:plan.children)Inspect(source,child,job);
    }

    std::string Details(const char* separator)const{
        std::string result;
        const auto add=[&](bool loss,const char* label){
            if(!loss)return;
            if(!result.empty())result+=separator;
            result+=label;
        };
        add(permissions,"Unix ownership and permissions");
        add(protection,"BSD protection flags");
        add(dates,"Access time, attribute-change time, or subsecond precision");
        add(links,"Hard-link relationships");
        add(special,"Symbolic links or special files");
        add(compression,"Native filesystem compression");
        add(allocation,"Sparse allocation or clone sharing");
        add(attributes,"Extended attributes or ACLs");
        return result;
    }
};

void AppendArchiveRestoreNotes(Volume& destination,const std::vector<Planned>& plans,uint32_t parent,bool parentExists,
                               const std::u16string& folder,std::string& notes,Storage::Job& job)
{
    for(const auto& plan:plans){
        if(job.cancelled())throw Storage::Cancelled();
        if(notes.size()>=32*1024){
            notes+="Additional entries are not shown.\r\n";
            return;
        }
        const auto& entry=plan.entry;
        const auto path=folder+entry.name;
        Entry existing;
        const bool found=parentExists&&destination.FindTransferChild(parent,entry,existing);
        if(plan.restoredNameChanged&&found)
            notes+="Restored name already exists: "+BrowserPresentation::Utf8(path)+"\r\n";
        if(plan.restoredMetadata&&entry.compressed&&entry.forks[1].size&&entry.inlineCompression.empty())
            notes+="May need an uncompressed copy to preserve the resource fork: "+BrowserPresentation::Utf8(path)+"\r\n";
        if(entry.directory)
            AppendArchiveRestoreNotes(destination,plan.children,found?existing.id:parent,found,path+u"/",notes,job);
    }
}

void ValidatePlans(Volume& dst,const std::vector<Planned>& plans,uint32_t parent,bool parentExists){
 std::vector<Entry> names;for(const auto& plan:plans)names.push_back(plan.entry);
 try{dst.ValidateCopyNames(parent,names,parentExists);}
 catch(const HfsPlus::Error& error){
  if(std::none_of(plans.begin(),plans.end(),[](const auto& plan){return plan.restoredNameChanged;}))throw;
  std::string message=std::string(error.what())+" Restored names in this folder:";
  for(const auto& name:names){
   if(message.size()>=512){message+=" ...";break;}
   message+=" "+BrowserPresentation::Utf8(name.name);
  }
  throw HfsPlus::Error(message);
 }
 for(const auto& plan:plans){
  if(plan.entry.version&&std::string_view(dst.Kind())!="MFS")
   throw HfsPlus::Error("The destination cannot preserve MFS file version "+
                        std::to_string(plan.entry.version)+" for "+BrowserPresentation::Utf8(plan.entry.name)+
                        ". Choose a writable MFS image.");
  Entry found;bool exists=parentExists&&dst.FindTransferChild(parent,plan.entry,found);
  Check(!plan.projectedName||!exists,"An approved classic archive name is now occupied");
  const auto incomingType=plan.entry.posix?plan.entry.posix->special:SpecialFileType::None;
  if(exists){
   const auto existingType=found.posix?found.posix->special:SpecialFileType::None;
   const bool incomingSpecial=incomingType!=SpecialFileType::None;
   const bool existingSpecial=existingType!=SpecialFileType::None;
   const std::string_view kind=dst.Kind();
   if(incomingSpecial!=existingSpecial)
    Check((kind=="HFS+"||kind=="HFSX"||kind=="APFS")&&!found.directory&&!plan.entry.directory&&
          (!found.hardLink||incomingSpecial),
          "The destination cannot replace these file types");
   if(incomingSpecial||existingSpecial)
    Check(!found.protectedFile&&(!existingSpecial||(!found.hardLink&&!found.forks[0].size&&!found.forks[1].size)),
          "The destination special file cannot be replaced");
  }
  if(incomingType!=SpecialFileType::None){
   const std::string_view kind=dst.Kind();
   Check(kind=="HFS+"||kind=="HFSX"||kind=="APFS","The destination cannot preserve a special file");
   Check(!plan.entry.directory&&!plan.entry.symbolicLink&&!plan.entry.compressed&&
         !plan.entry.hardLink&&!plan.entry.forks[0].size&&!plan.entry.forks[1].size,
         "Special file copies cannot contain forks");
  }
  if(plan.linkTarget){
   Check(!exists||(!found.protectedFile&&(dst.SymbolicLinkTarget(found.id).has_value()||
                                        dst.CanReplaceFileWithSymbolicLink(found))),
         "The symbolic link destination is locked or is not a symbolic link");
   dst.ValidateSymbolicLink(plan.entry,*plan.linkTarget);
  }else if(plan.entry.directory&&exists){Check(found.directory,"A file blocks the destination folder");Check(!found.protectedFile&&found.readError.empty(),"The destination folder cannot be merged");ValidatePlans(dst,plan.children,found.id,true);}
  else {dst.ValidateEntry(plan.entry);if(plan.entry.directory)ValidatePlans(dst,plan.children,parent,false);}
 }
}
}
uint32_t Volume::MakeCopiedHardLinkForTransfer(const Entry& copied,uint32_t parent,const std::u16string& name,
                                               const Entry* approvedDestination,Storage::Job& job)
{
    // HFS+ assigns a new catalog ID to the original name when it gains
    // its first alias. Find that name again before adding another alias.
    Entry original;
    Check(FindChild(copied.parent,copied.name,original)&&original.resolvedId==copied.resolvedId&&
          (!copied.protectedFile||(original.protectedFile&&original.posix==copied.posix)),
          "Copied hard-link source disappeared or changed");
    if(approvedDestination){
        Entry destination;
        destination.name=name;
        Entry current;
        Check(FindTransferChild(parent,destination,current)&&current.identity==approvedDestination->identity,
              "The hard-link destination changed after overwrite approval");
    }
    return AddCopiedHardLink(original.id,parent,name,approvedDestination!=nullptr,job);
}

void Volume::WithCopyTransaction(const std::function<void(bool)>& work,const std::string& move)
{
    if(!BeginCopyTransaction(move)){work(false);return;}
    try{work(true);}
    catch(...){FinishCopyTransaction(false);throw;}
    FinishCopyTransaction(true);
}

void Volume::RunWriteBatch(Storage::Job& job,const std::function<void(bool)>& work)
{
    const auto previous=job.progress();
    Storage::Rollback rollback;
    bool retained,grouped=false;
    std::exception_ptr failure;
    {
        Storage::OutcomeEvidence batch;
        try{
            WithCopyTransaction([&](bool active){
                grouped=active;
                work(active);
                if(active&&job.cancelled())throw Storage::Cancelled();
            });
        }catch(...){failure=std::current_exception();}
        rollback=batch.RollbackState();
        retained=batch.RetainedChanges();
    }
    Storage::OutcomeEvidence::NoteRollback(rollback);
    if(failure&&grouped&&rollback==Storage::Rollback::Restored)job.progress(previous);
    else if(retained)Storage::OutcomeEvidence::NoteRetainedChange();
    if(failure)std::rethrow_exception(failure);
}

bool CopyMany(const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>>& sources,Volume& dst,uint32_t parent,Storage::Job& job,const TransferAdapter& adapter,const std::vector<ForkSource>& routes,bool moving,bool batchActive){
    Check(routes.empty()||(!adapter&&routes.size()==sources.size()),"Invalid transfer routes");
    Check(dst.Supports().create,"The destination cannot create files");Storage::Progress progress;progress.fileCopy=progress.utf8=true;progress.phase="Scanning selected files";
    std::vector<Planned> plans;
    for(const auto& source:sources){ValidateCopy(*source.first,source.second,dst,parent);std::set<uint32_t> ancestors;
        plans.push_back(Scan(*source.first,source.second,job,progress,ancestors,0,false,true,adapter));
        const std::string_view kind=source.first->Kind();
        const std::string_view target=dst.Kind();
        const bool archiveSource=moving&&kind=="HFS"&&
            ClassicCopyArchive::ValidateCut(*source.first,plans.back().entry,job);
        if(archiveSource)
            Check(!adapter&&(target=="HFS"||target=="APFS"||dst.HfsPlusDateRange()),
                  "The destination cannot preserve the archive during Cut/Paste. Source retained.");
        if(!adapter){
            if(kind=="HFS"&&(target=="APFS"||dst.HfsPlusDateRange()||(archiveSource&&target=="HFS")))
                RestoreClassicArchive(*source.first,plans.back(),progress,job,target=="HFS");
            PrepareCopyTimestamps(plans.back(),dst,kind=="APFS",kind=="HFS+"||kind=="HFSX");
        }
    }
    if(dst.Supports().CanCreateHardLinks()){
        RestoredArchiveGroups groups;
        for(size_t i=0;i<plans.size();++i)
            RestoreArchiveLinks(*sources[i].first,plans[i],groups,job);
    }
    std::string firstLinkedFolder;
    if(std::string_view(dst.Kind())=="APFS"){
        DirectoryLinkGroups groups;
        for(size_t i=0;i<plans.size();++i)
            MaterializeDirectoryLinks(plans[i],sources[i].first->ResourceKey(),groups,firstLinkedFolder);
    }
    if(dst.HfsPlusDateRange()&&!adapter){
        SavedDirectoryLinks saved;
        for(size_t i=0;i<plans.size();++i)
            if(std::string_view(sources[i].first->Kind())=="APFS")
                RestoreDirectoryLinks(*sources[i].first,plans[i],saved,job);
    }
    Check(!moving||firstLinkedFolder.empty(),
          "APFS cannot preserve directory hard links during Cut/Paste. Use Copy to keep the linked source folders.");
    bool archiveCopy=false;
    const auto containsArchive=[](auto&& recurse,const Planned& plan)->bool {
        if(plan.restoredMetadata)return true;
        for(const auto& child:plan.children)if(recurse(recurse,child))return true;
        return false;
    };
    const bool carried=moving&&std::string_view(dst.Kind())=="HFS"&&
        std::any_of(plans.begin(),plans.end(),[&](const auto& plan){return containsArchive(containsArchive,plan);});
    if(carried)
        Check(std::all_of(sources.begin(),sources.end(),[](const auto& item){return std::string_view(item.first->Kind())=="HFS";}),
              "Move classic archives separately from other formats. Source retained.");
    if(std::string_view(dst.Kind())=="HFS"&&!adapter&&!carried){
        ClassicCopyLoss loss;
        for(size_t i=0;i<plans.size();++i){
            auto& source=*sources[i].first;
            const std::string_view kind=source.Kind();
            if(kind=="APFS"||kind=="HFS+"||kind=="HFSX")loss.Inspect(source,plans[i],job);
        }
        const auto details=loss.Details(moving?", ":"\r\n");
        auto projected=plans;
        for(auto& plan:projected)PrepareClassicArchive(plan);
        std::string preview;
        ProjectArchiveNames(projected,dst,parent,true,{},preview,job);
        ValidatePlans(dst,projected,parent,true);
        std::optional<ArchiveSpace> space;
        auto archiveProgress=progress;
        std::string spaceError,spaceDetails;
        if(!preview.empty()||!details.empty()){
            try{
                const auto block=dst.ClassicAllocationBlockSize();
                Check(block!=0,"Cannot determine classic HFS allocation block size");
                space=ArchiveSpace{0,0,0,0,block,dst.FreeBytes()};
                ArchiveLinkGroups groups;
                for(size_t i=0;i<projected.size();++i){
                    ValidateArchiveNames(projected[i]);
                    RecordArchiveLayout(*sources[i].first,projected[i],groups,archiveProgress,job);
                    space->Count(dst,parent,projected[i],job);
                }
                spaceDetails=space->Details();
            }catch(const HfsPlus::Error& error){
                if(!preview.empty())throw;
                spaceError=error.what();
                spaceDetails="\r\n\r\nArchive Copy unavailable: "+spaceError;
            }
        }
        if(!preview.empty()){
            const auto prompt=moving?Storage::CopyPrompt::ClassicArchiveMoveNames:Storage::CopyPrompt::ClassicArchiveNames;
            if(job.requestChoice(prompt,preview+spaceDetails)!=Storage::ConflictChoice::Replace)
                throw Storage::Cancelled();
            archiveCopy=true;
        }else if(!details.empty()){
            const auto choice=job.requestChoice(moving?Storage::CopyPrompt::ClassicArchiveMove:Storage::CopyPrompt::ClassicMetadataLoss,
                                                details+spaceDetails);
            if(choice==Storage::ConflictChoice::ReplaceAll){
                archiveCopy=true;
            }else if(choice!=Storage::ConflictChoice::Replace)throw Storage::Cancelled();
            else if(moving)throw HfsPlus::Error("Cut/Paste requires Archive Copy to preserve this selection. Source retained.");
        }
        if(archiveCopy){
            if(!spaceError.empty())throw HfsPlus::Error(spaceError);
            Check(space->Fits(),"Insufficient free space for the classic HFS archive");
            progress=std::move(archiveProgress);
            plans=std::move(projected);
        }
    }
    ValidatePlans(dst,plans,parent,true);
    const bool verifyArchiveMove=moving&&(archiveCopy||carried||
        std::any_of(plans.begin(),plans.end(),[&](const auto& plan){return containsArchive(containsArchive,plan);}));

    const bool restoring=std::string_view(dst.Kind())!="HFS"&&!adapter&&
        std::any_of(plans.begin(),plans.end(),[&](const auto& plan){return containsArchive(containsArchive,plan);});
    if(restoring){
        std::string notes;
        if(!firstLinkedFolder.empty())
            notes="APFS will restore directory hard links as independent folders. First linked folder: "+firstLinkedFolder+"\r\n";
        AppendArchiveRestoreNotes(dst,plans,parent,true,{},notes,job);
        if(!notes.empty()&&job.requestChoice(Storage::CopyPrompt::ClassicArchiveRestorePreview,notes)!=Storage::ConflictChoice::Replace)
            throw Storage::Cancelled();
    }else if(!firstLinkedFolder.empty()&&
             job.requestChoice(Storage::CopyPrompt::IndependentFolders,firstLinkedFolder)!=Storage::ConflictChoice::Replace){
        throw Storage::Cancelled();
    }
    progress.scanning=false;progress.byteTransfer=true;progress.phase="Copying";job.progress(progress);bool complete=true;
    CopiedLinks copies;
    const auto transfer=[&](bool durable){
        Check(!(archiveCopy||carried)||durable,"Archive Copy requires classic HFS write recovery");
        for(size_t i=0;i<plans.size();++i){
            const auto route=routes.empty()?ForkSource::Stream:routes[i];
            if(!Transfer(*sources[i].first,plans[i],dst,parent,job,progress,copies,route,durable,verifyArchiveMove))complete=false;
        }
        if(verifyArchiveMove&&complete){
            progress.phase="Verifying archive before source cleanup";
            job.progress(progress);
            const bool classic=std::string_view(dst.Kind())=="HFS";
            std::map<std::pair<std::string,uint32_t>,uint32_t> groups;
            for(size_t i=0;i<plans.size();++i){
                VerifyMovedArchive(*sources[i].first,plans[i],dst,parent,classic,job);
                if(!classic)VerifyRestoredLinks(dst,parent,plans[i],sources[i].first->ResourceKey(),groups);
            }
        }
    };
    if(!batchActive&&(archiveCopy||carried||plans.size()>1||moving||(!plans.empty()&&plans.front().entry.directory)))
        dst.RunWriteBatch(job,transfer);
    else transfer(batchActive);
    return complete;
}
bool Copy(Volume &src,uint32_t id,Volume &dst,uint32_t parent,Storage::Job& job,bool move){
    Check(!move||src.Writable(),"The source is read-only");
    // The public single-item helper borrows its caller-owned adapter.
    std::shared_ptr<Volume> borrowed(&src,[](Volume*){});
    bool complete=CopyMany({{borrowed,id}},dst,parent,job,{},{},move);
    if(move&&complete)RemoveTree(src,id,job);return complete;
}
}
