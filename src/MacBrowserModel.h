// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include "MfsImage.h"
#include "StorageWorker.h"

class MediaImageSource;
namespace MacBrowser {
struct DetectedVolumeError : HfsPlus::Error { using HfsPlus::Error::Error; };
struct Fork { uint64_t size = 0; };
// Presentation and transfer data: deliberately independent of catalog layouts,
// allocation extents, filesystem encodings and journal structures.
struct Entry {
    uint32_t id = 0, parent = 0, resolvedId = 0;
    std::u16string name;
    bool directory = false, protectedFile = false, compressed = false, hardLink = false;
    std::string readError;
    Fork forks[2];
    std::array<uint8_t,32> finder{};
    uint32_t created = 0, modified = 0;
    bool datesValid = false;
    unsigned version = 0; // optional legacy file version, not part of its identity
    std::string identity; // backend object/version identity for move revalidation
};
struct Capabilities {
    bool read = true, create = false, rename = false, remove = false;
    bool unicodeNames = true, resourceForks = true;
};
using Reader = std::function<void(unsigned, uint64_t, void *, size_t)>;
// File operations and browsing have separate queues. Native backends guard
// calls and leases; callers reserve volumes before a multi-call transaction.
// GUI code holds IDs and copied entries, never filesystem catalog records.
class Volume {
public:
    virtual ~Volume() = default;
    virtual std::string ResourceKey()const{return "adapter:"+std::to_string(reinterpret_cast<uintptr_t>(this));}
    virtual std::u16string Name() const = 0;
    virtual const char *Kind() const = 0;
    virtual std::string Description() const { return Kind(); }
    virtual bool Writable() const = 0;
    virtual Capabilities Supports() const {
        Capabilities out; out.create = out.rename = out.remove = Writable(); return out;
    }
    virtual bool CaseSensitive() const = 0;
    virtual uint64_t FreeBytes() const = 0;
    virtual void Reload() {}
    virtual void Close() {}
    virtual void ValidateCopyTo(uint32_t, Volume &, uint32_t) {}
    virtual bool CanMoveFrom(const Volume&)const{return false;}
    virtual void MoveHere(Volume&,uint32_t,uint32_t,const Entry&){throw HfsPlus::Error("This backend cannot move entries in place");}
    virtual void ValidateEntry(const Entry &e) const { HfsPlus::Normalize(e.name,CaseSensitive()); }
    virtual void ValidateRemoval(uint32_t) { if(!Writable())throw HfsPlus::Error("The source is read-only"); }
    virtual std::vector<Entry> List(uint32_t parent = 2) = 0;
    virtual Entry Stat(uint32_t id) = 0;
    virtual bool FindChild(uint32_t parent,const std::u16string& name,Entry& found){for(auto& e:List(parent))if(HfsPlus::CompareNames(e.name,name,CaseSensitive())==0){found=e;return true;}return false;}
    virtual void Read(uint32_t id, unsigned fork, uint64_t offset, void *, size_t) = 0;
    virtual uint32_t MakeDirectory(uint32_t parent, const std::u16string &) = 0;
    virtual uint32_t Put(uint32_t parent, const Entry &, Reader, bool replace) = 0;
    virtual void Rename(uint32_t id, uint32_t parent, const std::u16string &) = 0;
    virtual void Remove(uint32_t id) = 0;
};
std::shared_ptr<Volume> FromHfsPlus(std::unique_ptr<HfsPlus::Source>, bool writable,std::string resourceKey = {});
std::shared_ptr<Volume> OpenImage(const std::wstring &path); // null = unrecognized
std::shared_ptr<Volume> OpenCd(int drive,std::shared_ptr<::MediaImageSource> media = {}); // null = not HFS+/HFSX
// Drive roots only; unrecognized/unreadable media falls back, recognized mount errors propagate.
std::shared_ptr<Volume> OpenCdRoot(const char *root,
    std::function<UINT(const char *)> driveType = {},
    std::function<std::shared_ptr<Volume>(int)> open = {});
Entry DescribeWindowsFile(const std::wstring &path);
std::pair<std::shared_ptr<Volume>,uint32_t> ResolveWindowsFile(const std::wstring &path);
bool WindowsResourceCompanion(const std::wstring &path);
std::shared_ptr<Volume> OpenWindows(const std::wstring &directory);
std::shared_ptr<Volume> OpenClassic(const std::string &image, const std::string &directory, bool writable);
// Recursive transfers retain both forks and reject self/descendant copies.
// A move removes its source only after the entire selected tree was copied.
bool Copy(Volume &source, uint32_t id, Volume &target, uint32_t parent, Storage::Job &, bool move = false);
bool CopyMany(const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> &,
              Volume &target,uint32_t parent,Storage::Job &);
void RemoveTree(Volume &, uint32_t id, Storage::Job &,bool deletionProgress=false);
std::string Snapshot(Volume &,uint32_t id,Storage::Job &,bool requireRemovable=false);
std::wstring Wide(const std::u16string &);
std::u16string U16(const std::wstring &);
}
