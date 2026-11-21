// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "BrowserDirectory.h"
#include <map>

namespace MacIcons {
using BundleIcons=std::map<uint32_t,std::shared_ptr<BrowserDirectory::OwnedIcons>>;
size_t ResourceForkLength(const HfsPlus::Bytes& header);
BundleIcons DecodeBundle(const HfsPlus::Bytes& bytes);
BundleIcons ReadClassicBundle(MacBrowser::Volume& volume,uint32_t file,uint64_t forkSize);
std::shared_ptr<BrowserDirectory::OwnedIcons> Decode(const HfsPlus::Bytes& bytes);
std::shared_ptr<BrowserDirectory::OwnedIcons> ReadClassic(MacBrowser::Volume& volume,
                                                       uint32_t file, uint64_t forkSize);
void Load(MacBrowser::Volume& volume, BrowserDirectory::Row& row);
}
