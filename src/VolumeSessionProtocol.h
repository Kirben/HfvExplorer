// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "CopyWorkerProtocol.h"

namespace VolumeSessionProtocol {
enum class Command {
    Root, List, Stat, Read, Attributes, SetAttributes, MakeDirectory, Rename, Remove,
    HardLink, ValidateRemoval, ValidateCopy, Compression, Put, FinalizeDirectory,
    ValidateEntry, GetClassicProperties, SetClassicProperties, CloseClassic, FindChild,
    SetBasicMetadata, TestCrashRename, TestCrashPut
};
inline constexpr size_t ReadLimit=256*1024;

struct ListRequest {uint32_t parent;};
struct StatRequest {uint32_t id;};
struct ReadRequest {uint32_t id,fork;uint64_t offset,count;};
struct RenameRequest {uint32_t id,parent;std::u16string name;};
struct CatalogItem {uint32_t id;};
struct DirectoryName {uint32_t parent;std::u16string name;};
struct HardLinkRequest {uint32_t source,parent;std::u16string name;bool replace;};
struct CopyCheck {uint32_t id,parent;};
struct FindChildRequest {uint32_t parent;std::u16string name;std::optional<uint32_t> version;};
struct CompressionRequest {uint32_t id;HfsPlus::Volume::CompressionMode method;};
struct CompressionProgress {uint64_t done,total;};
struct ForkRange {uint32_t fork;uint64_t offset,count;};
struct PutHeader {uint32_t parent;bool replace,attributesKnown;uint32_t original,source;};
struct PutRequest {PutHeader header;MacBrowser::Entry entry;};
struct DirectoryUpdateHeader {uint32_t id;bool merge,attributesKnown;};
struct DirectoryUpdate {DirectoryUpdateHeader header;MacBrowser::Entry desired,expected;};
struct BasicMetadataRequest {uint32_t id;MacBrowser::Entry entry;};
struct ClassicPropertiesRequest {uint32_t id;hfsdirent properties;};

struct Reply {
    unsigned code = 0;
    std::string message;
    Storage::Rollback rollback = Storage::Rollback::None;
    DWORD error = 0;
    bool retained = false;
};

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
CopyWorkerProtocol::Data Encode(const CatalogItem&);
CopyWorkerProtocol::Data Encode(const DirectoryName&);
CopyWorkerProtocol::Data Encode(const HardLinkRequest&);
CopyWorkerProtocol::Data Encode(const CopyCheck&);
CopyWorkerProtocol::Data Encode(const FindChildRequest&);
CopyWorkerProtocol::Data Encode(const CompressionRequest&);
CopyWorkerProtocol::Data Encode(const CompressionProgress&);
CopyWorkerProtocol::Data Encode(const ForkRange&);
CopyWorkerProtocol::Data EncodePut(const PutHeader&,const MacBrowser::Entry&);
CopyWorkerProtocol::Data EncodeDirectoryUpdate(const DirectoryUpdateHeader&,const MacBrowser::Entry& desired,
                                               const MacBrowser::Entry& expected);
CopyWorkerProtocol::Data EncodeEntryCheck(const MacBrowser::Entry&);
CopyWorkerProtocol::Data EncodeBasicMetadata(uint32_t id,const MacBrowser::Entry&);
CopyWorkerProtocol::Data EncodeClassicProperties(uint32_t id,const hfsdirent&);

CatalogItem DecodeItem(CopyWorkerProtocol::Data&);
DirectoryName DecodeDirectoryName(CopyWorkerProtocol::Data&);
HardLinkRequest DecodeHardLink(CopyWorkerProtocol::Data&);
CopyCheck DecodeCopyCheck(CopyWorkerProtocol::Data&);
FindChildRequest DecodeFindChild(CopyWorkerProtocol::Data&);
CompressionRequest DecodeCompression(CopyWorkerProtocol::Data&);
CompressionProgress DecodeCompressionProgress(CopyWorkerProtocol::Data&);
ForkRange DecodeForkRange(CopyWorkerProtocol::Data&,const MacBrowser::Entry&);
PutRequest DecodePut(CopyWorkerProtocol::Data&);
DirectoryUpdate DecodeDirectoryUpdate(CopyWorkerProtocol::Data&);
MacBrowser::Entry DecodeEntryCheck(CopyWorkerProtocol::Data&);
BasicMetadataRequest DecodeBasicMetadata(CopyWorkerProtocol::Data&);
ClassicPropertiesRequest DecodeClassicProperties(CopyWorkerProtocol::Data&);

// True for Continue or the expected data frame, false for an empty Cancel frame.
bool DecodeForkReply(CopyWorkerProtocol::Message,CopyWorkerProtocol::Data&,size_t count,bool local);
bool DecodeContinue(CopyWorkerProtocol::Message,CopyWorkerProtocol::Data&);
}
