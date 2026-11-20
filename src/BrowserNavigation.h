// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "BrowserDirectory.h"
#include "BrowserMountInventory.h"
#include "PhysicalRemovable.h"
#include "StorageWorker.h"
#include "DmgEncryption.h"

namespace BrowserNavigation {
enum class Intent { SelectFolder, RefreshFolder, RevealFindHit, StartupFolder, OpenImage };

struct ImageSource {
    std::wstring path, externalJournal;
    Dmg::Credentials credentials;
    std::string expectedImage;
};

struct Request {
    FileOperations::Location folder;
    Intent intent;
    std::string resource;
    FileOperations::Location reveal;
    std::vector<uint32_t> catalogRoute;
    int mediaDrive=-1;
    std::optional<ImageSource> image;
    bool tryNextStartupRoot=false;
};

struct ClassicFolder {
    BrowserMountInventory::ClassicMount mount;
    std::vector<MacBrowser::ClassicAncestor> ancestors;
};
struct FloppyFolder {BrowserMountInventory::ClassicMount mount;};
struct OpticalMount {std::shared_ptr<MacBrowser::Volume> volume;};
struct ExternalMounts {std::vector<Removable::Mounted> volumes;};
struct ExternalJournal {std::string prompt;};
struct ImageUnlock {bool retry;Dmg::UnlockMethod method;std::string image;};
struct ApfsUnlock {bool retry;};
struct MountedFolder {
    uint64_t free=0;
    std::vector<uint32_t> ancestors;
    std::wstring title;
    std::vector<std::pair<uint32_t,std::vector<MacBrowser::Entry>>> levels;
    std::vector<MacBrowser::Entry> entries;
    bool mutableFolder=true;
};

struct Result {
    BrowserDirectory::Snapshot listing;
    std::variant<std::monostate,ClassicFolder,FloppyFolder,OpticalMount,ExternalMounts,MountedFolder,ExternalJournal,ImageUnlock,ApfsUnlock> payload;
};

using Read = std::function<Storage::Result(const Request&, Result&, Storage::Job&)>;
using Publish = std::function<void(Result&)>;
} // namespace BrowserNavigation
