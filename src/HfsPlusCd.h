// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
class MediaImageSource;
namespace HfsPlus {
// Read-only first/single data track. The same native transport and TOC policy
// are used by Dump CD to file. Injection is for synthetic tests, never paths.
std::unique_ptr<Source> OpenCd(int drive, std::shared_ptr<MediaImageSource> media = {});
} // namespace HfsPlus
