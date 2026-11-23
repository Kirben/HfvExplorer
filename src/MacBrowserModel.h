// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace DiskPartitions { struct Selection; }
namespace ClassicHfsRecovery { struct ImageRange; }
namespace MacBrowser { struct WorkerOrigin; }
#include "HfsPlus.h"
#include "EntryIdentity.h"
#include "MfsImage.h"
#include <map>
#include "StorageWorker.h"
extern "C" {
#include "hfs/libhfs/hfs.h"
}

class MediaImageSource;
namespace Dmg { class Password; class PrivateKey; }
namespace MacBrowser {
struct DetectedVolumeError : HfsPlus::Error { using HfsPlus::Error::Error; };
struct Fork { uint64_t size = 0; };
using HfsPlus::SparseRange;
using SparseForks = std::array<std::vector<SparseRange>,2>;
// Presentation and transfer data: deliberately independent of catalog layouts,
// allocation extents, filesystem encodings and journal structures.
struct Entry {
    uint32_t id = 0, parent = 0, resolvedId = 0, linkCount = 1;
    std::u16string name;
    bool directory = false, protectedFile = false, compressed = false, hardLink = false;
    std::string readError;
    Fork forks[2];
    HfsPlus::Bytes inlineCompression; // Exact inline decmpfs bytes supplied by Stat.
    HfsPlus::Bytes resourceCompressionHeader;
    uint64_t compressionResourceSize=0;
    uint32_t archivedCompressionType=0;
    bool WritesStoredCompressionResource() const {
        return compressed&&compressionResourceSize&&forks[1].size==compressionResourceSize&&
               !resourceCompressionHeader.empty();
    }
    std::array<uint8_t,32> finder{};
    uint64_t created = 0, modified = 0; // canonical seconds since 1904, including expanded dates
    HfsPlus::AdditionalFileDates additionalDates;
    bool datesValid = false;
    bool apfsCloneMarked = false;
    std::optional<std::array<uint64_t,4>> apfsTimestamps; // On-disk creation, modification, change and access times.
    bool fractionalDates = false; // At least one stored timestamp has subsecond precision.
    HfsPlus::Attributes attributes;
    bool attributesKnown = false; // Copy requests carry an explicit source metadata snapshot.
    unsigned version = 0; // optional legacy file version, not part of its identity
    bool portableContainer = false; // Encoded Macintosh metadata is already in the data stream.
    EntryIdentity expectedDestinationIdentity; // transient overwrite approval; never filesystem metadata
    EntryIdentity identity;
    bool symbolicLink = false;
    std::optional<PosixMetadata> posix;
    HfsPlus::Volume::OverwriteScope hfsOverwriteScope = HfsPlus::Volume::OverwriteScope::SharedInode;
};
void ValidateHfsPlusCopyNames(const std::vector<Entry>& entries, bool caseSensitive);
void ValidateClassicCopyNames(const std::vector<Entry>& entries);
std::string ClassicCopyNameKey(const std::u16string& name);
std::u16string ProjectClassicCopyName(const std::u16string& name,unsigned suffix=0);
void ValidateMfsCopyNames(const std::vector<Entry>& entries);
bool IsHfsPlusSymlinkReplacementFile(const Entry& entry);
struct Capabilities {
    bool read = true, create = false, rename = false, remove = false;
    bool unicodeNames = true, resourceForks = true, createFolders = true;
    bool writable = false;
    bool attributes = false, basicMetadata = false, classicProperties = false;
    bool compression = false, hardLinks = false, finderMetadata = false;

