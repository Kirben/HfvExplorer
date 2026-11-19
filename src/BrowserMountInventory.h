// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Mactypes.h"
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
        bool operator==(const Address& other) const { return kind == other.kind && index == other.index; }
    };
    struct Mount {
        std::shared_ptr<MacBrowser::Volume> volume;
        std::wstring key;
        HTREEITEM root=nullptr;
        int drive=-1;
        std::wstring physicalPath,suffix;
        uint64_t physicalOffset=0;
    };
    struct Entry {
        Address address;
        HTREEITEM root=nullptr;
        std::wstring locator;
        std::weak_ptr<MacBrowser::Volume> volume;
        std::optional<DWORD> generation;
        int drive=-1;
        uint64_t mediaEpoch=0;
    };
    class ClassicSlots {
        std::unique_ptr<CHFVVolume> volumes_[MAX_HFS_VOLUMES];
    public:
        CHFVVolume& operator[](int index) {
            auto& volume=volumes_[index];
            if(!volume)volume=std::make_unique<CHFVVolume>();
            return *volume;
        }
        const CHFVVolume* Find(int index) const { return volumes_[index].get(); }
        void ReplaceFrom(int index,ClassicSlots& source,int slot) {
            volumes_[index].swap(source.volumes_[slot]);
        }
    } classic;
    CFATVolume windows[MAX_FAT_VOLUMES];
    int classicCount=0,windowsCount=0;
    uint64_t classicRevision[MAX_HFS_VOLUMES]{};
    std::vector<Mount> mounted;

    static std::optional<Address> FromTreeBinding(int type,int index);
    std::optional<Entry> Lookup(Address address) const;
    std::optional<Entry> FindWindowsDrive(int drive) const;
    std::optional<Entry> FindClassicVolume(const std::wstring& locator) const;
    bool IsCurrent(const Entry& captured) const;
    void Observe(Address address,const std::string& resource,std::optional<DWORD> windowsSerial={});
    const std::vector<Entry>& Observed(const std::string& resource) const;
    void MediaChanged(DWORD driveMask);
    void ClearObserved(){observed_.clear();}

private:
    std::array<uint64_t,26> mediaEpoch_{};
    std::array<std::optional<DWORD>,MAX_FAT_VOLUMES> windowsSerial_{};
    std::map<std::string,std::vector<Entry>> observed_;
};
