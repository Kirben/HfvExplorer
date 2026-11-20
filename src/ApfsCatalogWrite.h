// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ApfsContainer.h"
#include <variant>

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
    std::optional<FileDates> dates;
    const FinderBytes* finder=nullptr;
};
struct CatalogRename {
    uint64_t parent,inode;
    const std::u16string& name;
};
struct CatalogDelete {
    uint64_t parent,inode;
};
struct CatalogReplaceData {
    uint64_t parent,inode;
    HfsPlus::Source& data;
    std::optional<FileDates> dates;
    const FinderBytes* finder=nullptr;
};
struct CatalogDirectoryDates {
    uint64_t parent,inode;
    FileDates dates;
};
using CatalogMutation=std::variant<CatalogCreate,CatalogRename,CatalogDelete,CatalogReplaceData,CatalogDirectoryDates>;

struct DataRun {
    uint64_t block,count;
};
struct CatalogWritePlan {
    std::vector<CheckpointBlockWrite> metadata;
    std::vector<DataRun> dataRuns;
    HfsPlus::Source* input=nullptr;
    uint64_t dataSize=0;
};
uint64_t CatalogTimestamp(uint64_t secondsSince1904);
void ValidateWritableLayout(HfsPlus::Source&,const Container&);
uint64_t WritableFreeBytes(HfsPlus::Source&,const Container&);
CatalogWritePlan PrepareCatalogMutation(
    HfsPlus::Source&,const Container&,uint64_t transaction,HfsPlus::Bytes& superblock,
    const CheckpointObjectLookup&,const CatalogMutation&);
}
