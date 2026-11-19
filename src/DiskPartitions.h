// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <optional>

namespace DiskPartitions {
struct MacPartition {
    uint64_t offset, length;
    unsigned index;
    bool readOnly;
};

bool HasProtectiveMbr(const HfsPlus::Bytes& boot);
bool HasGptHeader(HfsPlus::Source&, uint32_t sector);
enum class GptType { AppleHfs, AppleApfs, AppleFilesystems };
// Validates the entire map, returning the requested Apple partition type.
std::vector<MacPartition> ReadGptMacPartitions(HfsPlus::Source&, uint32_t sector, GptType type=GptType::AppleHfs);
// Returns Mac entries only, after validating every entry. A Mac partition that
// overlaps the descriptor or map blocks cannot be mounted writable.
std::optional<std::vector<MacPartition>> ProbeApm(HfsPlus::Source&, const HfsPlus::Bytes& boot);
// A file has no reported sector geometry. A valid map may contain no Mac partitions.
std::optional<std::vector<MacPartition>> ProbeGptImage(HfsPlus::Source&, const HfsPlus::Bytes& boot,
                                                        GptType type=GptType::AppleHfs);
}
