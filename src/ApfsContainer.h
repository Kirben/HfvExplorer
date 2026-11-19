// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <optional>

namespace Apfs {
struct Container {
    uint64_t offset, blockCount, transaction, objectMap;
    uint32_t blockSize;
    std::array<uint8_t,16> uuid;
    std::vector<uint64_t> volumes;
    uint64_t checkpointBlock=0;
};

// Locate an APFS container and validate its latest complete contiguous checkpoint.
std::optional<Container> Probe(HfsPlus::Source&);
uint64_t ObjectChecksum(const HfsPlus::Bytes&);
}