    bool CanEditAttributes() const { return writable && attributes; }
    bool CanEditBasicMetadata() const { return writable && basicMetadata; }
    bool CanEditClassicProperties() const { return writable && classicProperties; }
    bool CanEditCompression() const { return writable && compression; }
    bool CanCreateHardLinks() const { return writable && hardLinks; }
    void RestrictWrites() { writable = create = rename = remove = false; }
};
using Reader = std::function<void(unsigned, uint64_t, void *, size_t)>;
inline constexpr auto ApfsTimesAttribute=u"org.hfvexplorer.apfs-timestamps";
inline constexpr auto ApfsSparseLayoutAttribute=u"org.hfvexplorer.apfs-sparse-ranges";
inline constexpr auto ApfsCloneLayoutAttribute=u"org.hfvexplorer.apfs-clone-layout";
SparseForks ValidateApfsSparseLayout(const Entry&,const Reader&,Storage::Job&);
struct SharedCloneRange { uint64_t offset,partnerOffset,length; };
std::vector<SharedCloneRange> MatchApfsCloneRanges(const Entry&,const Reader&,
                                                  const Entry&,const Reader&,Storage::Job&);
class Volume;
// Adapt a scanned entry before destination validation. Return a reader for
// transformed forks, or an empty reader to use the original storage adapter.
using TransferAdapter = std::function<Reader(Volume &, Entry &, Storage::Job &, bool root)>;
// File operations and browsing have separate queues. Native backends guard
// calls and leases; callers reserve volumes before a multi-call transaction.
// GUI code holds IDs and copied entries, never filesystem catalog records.
class Volume {
public:
    virtual ~Volume() = default;
    virtual std::wstring HostPath(uint32_t)const{return {};}
    virtual std::string ResourceKey()const{return "adapter:"+std::to_string(reinterpret_cast<uintptr_t>(this));}
    virtual std::u16string Name() const = 0;
    virtual const char *Kind() const = 0;
    virtual std::string Description() const { return Kind(); }
    virtual bool Writable() const = 0;
    virtual bool Journaled() const { return false; }
    virtual int CdDrive() const { return -1; }
    virtual Capabilities Supports() const {
        Capabilities out; out.writable = out.create = out.rename = out.remove = Writable(); return out;
    }
    virtual bool CaseSensitive() const = 0;
    virtual uint64_t ClassicAllocationBlockSize() const {return 0;}
    virtual uint64_t FreeBytes() const = 0;
    virtual void Reload() {}
    virtual void Close() {}
    virtual void ValidateMedia() {}
    virtual void ReleaseMedia() {} // Explicit physical-media release, including idle clipboard references.
    virtual void ValidateCopyTo(uint32_t, Volume &, uint32_t) {}
    virtual uint32_t TryCloneFileForTransfer(Volume&,const Entry&,uint32_t,Storage::Job&){return 0;}
    virtual bool CanMoveFrom(const Volume&)const{return false;}
    virtual bool CanMoveEntryFrom(const Volume& source,uint32_t,uint32_t){return CanMoveFrom(source);}
    virtual void MoveHereForJob(Volume& source,uint32_t id,uint32_t parent,const Entry& entry,Storage::Job&){
        MoveHere(source,id,parent,entry);
    }
    virtual void MoveHere(Volume&,uint32_t,uint32_t,const Entry&){throw HfsPlus::Error("This backend cannot move entries in place");}
    virtual std::optional<HfsPlus::Bytes> SymbolicLinkTarget(uint32_t){return {};}
    virtual bool CanReplaceFileWithSymbolicLink(const Entry&)const{return false;}
    virtual void ValidateSymbolicLink(const Entry&,const HfsPlus::Bytes&)const{
        throw HfsPlus::Error("The destination cannot copy symbolic links");
    }
    virtual void PutSymbolicLink(uint32_t,const Entry&,const HfsPlus::Bytes&,Storage::Job&){
        throw HfsPlus::Error("The destination cannot copy symbolic links");
    }
    virtual uint32_t MakeHardLink(uint32_t,uint32_t,const std::u16string&,bool=false){throw HfsPlus::Error("This format cannot create hard links");}
    virtual uint32_t MakeHardLinkForTransfer(uint32_t source,uint32_t parent,const std::u16string& name,
                                            bool replace,Storage::Job&){
        return MakeHardLink(source,parent,name,replace);
    }
    virtual uint32_t AddCopiedHardLink(uint32_t source,uint32_t parent,const std::u16string& name,
                                       bool replace,Storage::Job& job){
        return MakeHardLinkForTransfer(source,parent,name,replace,job);
    }
    virtual uint32_t MakeCopiedHardLinkForTransfer(const Entry& copied,uint32_t parent,const std::u16string& name,
                                                  const Entry* approvedDestination,Storage::Job& job);
    virtual void SetCompressionMethod(uint32_t id,HfsPlus::Volume::CompressionMode method,std::function<void(uint64_t,uint64_t)> progress){if(method!=HfsPlus::Volume::CompressionMode::Zlib&&method!=HfsPlus::Volume::CompressionMode::Uncompressed)throw HfsPlus::Error("This format cannot create this compression method");SetCompression(id,method!=HfsPlus::Volume::CompressionMode::Uncompressed,std::move(progress));}
    virtual void SetCompressionMethodForJob(uint32_t id,HfsPlus::Volume::CompressionMode mode,
                                            std::function<void(uint64_t,uint64_t)> progress,Storage::Job&){
        SetCompressionMethod(id,mode,std::move(progress));
    }
    virtual void SetCompression(uint32_t,bool,std::function<void(uint64_t,uint64_t)>){throw HfsPlus::Error("This format cannot create compression");}
    virtual HfsPlus::Attributes GetAttributes(uint32_t){return {};}
    virtual void SetAttributes(uint32_t,const HfsPlus::Attributes&){throw HfsPlus::Error("This format cannot store extended attributes or Mac ACLs");}
    virtual void SetAttributesForJob(uint32_t id,const HfsPlus::Attributes& attributes,Storage::Job&) {
        SetAttributes(id,attributes);
    }
    virtual void ValidateCopyNames(uint32_t,const std::vector<Entry>&,bool) {}
    virtual void FinalizeDirectory(uint32_t,const Entry&,const Entry&,bool,bool=false) {}
    virtual uint32_t MakeDirectoryWithAttributes(uint32_t parent,const Entry& e){ValidateEntry(e);return MakeDirectory(parent,e.name);}
    virtual void ValidateName(const std::u16string& name) const { HfsPlus::Normalize(name,CaseSensitive()); }
    virtual std::optional<std::pair<uint64_t,uint64_t>> HfsPlusDateRange() const { return {}; }
    virtual void ValidateEntry(const Entry &e) const { ValidateName(e.name);if(!e.attributes.empty()&&!Supports().attributes)throw HfsPlus::Error("The destination format cannot preserve extended attributes or Mac ACLs; source retained"); }
    virtual bool BeginCopyTransaction(const std::string&) { return false; }
    virtual void FinishCopyTransaction(bool) {}
    virtual void PrepareCopyTransaction() { throw HfsPlus::Error("This volume cannot prepare a coordinated move"); }
    virtual void WithCopyTransaction(const std::function<void(bool)>& work,const std::string& move={});
    void RunWriteBatch(Storage::Job&,const std::function<void(bool)>&);
    virtual bool RemoveTreeAtomically(uint32_t,Storage::Job&,bool) { return false; }
    virtual void ValidateRemoval(uint32_t) { if(!Writable())throw HfsPlus::Error("The source is read-only"); }
    virtual std::vector<Entry> List(uint32_t parent = 2) = 0;
    virtual Entry Stat(uint32_t id) = 0;
    virtual std::string PropertiesDetails(uint32_t) { return {}; }
    virtual bool FindChild(uint32_t parent,const std::u16string& name,Entry& found){for(auto& e:List(parent))if(HfsPlus::CompareNames(e.name,name,CaseSensitive())==0){found=e;return true;}return false;}
    virtual void Read(uint32_t id, unsigned fork, uint64_t offset, void *, size_t) = 0;
    virtual SparseForks SparseRanges(uint32_t) { return {}; }
    virtual HfsPlus::Bytes ApfsCloneLayout(uint32_t) { return {}; }
    virtual void ReadCompressionResource(uint32_t, uint64_t, void*, size_t) {
        throw HfsPlus::Error("This volume cannot read a stored compression resource");
    }
    virtual bool FindTransferChild(uint32_t parent,const Entry& entry,Entry& found){return FindChild(parent,entry.name,found);}
    virtual bool GetClassicProperties(uint32_t, hfsdirent&) { return false; }
    virtual void SetClassicProperties(uint32_t, const hfsdirent&) { throw HfsPlus::Error("Classic Finder properties are unavailable"); }
    virtual void SetBasicMetadata(uint32_t,const Entry&){throw HfsPlus::Error("This format has no basic metadata editor");}
    virtual uint32_t MakeDirectory(uint32_t parent, const std::u16string &) = 0;
    virtual uint32_t Put(uint32_t parent, const Entry &, Reader, bool replace) = 0;
    virtual uint32_t PutForTransfer(uint32_t parent,const Entry& entry,Reader reader,bool replace,Storage::Job&) {
        return Put(parent,entry,std::move(reader),replace);
    }
    virtual uint32_t PutFromHost(uint32_t parent,const Entry& entry,Reader reader,bool replace,
                                 const std::wstring&,Storage::Job&) {
        return Put(parent,entry,std::move(reader),replace);
    }
    // A backend that can read the original data fork locally reports those
    // bytes separately so the caller can update progress and cancel.
    virtual uint32_t PutResourceReplacement(uint32_t, const Entry& entry, Reader reader,
                                             std::function<void(uint64_t,size_t)>) {
        return Put(entry.parent, entry, std::move(reader), true);
    }
    virtual uint32_t PutResourceReplacementForJob(uint32_t id,const Entry& entry,Reader reader,
                                                   std::function<void(uint64_t,size_t)> keptData,Storage::Job&) {
        return PutResourceReplacement(id,entry,std::move(reader),std::move(keptData));
    }
    virtual void Rename(uint32_t id, uint32_t parent, const std::u16string &) = 0;
    virtual void RenameForJob(uint32_t id,uint32_t parent,const std::u16string& name,Storage::Job&) {
        Rename(id,parent,name);
    }
    virtual void Remove(uint32_t id) = 0;
};
std::shared_ptr<Volume> FromHfsPlus(std::unique_ptr<HfsPlus::Source>, bool writable,std::string resourceKey = {},std::wstring externalJournal = {});
std::shared_ptr<Volume> OpenImage(const std::wstring &path,const std::wstring &externalJournal = {}); // null = unrecognized
std::shared_ptr<Volume> OpenBrowserImage(const std::wstring &path,const std::wstring &externalJournal = {},
                                         const Dmg::Password* password = nullptr,const Dmg::PrivateKey* key=nullptr,
                                         const std::string& expectedImage={});
std::shared_ptr<Volume> OpenCd(int drive,std::shared_ptr<::MediaImageSource> media = {}); // null = no recognized filesystem
// Drive roots only; unrecognized/unreadable media falls back, recognized mount errors propagate.
std::shared_ptr<Volume> OpenCdRoot(const char *root,
    std::function<UINT(const char *)> driveType = {},
    std::function<std::shared_ptr<Volume>(int)> open = {});
Entry DescribeWindowsFile(const std::wstring &path);
std::pair<std::shared_ptr<Volume>,uint32_t> ResolveWindowsFile(const std::wstring &path);
bool WindowsResourceCompanion(const std::wstring &path);
void ValidateWindowsCut(Volume&,uint32_t,Storage::Job&,bool windowsDestination);
std::shared_ptr<Volume> OpenWindows(const std::wstring &directory);
std::shared_ptr<Volume> OpenClassicSource(std::shared_ptr<HfsPlus::Source>,const std::string& key,bool write=false,bool stable=false);
std::shared_ptr<Volume> OpenClassicNdif(std::shared_ptr<HfsPlus::Source>,const std::string& key,
                                       const ClassicHfsRecovery::ImageRange&,bool recovered);
struct MfsCatalogIds {
    std::map<uint32_t,uint32_t> byFile;
    uint32_t next=3;
};
std::shared_ptr<Volume> FromMfs(std::unique_ptr<MfsSource>,const std::string& key,
                                std::shared_ptr<MfsCatalogIds> ids={});
std::shared_ptr<Volume> OpenClassic(const std::string &image, const std::string &directory, bool writable,
                                    const std::string& expectedImage = {});
bool RecognizesClassicImage(HANDLE opened);
std::shared_ptr<Volume> OpenEditableNdif(std::unique_ptr<HfsPlus::Source>,const std::string& key,
                                          const DiskPartitions::Selection&);
std::shared_ptr<Volume> OpenClassicImage(const std::wstring&,bool writable,const std::string& expectedImage);
std::shared_ptr<Volume> OpenWorkerImage(const WorkerOrigin&);
// Recursive transfers retain both forks and reject self/descendant copies.
// A move removes its source only after the entire selected tree was copied.
bool Copy(Volume &source, uint32_t id, Volume &target, uint32_t parent, Storage::Job &, bool move = false);
enum class ForkSource { Stream, MountedWorker };
bool CopyMany(const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> &,
              Volume &target,uint32_t parent,Storage::Job &, const TransferAdapter & = {},
              const std::vector<ForkSource>& routes = {},bool moving=false,bool batchActive=false,std::vector<std::u16string>* publishedNames=nullptr);
void RemoveTree(Volume &, uint32_t id, Storage::Job &,bool deletionProgress=false);
void ApplyRemovalPlan(Volume&,uint32_t id,Storage::Job&,bool deletionProgress,
                      const std::function<void(uint32_t)>& removeEntry);
using SnapshotVisitor=std::function<void(Volume&,const Entry&)>;
std::string Snapshot(Volume &,uint32_t id,Storage::Job &,bool requireRemovable=false,
                     const SnapshotVisitor& visit={});
std::wstring Wide(const std::u16string &);
std::u16string U16(const std::wstring &);
}

#ifdef HFV_TESTING
namespace MacBrowser {extern std::function<void(const char*)> directoryMetadataFault;extern std::function<void(const char*)> windowsPublicationFault;}
#endif
