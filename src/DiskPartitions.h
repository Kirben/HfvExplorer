// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <optional>

namespace DiskPartitions {
struct MacPartition {
    uint64_t offset, length;
    unsigned index;
    bool readOnly;

    bool operator==(const MacPartition& other) const {
        return offset==other.offset && length==other.length &&
               index==other.index && readOnly==other.readOnly;
    }
};
enum class Filesystem { HfsPlus, HfsX, Classic, Mfs, Apfs };
struct Selection {
    Filesystem filesystem;
    MacPartition partition;
    bool operator!=(const Selection& other) const { return !(*this==other); }
    bool operator==(const Selection& other) const {
        return filesystem==other.filesystem && partition==other.partition;
    }
};

std::optional<Selection> FindHfsPlus(HfsPlus::Source&);
std::optional<Filesystem> LegacyFilesystem(HfsPlus::Source&, const MacPartition&, bool diskCopy = false);
std::optional<Selection> FindLegacy(HfsPlus::Source&, bool diskCopy = false);

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
