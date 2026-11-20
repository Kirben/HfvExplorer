// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicCatalog.h"
#include "CFATVolume.h"
#include "MacBrowserModel.h"
#include <array>
#include <map>
#include <optional>

#define MAX_HFS_VOLUMES 50
#define MAX_FAT_VOLUMES 50

class BrowserMountInventory {
public:
    enum class Kind { WindowsDrive, ClassicVolume, MountedVolume };
    struct Address {
        Kind kind;
        int index;
        uint64_t incarnation=0;
        bool SameSlot(const Address& other) const { return kind==other.kind&&index==other.index; }
        bool operator==(const Address& other) const { return SameSlot(other)&&incarnation==other.incarnation; }
    };
    struct Mount {
        std::shared_ptr<MacBrowser::Volume> volume;
        std::wstring key;
        int drive=-1;
        std::wstring physicalPath,suffix;
        uint64_t physicalOffset=0;
        int sourceDrive=-1;
        uint64_t incarnation=0;
    };
    struct Entry {
        Address address;
        std::wstring locator;
        std::weak_ptr<MacBrowser::Volume> volume;
        std::optional<DWORD> generation;
        int drive=-1;
        uint64_t mediaEpoch=0;
    };
    struct ClassicMount {
        CString m_file_name, m_volume_name;
        bool m_free=true,m_opened=false,m_is_floppy=false,m_is_cd=false,m_is_hd=false,m_is_removable=false;
        bool m_floppy_update_needed=false,m_mediaGenerationKnown=false;
        DWORD m_mediaGeneration=0;
        uint64_t freeBytes=0;
        int open();
        void close(){m_opened=false;}
        void ReadMount(const MacBrowser::ClassicMountInfo&);
        bool ProbeChanged(const std::string& key);
        const std::string& ResourceKey() const { return resourceKey_; }
    private:
        void update_time_stamp();
        FILETIME modified_{};
        std::string resourceKey_;
    };
    int ClassicCount() const {return classicCount;}
    int WindowsCount() const {return windowsCount;}
    const ClassicMount& ClassicAt(int index) const {return *classic.at(index);}
    const CFATVolume& WindowsAt(int index) const {return windows.at(index);}
    int FindWindowsSlot(char letter) const;
    int FindClassicSlot(const CString& path) const;
    void CloseClassic(int index);
    void CloseClassicVolumes();
    void MarkClassicDirty(int index);
    uint64_t ClassicStateVersion(int index) const { return classicState_.at(index); }
#ifdef HFV_TESTING
    bool OpenClassicForTest(int index);
    void SetClassicSourceForTest(int index,const CString& path,bool cd);
#endif
    int AddClassic(ClassicMount volume);
    void RemoveClassic(int index);
    void UpdateClassic(int index,const MacBrowser::ClassicMountInfo&);
    bool PublishClassic(const Entry& expected,ClassicMount volume);
    int AddWindows(char letter,UINT type,CString path={});
    void SetWindowsType(int index,UINT type);
    int AddMounted(Mount mount);
    void ReplaceMounted(int index,Mount mount);
    void SetMountedSourceDrive(int index,int drive);
    std::shared_ptr<MacBrowser::Volume> RemoveMounted(int index);
    void ClearMounted(){mounted.clear();}
    const std::vector<Mount>& Mounted() const { return mounted; }
    Address Bind(Address address) const;
    bool IsBound(Address address) const;
    std::optional<Entry> Lookup(Address address) const;
    std::optional<Entry> FindWindowsDrive(int drive) const;
    std::optional<Entry> FindClassicVolume(const std::wstring& locator) const;
    bool IsCurrent(const Entry& captured) const;
    void Observe(Address address,const std::string& resource,std::optional<DWORD> windowsSerial={});
    const std::vector<Entry>& Observed(const std::string& resource) const;
    void MediaChanged(DWORD driveMask);
    void ClearObserved(){observed_.clear();}

private:
    std::array<std::unique_ptr<ClassicMount>,MAX_HFS_VOLUMES> classic;
    std::array<CFATVolume,MAX_FAT_VOLUMES> windows;
    int classicCount=0,windowsCount=0;
    std::vector<Mount> mounted;
    std::array<uint64_t,MAX_HFS_VOLUMES> classicIncarnation_{},classicState_{};
    std::array<uint64_t,MAX_FAT_VOLUMES> windowsIncarnation_{};
    uint64_t nextIncarnation_=1;
    std::array<uint64_t,26> mediaEpoch_{};
    std::array<std::optional<DWORD>,MAX_FAT_VOLUMES> windowsSerial_{};
    std::map<std::string,std::vector<Entry>> observed_;
};
