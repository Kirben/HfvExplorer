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
// Folder and file creation and rename support up to two catalog tree levels in native images.
// The same exclusive image ownership is required as for CommitCheckpoint.
uint64_t CreateCatalogDirectory(HfsPlus::Source&,const Container& expected,uint64_t parent,const std::u16string& name,Storage::Job&);
uint64_t CreateEmptyFile(HfsPlus::Source&,const Container& expected,uint64_t parent,const std::u16string& name,Storage::Job&);
// Stream a data fork from a separate source into a newly created file.
uint64_t CreateFile(HfsPlus::Source&,const Container& expected,uint64_t parent,const std::u16string& name,
                    HfsPlus::Source& data,Storage::Job&);
// Rename within the same directory, using on-disk inode IDs and the expected checkpoint.
uint64_t RenameEntry(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                     const std::u16string& name,Storage::Job&);
// Delete a regular file or an empty folder. File blocks become reusable after publication.
uint64_t DeleteEntry(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,Storage::Job&);
// Replace a regular file's data fork in one checkpoint, preserving its inode.
uint64_t ReplaceFileData(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                         HfsPlus::Source& data,Storage::Job&);
}
