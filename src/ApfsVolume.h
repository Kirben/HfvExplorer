// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace MacBrowser {class Volume;}
namespace Dmg {class Password;}
namespace Apfs {
// Open supported live volumes in a recognized read-only container.
std::shared_ptr<MacBrowser::Volume> Open(std::unique_ptr<HfsPlus::Source>&,const std::string& key);
// The caller owns an exclusive raw image handle. Only namespace edits are enabled.
std::shared_ptr<MacBrowser::Volume> OpenWritable(std::unique_ptr<HfsPlus::Source>&,const std::string& key);
using ReopenRawImage=std::function<std::unique_ptr<HfsPlus::Source>(bool writable)>;
// Browser opening of an ordinary raw file may upgrade to an exclusive handle.
std::shared_ptr<MacBrowser::Volume> OpenRawBrowserImage(std::unique_ptr<HfsPlus::Source>&,
    const std::string& key,const ReopenRawImage&);
struct LockedVolume: HfsPlus::Error {
    explicit LockedVolume(bool retry=false,std::wstring hint={});
    bool retry;
    std::wstring hint;
};
void Unlock(MacBrowser::Volume&,uint32_t id,const Dmg::Password&);
}
