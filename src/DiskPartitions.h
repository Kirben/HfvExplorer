// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <optional>

namespace DiskPartitions {
struct GptPartition {
    uint64_t offset, length;
    unsigned index;
    bool readOnly;
};

bool HasProtectiveMbr(const HfsPlus::Bytes& boot);
bool HasGptHeader(HfsPlus::Source&, uint32_t sector);
// Validates the entire map, returning only Apple HFS-type partitions.
std::vector<GptPartition> ReadGptMacPartitions(HfsPlus::Source&, uint32_t sector);
// A file has no reported sector geometry. A valid map may contain no Mac partitions.
std::optional<std::vector<GptPartition>> ProbeGptImage(HfsPlus::Source&, const HfsPlus::Bytes& boot);
}
