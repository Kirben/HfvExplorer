// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "PhysicalRemovable.h"
#include "ClassicCatalog.h"
#include <map>
#include "StorageWorker.h"

namespace MediaDiscovery {
using OpticalProbe=std::function<std::shared_ptr<MacBrowser::Volume>()>;
std::shared_ptr<MacBrowser::Volume> Optical(int drive,Storage::Job& job,OpticalProbe probe={});
bool Floppy(int drive,Storage::Job& job);
std::vector<Removable::Mounted> External(const Removable::Device&,const std::shared_ptr<Removable::Api>&,Storage::Job&);
// Callers supply the drive class and the kinds of Mac media to inspect.
struct DriveMedia {
    std::shared_ptr<MacBrowser::Volume> cd,floppy;
    std::vector<Removable::Mounted> removable;
};
DriveMedia ProbeDrive(int drive,UINT type,bool checkFloppy,bool checkExternal,Storage::Job&);
uint64_t Generation();
void Changed(DWORD driveMask);
}

class BrowserMountInventory;
namespace VolumeCoord { class Lease; }

namespace MediaDiscovery {
struct DriveUpdate {
    int drive;
    bool rescan;
    UINT type=DRIVE_NO_ROOT_DIR;
    std::shared_ptr<MacBrowser::Volume> cd;
    std::vector<Removable::Mounted> removable;
    std::optional<MacBrowser::ClassicMountInfo> floppy;

    bool Present() const {return type>=DRIVE_REMOVABLE&&type<=DRIVE_RAMDISK;}
    bool ReplaceWindows(UINT previous) const {return !Present()||type!=previous;}
    bool RefreshFloppy(bool opened,std::optional<DWORD> generation) const {
        return !rescan||!floppy||!opened||!generation||generation!=floppy->generation;
    }
    bool RemoveOptical(int existingDrive,const std::shared_ptr<MacBrowser::Volume>& existing) const {
        return existing&&existingDrive==drive&&existing!=cd;
    }
    bool UpdateExternal() const {return !rescan||!Present();}
};

class UpdateController {
public:
    enum class Status { Idle, Working, Retry };
    using Wake=std::function<void(const std::shared_ptr<Storage::Job>&)>;
    using ApplyDrive=std::function<void(DriveUpdate&,std::shared_ptr<VolumeCoord::Lease>)>;
    using ApplyScan=std::function<void(Removable::Discovery&,bool,const Storage::Result&)>;

    UpdateController(const BrowserMountInventory& mounts,Wake wake):mounts_(mounts),wake_(std::move(wake)){}
    ~UpdateController(){Cancel();}
    void ScanExternal(bool silent,bool startupOnly=false);
    void Queue(DWORD changed,DWORD arrived,bool rescan);
    Status Apply(bool startupFloppies,const ApplyDrive&,const ApplyScan&);
    bool Pending() const {return externalScan_.has_value()||!driveChanges_.empty();}
    void Cancel();
#ifdef HFV_TESTING
    std::function<UINT(int)> testDriveType;
#endif

private:
    struct DriveChange {
        bool arrived,rescan;
        std::shared_ptr<DriveUpdate> media;
        std::shared_ptr<Storage::Job> job;
    };
    struct ExternalScan {
        std::shared_ptr<Removable::Discovery> media;
        std::shared_ptr<Storage::Job> job;
        uint64_t generation;
        bool silent;
    };
    const BrowserMountInventory& mounts_;
    Wake wake_;
    bool removableScanned_=false;
    std::map<int,DriveChange> driveChanges_;
    std::optional<ExternalScan> externalScan_;
};

enum class ExternalChange { Keep, Reuse, Remove };
ExternalChange ClassifyExternal(const MacBrowser::Volume& current,const std::wstring& path,
    int sourceDrive,const MacBrowser::Volume* found,int scannedDrive,const std::vector<std::wstring>& failed);
}
