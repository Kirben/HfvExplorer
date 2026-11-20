// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <optional>

namespace Storage {class Job;}
namespace Apfs {
struct Container {
    uint64_t offset, blockCount, transaction, objectMap;
    uint32_t blockSize;
    std::array<uint8_t,16> uuid;
    std::vector<uint64_t> volumes;
    uint64_t checkpointBlock=0;
    uint64_t keybagBlock=0,keybagBlocks=0;
};

// Locate an APFS container and validate its latest complete contiguous checkpoint.
std::optional<Container> Probe(HfsPlus::Source&);
uint64_t ObjectChecksum(const HfsPlus::Bytes&);

struct CheckpointPublicationUncertain : HfsPlus::Error {
    using HfsPlus::Error::Error;
};
// Republish the current state at a new transaction ID. The caller must hold
// exclusive writable access to an ordinary, raw image for the whole operation.
uint64_t CommitCheckpoint(HfsPlus::Source&,const Container& expected,Storage::Job&);
// Root folder creation supports flat, single-leaf catalogs in native images.
// The same exclusive image ownership is required as for CommitCheckpoint.
uint64_t CreateRootDirectory(HfsPlus::Source&,const Container& expected,const std::u16string& name,Storage::Job&);
}
