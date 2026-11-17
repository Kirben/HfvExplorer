// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <functional>

// Default write methods retain compatibility with read-only media/fixtures.
class MfsSource {
public:
    virtual ~MfsSource() {}
    virtual uint64_t Length() const = 0;
    virtual bool ReadAt(uint64_t offset,void* bytes,size_t length) = 0;
    virtual bool Writable()const{return false;}
    virtual bool WriteAt(uint64_t,const void*,size_t){return false;}
    virtual bool Flush(){return false;}
    virtual void DisableWrites(){}
};
class MfsImageSource : public MfsSource {
public:
    MfsImageSource();
    ~MfsImageSource();
    bool Open(const wchar_t* path,std::string& error,bool write=false);
    uint64_t Length() const override { return length_; }
    bool ReadAt(uint64_t offset,void* bytes,size_t length) override;
    bool Writable()const override{return writable_;}
    bool WriteAt(uint64_t,const void*,size_t)override;
    bool Flush()override;
    void DisableWrites()override{writable_=false;}
private:
    bool writable_=false;
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
    bool datesValid=false;
    uint32_t id=0,created=0,modified=0;
    MfsFork forks[2];
};
class MfsImage {
public:
    // A signature probe, not validation. Only raw and Disk Copy 4.2 containers.
    static bool Recognizes(MfsSource& source);
    bool Open(MfsSource& source);
    using Reader=std::function<void(unsigned,uint64_t,void*,size_t)>;
    static bool Format(MfsSource&,const std::string& name,uint64_t dataBytes,bool dc42,std::string& error);
    bool Writable()const{return source_&&source_->Writable()&&error_.empty()&&writeError_.empty()&&usable_;}
    bool RecoveryRequired()const{return recoveryRequired_;}
    const std::string& WriteError()const{return writeError_;}
    bool Put(const MfsEntry&,Reader,bool overwrite,uint32_t& id);
    bool Rename(uint32_t id,const std::string& name);
    bool Remove(uint32_t id);
    bool SetMetadata(uint32_t id,const MfsEntry&);
    bool RenameVolume(const std::string& name);
    // MacRoman names are compared in the original byte space, not HFS+ folds.
    static bool EqualNames(const std::string&,const std::string&);
    static bool EncodeName(const std::wstring&,std::string&,std::string& error,unsigned maximum=255);
    bool ReadFork(size_t entry,unsigned fork,uint64_t offset,void* bytes,size_t requested,size_t& read);
    const std::string& Error() const { return error_; }
    const std::string& VolumeName() const { return name_; }
    const std::vector<MfsEntry>& Entries() const { return entries_; }
    uint32_t AllocationSize() const { return allocationSize_; }
    uint64_t FreeBytes() const { return uint64_t(freeBlocks_)*allocationSize_; }
private:
    bool Fail(const char* error);
    bool Guard();
    bool Commit(std::vector<BYTE>& desired);
    bool Serialize(std::vector<BYTE>&,const std::vector<MfsEntry>&,const std::vector<uint16_t>&,uint32_t nextId);
    bool Snapshot(std::vector<BYTE>&);
    size_t Index(uint32_t id)const;
    void AuditWrites(const std::vector<bool>& claimed);
    bool Read(uint64_t offset,void* bytes,size_t length);
    bool Chain(MfsFork& fork,uint16_t first,std::vector<bool>& claimed);
    MfsSource* source_=nullptr;
    uint64_t base_=0,length_=0,allocationStart_=0;
    uint32_t allocationSize_=0;
    unsigned freeBlocks_=0;
    std::vector<uint16_t> map_;
    std::vector<MfsEntry> entries_;
    std::string name_,error_,writeError_;
    bool usable_=true,recoveryRequired_=false;
    uint16_t volumeFlags_=0;
    uint32_t nextId_=0,directoryStart_=0,directoryBlocks_=0;
    uint32_t tagBytes_=0;
};
std::wstring MfsDisplayName(const std::string& name);
// Export is explicit and creates a new destination; it never overwrites a path.
bool MfsExportFile(MfsImage& image,size_t entry,const wchar_t* path,bool appleSingle,std::string& error);
