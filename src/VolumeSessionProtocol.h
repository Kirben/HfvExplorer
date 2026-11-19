// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "CopyWorkerProtocol.h"

namespace VolumeSessionProtocol {
enum class Command {
    Root, List, Stat, Read, Attributes, SetAttributes, MakeDirectory, Rename, Remove,
    HardLink, ValidateRemoval, ValidateCopy, Compression, Put, FinalizeDirectory,
    ValidateEntry, GetClassicProperties, SetClassicProperties, CloseClassic, FindMfs,
    SetBasicMetadata, TestCrashRename, TestCrashPut
};
inline constexpr size_t ReadLimit=256*1024;

struct ListRequest {uint32_t parent;};
struct StatRequest {uint32_t id;};
struct ReadRequest {uint32_t id,fork;uint64_t offset,count;};
struct RenameRequest {uint32_t id,parent;std::u16string name;};
struct Reply {unsigned code;std::string error;Storage::Rollback rollback;};

CopyWorkerProtocol::Data EncodeCommand(Command,const CopyWorkerProtocol::Data& payload);
Command DecodeCommand(CopyWorkerProtocol::Data&,bool primaryAttributes,bool secondaryAttributes);
CopyWorkerProtocol::Data Encode(const ListRequest&);
CopyWorkerProtocol::Data Encode(const StatRequest&);
CopyWorkerProtocol::Data Encode(const ReadRequest&);
CopyWorkerProtocol::Data Encode(const RenameRequest&);
CopyWorkerProtocol::Data Encode(const Reply&);
ListRequest DecodeList(CopyWorkerProtocol::Data&);
StatRequest DecodeStat(CopyWorkerProtocol::Data&);
ReadRequest DecodeRead(CopyWorkerProtocol::Data&);
RenameRequest DecodeRename(CopyWorkerProtocol::Data&);
Reply DecodeReply(CopyWorkerProtocol::Data&);
uint32_t CatalogNumber(CopyWorkerProtocol::Data&);
}
