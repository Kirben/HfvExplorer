// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ApfsAllocation.h"
#include <variant>

namespace Apfs {
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
    bool external=false;
    uint64_t replacedEntry=0;
    uint64_t cloneSource=0,cloneSize=0;
    std::optional<PosixMetadata> posix;
    uint64_t replacedAlias=0;
};
struct CatalogRename {
    uint64_t parent,inode;
    const std::u16string& name;
    uint64_t alias=0;
};
struct CatalogDelete {
    uint64_t parent,inode;
    uint64_t alias=0;
    bool external=false,recursive=false;
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
    bool external=false;
    uint64_t alias=0;
    bool preserveDates=false;
    std::optional<PosixMetadata> posix;
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
    bool external=false;
};
struct CatalogResourceFork {
    uint64_t parent,inode;
    HfsPlus::Source& resource;
    uint64_t modified;
    bool external=false;
};
struct CatalogReplaceSymlink {
    uint64_t parent,inode;
    const HfsPlus::Bytes& target;
    std::optional<FileDates> dates;
    const FinderBytes& finder;
    bool locked;
    const HfsPlus::Attributes& attributes;
    bool external=false;
    std::optional<PosixMetadata> posix;
};
struct CatalogHardLink {
    uint64_t parent,inode;
    const std::u16string& name;
    uint64_t replaced=0;
    bool external=false;
    bool copiedLockedInode=false;
};
struct ExternalCatalogEdit {
    uint64_t parent;
    const std::u16string& name;
    TimestampEdits dates;
    std::optional<bool> locked;
    const std::u16string* newName=nullptr;
    const FinderBytes* finder=nullptr;
    uint64_t destinationParent=0;
};
struct CatalogVolumeRename { const std::u16string& name; };
struct CatalogSnapshot { const std::u16string& name; bool create; };
using CatalogMutation=std::variant<ExternalCatalogEdit,CatalogCreate,CatalogRename,CatalogDelete,CatalogReplaceData,CatalogMetadata,CatalogAttributes,CatalogResourceFork,CatalogReplaceSymlink,CatalogHardLink,CatalogSnapshot,CatalogVolumeRename>;

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
struct CatalogLayoutCounts {
    size_t catalogRecords,extentReferences;
};
CatalogLayoutCounts InspectCatalogLayout(HfsPlus::Source&,const Container&);
void ValidateWritableLayout(HfsPlus::Source&,const Container&);
uint64_t WritableFreeBytes(HfsPlus::Source&,const Container&);
uint64_t InspectAllocationFreeBytes(HfsPlus::Source&,const Container&);
uint64_t InspectFreeQueueAllocation(HfsPlus::Source&,const Container&,uint64_t selected=0);
uint64_t VerifyFreeQueueAllocation(HfsPlus::Source&,const Container&,uint64_t,HfsPlus::Bytes&,const CheckpointObjectLookup&,uint64_t selected);
CatalogWritePlan PrepareCatalogMutation(
    HfsPlus::Source&,const Container&,uint64_t transaction,HfsPlus::Bytes& superblock,
    const CheckpointObjectLookup&,const CatalogMutation&,uint64_t selected);
}
