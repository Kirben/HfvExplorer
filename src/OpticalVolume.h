// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"

namespace Optical {
bool Recognizes(HfsPlus::Source&);
std::shared_ptr<MacBrowser::Volume> Open(std::unique_ptr<HfsPlus::Source>&,const std::string& key);
}
