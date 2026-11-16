// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacBrowserModel.h"
#include "HfsPlusCd.h"
#include "BrowserPresentation.h"
#include <set>
#include <algorithm>

namespace MacBrowser {
std::wstring Wide(const std::u16string &s) { return {s.begin(), s.end()}; }
std::u16string U16(const std::wstring &s) { return {s.begin(), s.end()}; }
namespace {
void Check(bool ok, const char *message) { if (!ok) throw HfsPlus::Error(message); }
class Plus final : public Volume {
    std::unique_ptr<HfsPlus::Source> source_;
    std::unique_ptr<HfsPlus::Volume> volume_;
    bool writable_, usable_ = true;
    template<class F> auto Mutate(F work) -> decltype(work()) {
        if (!usable_) throw HfsPlus::Error("Close and reopen this volume before further access");
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
    Plus(std::unique_ptr<HfsPlus::Source> source, bool write)
        : source_(std::move(source)), volume_(new HfsPlus::Volume(*source_, write)), writable_(write) {}
    std::u16string Name() const override { return volume_->Name(); }
    const char *Kind() const override { return volume_->IsHfsX() ? "HFSX" : "HFS+"; }
    std::string Description()const override {
        return std::string(Kind())+(CaseSensitive()?" (case-sensitive)":" (case-insensitive)")+
            (volume_->Journaled()?", journaled":", non-journaled");
    }
    bool Writable() const override { return usable_ && volume_->Writable(); }
    void Reload() override {
        try { volume_.reset(new HfsPlus::Volume(*source_, writable_)); usable_=true; }
        catch (...) { usable_=false; throw; }
    }
    bool CaseSensitive() const override { return volume_->CaseSensitive(); }
    uint64_t FreeBytes() const override { return volume_->FreeBytes(); }
    std::vector<Entry> List(uint32_t id) override {
        Ready();std::vector<Entry> out;for(const auto& e:volume_->List(id))out.push_back(Describe(e));return out;
    }
    Entry Stat(uint32_t id) override { Ready(); return Describe(volume_->Stat(id)); }
    void Read(uint32_t id, unsigned fork, uint64_t offset, void *p, size_t n) override {
        Ready(); Check(volume_->Read(id, fork, offset, p, n) == n, "Short Macintosh fork read");
    }
    uint32_t MakeDirectory(uint32_t p, const std::u16string &s) override {
        return Mutate([&] { return volume_->MakeDirectory(p, s); });
    }
    uint32_t Put(uint32_t p, const Entry &e, Reader read, bool replace) override {
        std::array<uint32_t,2> dates{e.created,e.modified};
        return Mutate([&] { return volume_->Put(p, e.name, e.forks[0].size, e.forks[1].size, read, &e.finder, replace,e.datesValid?&dates:nullptr); });
    }
    void Rename(uint32_t id, uint32_t p, const std::u16string &s) override {
        Mutate([&] { volume_->Rename(id, p, s); });
    }
    void Remove(uint32_t id) override { Mutate([&] { volume_->Remove(id); }); }
    void ValidateRemoval(uint32_t id)override{Ready();volume_->ValidateRemoval(id);}
};
class Mfs final : public Volume {
    std::unique_ptr<MfsImageSource> source_;
    MfsImage image_;
    std::map<uint32_t, size_t> entries_;
    Entry EntryAt(size_t i) {
        const auto &e = image_.Entries().at(i); Entry out;
        out.id = uint32_t(i + 3); out.parent = 2; out.name = U16(MfsDisplayName(e.name));
        out.version=e.version;
        out.created = e.created; out.modified = e.modified;out.datesValid=true; out.protectedFile = (e.flags & 1) != 0;
        memcpy(out.finder.data(), e.finderInfo, 16);
        for (unsigned f = 0; f < 2; ++f) out.forks[f].size = e.forks[f].length;
        return out;
    }
    void Refuse() { throw HfsPlus::Error("MFS disk images are read-only"); }
public:
    explicit Mfs(std::unique_ptr<MfsImageSource> source) : source_(std::move(source)) {
        if (!image_.Open(*source_)) throw HfsPlus::Error(image_.Error());
        for (size_t i = 0; i < image_.Entries().size(); ++i) entries_[uint32_t(i + 3)] = i;
    }
    std::u16string Name() const override { return U16(MfsDisplayName(image_.VolumeName())); }
    const char *Kind() const override { return "MFS"; }
    bool Writable() const override { return false; }
    bool CaseSensitive() const override { return false; }
    uint64_t FreeBytes() const override { return image_.FreeBytes(); }
    std::vector<Entry> List(uint32_t parent) override {
        Check(parent == 2, "MFS has no subdirectories"); std::vector<Entry> out;
        for (const auto &e : entries_) out.push_back(EntryAt(e.second)); return out;
    }
    Entry Stat(uint32_t id) override {
        if (id == 2) { Entry e; e.id = 2; e.parent = 1; e.directory = true; e.name = Name(); return e; }
        return EntryAt(entries_.at(id));
    }
    void Read(uint32_t id, unsigned fork, uint64_t offset, void *p, size_t n) override {
        size_t got = 0; if (!image_.ReadFork(entries_.at(id), fork, offset, p, n, got)) throw HfsPlus::Error(image_.Error()); Check(got == n, "Short MFS fork read");
    }
    uint32_t MakeDirectory(uint32_t, const std::u16string &) override { Refuse(); return 0; }
    uint32_t Put(uint32_t, const Entry &, Reader, bool) override { Refuse(); return 0; }
    void Rename(uint32_t, uint32_t, const std::u16string &) override { Refuse(); }
    void Remove(uint32_t) override { Refuse(); }
};
bool Equal(Volume &v, const std::u16string &a, const std::u16string &b) {
    return HfsPlus::CompareNames(a, b, v.CaseSensitive()) == 0;
}
struct Planned { Entry entry; std::vector<Planned> children; };
Planned Scan(Volume &v, uint32_t id, Storage::Job &job, Storage::Progress &p,
             std::set<uint32_t> &ancestors, unsigned depth) {
    if (job.cancelled()) throw Storage::Cancelled();
    Check(depth <= 128 && ancestors.insert(id).second, "Recursive folder link or excessive folder depth");
    Planned out; out.entry = v.Stat(id);
    Check(out.entry.readError.empty(), out.entry.readError.c_str());
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
    for (auto &e : dst.List(parent)) if (Equal(dst, e.name, entry.name)) { existing = e; found = true; break; }
    if (entry.directory) {
        Check(!found || existing.directory, "A file blocks the destination folder");
        uint32_t id = found ? existing.id : dst.MakeDirectory(parent, entry.name);
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
    dst.Put(parent, entry, [&](unsigned fork, uint64_t offset, void *p, size_t n) {
        if (job.cancelled()) throw Storage::Cancelled();
        src.Read(entry.id, fork, offset, p, n);
        progress.fileBytes += n; progress.bytes += n;progress.processed+=n;job.progress(progress);
    }, found);
    ++progress.files; job.progress(progress); return true;
}
}
std::shared_ptr<Volume> FromHfsPlus(std::unique_ptr<HfsPlus::Source> s, bool write) {
    return std::make_shared<Plus>(std::move(s), write);
}
std::shared_ptr<Volume> OpenImage(const std::wstring &path) {
    DWORD attributes=GetFileAttributesW(path.c_str());if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))return {};
    auto source = HfsPlus::OpenImage(path.c_str(), false);
    if (HfsPlus::Volume::Recognizes(*source)) {
        source.reset();
        try { return FromHfsPlus(HfsPlus::OpenImage(path.c_str(), true), true); }
        catch (...) { return FromHfsPlus(HfsPlus::OpenImage(path.c_str(), false), false); }
    }
    source.reset(); std::unique_ptr<MfsImageSource> mfs(new MfsImageSource); std::string error;
    if (!mfs->Open(path.c_str(), error)) throw HfsPlus::Error(error);
    if (!MfsImage::Recognizes(*mfs)) return {};
    return std::make_shared<Mfs>(std::move(mfs));
}
std::shared_ptr<Volume> OpenCd(int drive, std::shared_ptr<::MediaImageSource> media) {
    auto source = HfsPlus::OpenCd(drive, std::move(media));
    if (!HfsPlus::Volume::Recognizes(*source)) return {};
    try { return FromHfsPlus(std::move(source), false); }
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
void RemoveTree(Volume &v, uint32_t id, Storage::Job &job) {
    Check(id != 2 && v.Writable(), "Cannot delete the volume root or a read-only item");
    std::set<uint32_t> parents; Storage::Progress p; auto plan = Scan(v, id, job, p, parents, 0);
    std::function<void(const Planned &)> remove = [&](const Planned &n) {
        if (job.cancelled()) throw Storage::Cancelled();
        // A directory hard link is a link, not ownership of its backing children.
        Check(!n.entry.hardLink, "Deleting hard links is not supported");
        for (const auto &c : n.children) remove(c);
        v.Remove(n.entry.id);
    };
    std::function<void(const Planned &)> validate=[&](const Planned& n){v.ValidateRemoval(n.entry.id);for(const auto& c:n.children)validate(c);};
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
