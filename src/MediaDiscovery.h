// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "PhysicalRemovable.h"
#include "StorageWorker.h"

namespace MediaDiscovery {
using OpticalProbe=std::function<std::shared_ptr<MacBrowser::Volume>()>;
std::shared_ptr<MacBrowser::Volume> Optical(int drive,Storage::Job& job,OpticalProbe probe={});
bool Floppy(int drive,Storage::Job& job);
std::vector<Removable::Mounted> External(const Removable::Device&,const std::shared_ptr<Removable::Api>&,Storage::Job&);
void Changed(DWORD driveMask);
}
