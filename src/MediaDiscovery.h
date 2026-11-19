// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "PhysicalRemovable.h"
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
