// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <optional>

namespace Storage {class Job;}
namespace Ndif {
// Recognize Disk Copy images by the bcem resource, including self-mounting images
// in MacBinary. The mounting program in a self-mounting image is never run.
bool Unwrap(std::unique_ptr<HfsPlus::Source>&,Storage::Job* = nullptr);
bool IsEditable(const HfsPlus::Source&);
std::optional<uint32_t> ExpectedChecksum(const HfsPlus::Source&);
void VerifySegmentChecksums(HfsPlus::Source&,Storage::Job&);
uint32_t Checksum(uint32_t running,const void* bytes,size_t count);
}
