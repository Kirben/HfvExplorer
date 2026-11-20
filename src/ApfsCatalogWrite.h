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
enum class CatalogKind : uint16_t { Directory=4, RegularFile=8, SymbolicLink=10 };
struct CatalogCreate {
    uint64_t parent;
    const std::u16string& name;
    CatalogKind kind;
    HfsPlus::Source* data=nullptr;
    std::optional<FileDates> dates;
    const FinderBytes* finder=nullptr;
    bool locked=false;
    const HfsPlus::Attributes* attributes=nullptr;
    HfsPlus::Source* resource=nullptr;
    const HfsPlus::Bytes* compression=nullptr;
    const HfsPlus::Bytes* linkTarget=nullptr;
};
struct CatalogRename {
    uint64_t parent,inode;
    const std::u16string& name;
    uint64_t alias=0;
};
struct CatalogDelete {
    uint64_t parent,inode;
    uint64_t alias=0;
};
struct CatalogReplaceData {
    uint64_t parent,inode;
    HfsPlus::Source& data;
    std::optional<FileDates> dates;
    const FinderBytes* finder=nullptr;
    bool locked=false;
    const HfsPlus::Attributes* attributes=nullptr;
    HfsPlus::Source* resource=nullptr;
    const HfsPlus::Bytes* compression=nullptr;
};
struct CatalogMetadata {
    uint64_t parent,inode;
    TimestampEdits dates;
    bool locked=false;
    const FinderBytes* finder=nullptr;
};
struct CatalogAttributes {
    uint64_t parent,inode;
    const HfsPlus::Attributes& attributes;
};
struct CatalogResourceFork {
    uint64_t parent,inode;
    HfsPlus::Source& resource;
    uint64_t modified;
};
struct CatalogReplaceSymlink {
    uint64_t parent,inode;
    const HfsPlus::Bytes& target;
    std::optional<FileDates> dates;
    const FinderBytes& finder;
    bool locked;
    const HfsPlus::Attributes& attributes;
};
struct CatalogHardLink {
    uint64_t parent,inode;
    const std::u16string& name;
    uint64_t replaced=0;
};
using CatalogMutation=std::variant<CatalogCreate,CatalogRename,CatalogDelete,CatalogReplaceData,CatalogMetadata,CatalogAttributes,CatalogResourceFork,CatalogReplaceSymlink,CatalogHardLink>;

struct DataRun {
    uint64_t block,count;
};
struct DataStreamWrite {
    HfsPlus::Source& input;
    uint64_t size;
    std::vector<DataRun> runs;
    uint64_t id=0;
};
struct CatalogWritePlan {
    std::vector<CheckpointBlockWrite> metadata;
    std::vector<std::unique_ptr<HfsPlus::Source>> attributeInputs;
    std::vector<DataStreamWrite> streams;
};
void AssignDataRuns(std::vector<DataStreamWrite>&,const std::vector<DataRun>&);
void ValidateWritableAttributes(const HfsPlus::Attributes&);
uint64_t CatalogTimestamp(uint64_t secondsSince1904);
void ValidateWritableLayout(HfsPlus::Source&,const Container&);
uint64_t WritableFreeBytes(HfsPlus::Source&,const Container&);
CatalogWritePlan PrepareCatalogMutation(
    HfsPlus::Source&,const Container&,uint64_t transaction,HfsPlus::Bytes& superblock,
    const CheckpointObjectLookup&,const CatalogMutation&);
}
