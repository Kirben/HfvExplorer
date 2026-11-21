// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ApfsContainer.h"

namespace Apfs {
struct CheckpointBlockWrite {
    uint64_t block;
    HfsPlus::Bytes bytes;
};
using CheckpointObjectLookup=std::function<HfsPlus::Bytes&(uint64_t oid)>;

struct DataRun {
    uint64_t block,count;
};

struct FileRun {
    uint64_t owner,block,count;
    uint64_t stream=0;
};

void StageObject(std::vector<CheckpointBlockWrite>&,uint64_t block,HfsPlus::Bytes,uint64_t oid,uint64_t transaction,
                 uint32_t type,uint32_t subtype=0);
uint64_t VerifyAllocationBitmaps(HfsPlus::Source&,const Container&,const HfsPlus::Bytes&);
std::vector<uint64_t> AllocateWriteBlocks(std::vector<CheckpointBlockWrite>&,HfsPlus::Source&,const Container&,
    HfsPlus::Bytes& space,HfsPlus::Bytes& superblock,const CheckpointObjectLookup&,
    const std::vector<uint64_t>& retire,const std::vector<uint64_t>& extentNodes,uint64_t snapshots,
    const std::vector<FileRun>& files,std::vector<DataRun> retiredData,uint64_t dataBlocks,
    std::vector<DataRun>& dataRuns,uint64_t transaction,const std::function<size_t()>& requiredMetadata,
    const std::vector<DataRun>& protectedRuns={});
}
