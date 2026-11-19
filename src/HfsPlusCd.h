// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
class MediaImageSource;
namespace HfsPlus {
// Only drive roots trigger CD detection; subdirectories and image paths do not.
int CdDriveFromRoot(const char *path);
// Prefer the newest Mac data track, then the newest recognized ISO/UDF track.
// Dump CD to file uses the same native transport and TOC policy.
std::unique_ptr<Source> OpenCd(int drive, std::shared_ptr<MediaImageSource> media = {}, uint16_t* macSignature = nullptr);
} // namespace HfsPlus
