// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace MacBrowser {class Volume;}
namespace Dmg {class Password;}
namespace Apfs {
// Open supported live volumes in a recognized read-only container.
std::shared_ptr<MacBrowser::Volume> Open(std::unique_ptr<HfsPlus::Source>&,const std::string& key);
struct LockedVolume: HfsPlus::Error {
    explicit LockedVolume(bool retry=false);
    bool retry;
};
void Unlock(MacBrowser::Volume&,uint32_t id,const Dmg::Password&);
}
