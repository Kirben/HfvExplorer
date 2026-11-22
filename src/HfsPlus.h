// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>
#include "HfsPlusAttributeData.h"
#include "PosixMetadata.h"
namespace HfsPlus {
using Bytes = std::vector<uint8_t>;
struct SparseRange {
    uint64_t offset,length;
    bool operator==(const SparseRange& other) const {
        return offset==other.offset&&length==other.length;
    }
};
struct ExternalJournalRequired : std::runtime_error {using std::runtime_error::runtime_error;};
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Every offset is relative to the image, never a native physical-device path.
class Source {
  public:
    virtual ~Source() = default;
    virtual uint64_t Size() const = 0;
    virtual std::vector<SparseRange> KnownZeroRanges() const { return {}; }
    virtual bool Writable() const = 0;
    virtual bool ProcessLocal()const{return false;}
    virtual void Read(uint64_t, void *, size_t) = 0;
    virtual void Write(uint64_t, const void *, size_t) = 0;
    virtual void Flush() = 0;
    // Removable read-only sources validate media even for cached catalog reads.
    virtual void CheckMedia() {}
    virtual std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const{return {};}
    virtual int MediaDrive()const{return -1;}
    virtual bool MediaGeneration(uint32_t&)const{return false;}
    virtual std::shared_ptr<Source> OpenJournal(const Bytes&){throw ExternalJournalRequired("Select the matching external journal disk image");}
};
#ifdef HFV_TESTING
extern std::function<void(bool,uint64_t,size_t)> workerIoFault;
#endif
std::unique_ptr<Source> OpenImage(const wchar_t *, bool write, const wchar_t *externalJournal = nullptr);
std::unique_ptr<Source> CreateImage(const wchar_t *, uint64_t size, const wchar_t *externalJournal = nullptr);
class NewImage : public Source {
public:
    virtual void Resize(uint64_t) = 0;
    virtual void PublishTo(const std::wstring&) = 0;
    virtual void Keep() noexcept = 0;
    virtual bool Discard() noexcept = 0;
};
// Owns a newly created file and deletes it by handle unless kept.
std::unique_ptr<NewImage> CreateFormatOutput(const wchar_t*,uint64_t size,const wchar_t* externalJournal=nullptr);
std::unique_ptr<NewImage> CreateStagedOutput(const std::wstring& destination,uint64_t size,bool allowExisting=false);
std::string InitializeExternalJournalImage(Source&);
std::string CreateExternalJournalImage(const wchar_t*,uint64_t size = 64ULL*1024*1024);
struct Extent {
    uint32_t start = 0, count = 0;
};
struct Fork {
    uint64_t size = 0;
    uint32_t blocks = 0;
    std::vector<Extent> extents;
};
using Attributes = std::map<std::u16string, AttributeData>;
// On-disk Mac ACLs retain owner/group UUIDs, ACE order, rights and inheritance flags.
// These are filesystem metadata, not Windows access-control enforcement.
void ValidateAttributes(const Attributes &);
struct AdditionalFileDates {
    std::optional<uint64_t> accessed, changed; // Seconds since 1904.
};
struct Entry {
    uint32_t id = 0, parent = 0;
    std::u16string name;
    bool directory = false, protectedFile = false;
    // Stat/List expose logical fork sizes and resolved content, while retaining
    // the visible link's catalog ID, name and parent. Mutations use raw records.
    bool compressed = false, hardLink = false, symbolicLink = false;
    uint32_t resolvedId = 0, linkCount = 1;
    std::string readError; // List can report a broken item without hiding its siblings.
    Fork forks[2];
    std::array<uint8_t, 32> finder{};
    uint64_t created = 0, modified = 0; // canonical seconds since 1904, including expanded dates
    AdditionalFileDates additionalDates;
    std::optional<PosixMetadata> posix;
};
// Unicode 3.2 canonical decomposition, with the exclusions required by TN1150.
std::u16string Normalize(const std::u16string &, bool caseSensitive = false);
int CompareNames(const std::u16string &, const std::u16string &, bool caseSensitive = false);
class Volume {
  public:
    enum class FormatKind { HfsPlus, HfsXCaseSensitive, HfsXCaseInsensitive };
    static bool Recognizes(Source &);
    static void Format(Source &, const std::u16string &name, bool journaled = true,
                       FormatKind kind = FormatKind::HfsPlus, bool expandedTimes = false);
    explicit Volume(Source &, bool write = false);
    ~Volume();
    std::vector<Entry> List(uint32_t parent = 2) const;
    bool FindChild(uint32_t parent, const std::u16string& name, Entry& found) const;
    Entry Stat(uint32_t id) const;
    static constexpr size_t MaxSymbolicLinkPath=1024;
    static void ValidateSymbolicLinkTarget(const Bytes& target);
    std::optional<Bytes> SymbolicLinkTarget(uint32_t id);
    void PutSymbolicLink(uint32_t parent,const std::u16string& name,const Bytes& target,
                         const std::array<uint8_t,32>& finder,bool replace,const Attributes& attributes,
                         const std::array<uint64_t,2>* dates,std::optional<PosixMetadata> posix={},AdditionalFileDates additionalDates={});
    uint64_t Read(uint32_t id, unsigned fork, uint64_t offset, void *, size_t);
    uint64_t CompressionResourceSize(uint32_t id);
    void ReadCompressionResource(uint32_t id, uint64_t offset, void *, size_t);
    uint32_t MakeDirectory(uint32_t parent, const std::u16string &name, const Attributes *attributes = nullptr);
    enum class CompressionMode { Preserve, Uncompressed, Zlib, Lzvn, Lzfse, Lzbitmap, Stored, StoredInline, CopyCompressed };
    void SetCompression(uint32_t id,CompressionMode,std::function<void(uint64_t,uint64_t)> progress = {});
    void SetCompression(uint32_t id,bool compressed,std::function<void(uint64_t,uint64_t)> progress = {});
    // A file edit follows its shared inode. A transfer can replace a
    // selected hard-link or symbolic-link entry instead.
    enum class OverwriteScope { SharedInode, SelectedName, SymbolicLink };
    // Streaming, copy-on-write replacement. Source callback must return exact bytes.
    uint32_t Put(uint32_t parent, const std::u16string &name, uint64_t dataSize, uint64_t resourceSize,
                 std::function<void(unsigned, uint64_t, void *, size_t)> read,
                 const std::array<uint8_t, 32> *finder = nullptr, bool overwrite = false,
                 const std::array<uint32_t,2> *dates = nullptr, const Attributes *attributes = nullptr, CompressionMode compression = CompressionMode::Preserve, const std::array<uint64_t,2> *wideDates = nullptr,
                 std::optional<PosixMetadata> posix = {},OverwriteScope scope=OverwriteScope::SharedInode,
                 AdditionalFileDates additionalDates={}, const Bytes* storedCompression=nullptr,bool storedResource=false);
    void SetDirectoryMetadata(uint32_t id,const std::array<uint8_t,32>* finder,
                              const std::array<uint64_t,2>* dates,const Attributes* attributes,
                              std::optional<PosixMetadata> posix = {},AdditionalFileDates additionalDates={});
    Attributes GetAttributes(uint32_t id) const;
    void SetAttributes(uint32_t id, const Attributes &);
    uint32_t MakeHardLink(uint32_t source, uint32_t parent, const std::u16string &name,bool replace=false,bool copiedImmutable=false);
    void Rename(uint32_t id, uint32_t parent, const std::u16string &name);
    void Remove(uint32_t id);
    void ValidateRemoval(uint32_t id) const;
    void ValidateCopyTo(uint32_t id,uint32_t parent)const;
    void UseExternalJournal(const std::string &uuid);
    bool ExternalJournal() const;
    bool ExpandedTimes() const;
    bool Journaled() const;
    bool IsHfsX() const;
    bool CaseSensitive() const;
    bool Writable() const;
    std::u16string Name() const;
    uint64_t FreeBytes() const;
    // Test fault boundary: callback runs after each durable commit phase.
    std::function<void(const char *)> fault;

  private:
    uint32_t PutFile(uint32_t parent,const std::u16string& name,uint64_t dataSize,uint64_t resourceSize,
                     const std::function<void(unsigned,uint64_t,void*,size_t)>& read,
                     const std::array<uint8_t,32>* finder,bool overwrite,
                     const std::array<uint32_t,2>* dates,const Attributes* attributes,CompressionMode compression,
                     const std::array<uint64_t,2>* wideDates,std::optional<PosixMetadata> posix,bool symbolicLink,
                     OverwriteScope scope,AdditionalFileDates additionalDates,const Bytes* storedCompression,bool storedResource);
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace HfsPlus
