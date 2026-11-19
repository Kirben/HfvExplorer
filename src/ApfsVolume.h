// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace MacBrowser {class Volume;}
namespace Apfs {
// Open the first supported live volume in a recognized read-only container.
std::shared_ptr<MacBrowser::Volume> Open(std::unique_ptr<HfsPlus::Source>&,const std::string& key);
}
