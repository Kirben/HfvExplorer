// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <optional>

namespace CdMedia {
std::optional<std::vector<std::unique_ptr<HfsPlus::Source>>> OpenTrackImage(
    const std::wstring&,std::unique_ptr<HfsPlus::Source>&,const std::string& key);
}
