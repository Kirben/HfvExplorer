// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include <vector>
struct PhysicalVolume {int drive;bool floppy;};
std::function<Storage::Result(Storage::Job&)> MakeMediaScanJob(bool,bool,std::shared_ptr<std::vector<PhysicalVolume>>,std::function<bool(int,bool)>);
std::vector<PhysicalVolume> ScanPhysicalMedia(bool,bool);