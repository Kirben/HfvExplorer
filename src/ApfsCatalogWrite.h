// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ApfsContainer.h"

namespace Apfs {
struct CheckpointBlockWrite {
    uint64_t block;
    HfsPlus::Bytes bytes;
};
using CheckpointObjectLookup=std::function<HfsPlus::Bytes&(uint64_t oid)>;
std::vector<CheckpointBlockWrite> PrepareRootDirectory(
    HfsPlus::Source&,const Container&,uint64_t transaction,HfsPlus::Bytes& superblock,
    const CheckpointObjectLookup&,const std::u16string& name);
}
