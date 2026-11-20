// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ApfsContainer.h"

namespace Apfs {
struct CheckpointBlockWrite {
    uint64_t block;
    HfsPlus::Bytes bytes;
};
using CheckpointObjectLookup=std::function<HfsPlus::Bytes&(uint64_t oid)>;
enum class CatalogKind : uint16_t { Directory=4, RegularFile=8 };
struct CatalogCreate {
    uint64_t parent;
    const std::u16string& name;
    CatalogKind kind;
    HfsPlus::Source* data=nullptr;
};
struct CatalogWritePlan {
    std::vector<CheckpointBlockWrite> metadata;
    uint64_t dataBlock=0,dataSize=0;
};
CatalogWritePlan PrepareCatalogCreate(
    HfsPlus::Source&,const Container&,uint64_t transaction,HfsPlus::Bytes& superblock,
    const CheckpointObjectLookup&,const CatalogCreate&);
}
