// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

// This interface deliberately has no write operation.
class MfsSource {
public:
    virtual ~MfsSource() {}
    virtual uint64_t Length() const = 0;
    virtual bool ReadAt(uint64_t offset,void* bytes,size_t length) = 0;
};
class MfsImageSource : public MfsSource {
public:
    MfsImageSource();
    ~MfsImageSource();
    bool Open(const wchar_t* path,std::string& error);
    uint64_t Length() const override { return length_; }
    bool ReadAt(uint64_t offset,void* bytes,size_t length) override;
private:
    MfsImageSource(const MfsImageSource&) = delete;
    MfsImageSource& operator=(const MfsImageSource&) = delete;
    HANDLE file_;
    uint64_t length_;
};
struct MfsFork {
    uint32_t length=0,physicalLength=0;
    std::vector<uint16_t> blocks;
};
struct MfsEntry {
    std::string name; // Original MacRoman bytes, never shortened to HFS Str31.
    BYTE finderInfo[16]={};
    BYTE flags=0,version=0;
    uint32_t id=0,created=0,modified=0;
    MfsFork forks[2];
};
class MfsImage {
public:
    // A signature probe, not validation. Only raw and Disk Copy 4.2 containers.
    static bool Recognizes(MfsSource& source);
    bool Open(MfsSource& source);
    bool ReadFork(size_t entry,unsigned fork,uint64_t offset,void* bytes,size_t requested,size_t& read);
    const std::string& Error() const { return error_; }
    const std::string& VolumeName() const { return name_; }
    const std::vector<MfsEntry>& Entries() const { return entries_; }
    uint32_t AllocationSize() const { return allocationSize_; }
    uint64_t FreeBytes() const { return uint64_t(freeBlocks_)*allocationSize_; }
private:
    bool Fail(const char* error);
    bool Read(uint64_t offset,void* bytes,size_t length);
    bool Chain(MfsFork& fork,uint16_t first,std::vector<bool>& claimed);
    MfsSource* source_=nullptr;
    uint64_t base_=0,length_=0,allocationStart_=0;
    uint32_t allocationSize_=0;
    unsigned freeBlocks_=0;
    std::vector<uint16_t> map_;
    std::vector<MfsEntry> entries_;
    std::string name_,error_;
};
std::wstring MfsDisplayName(const std::string& name);
// Export is explicit and creates a new destination; it never overwrites a path.
bool MfsExportFile(MfsImage& image,size_t entry,const wchar_t* path,bool appleSingle,std::string& error);
