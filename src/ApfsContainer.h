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
};

// Locate and validate the block-zero superblock of a raw or GPT APFS image.
std::optional<Container> Probe(HfsPlus::Source&);
uint64_t ObjectChecksum(const HfsPlus::Bytes&);
}
