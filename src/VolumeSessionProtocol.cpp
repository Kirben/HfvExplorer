// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "VolumeSessionProtocol.h"

namespace VolumeSessionProtocol {
using CopyWorkerProtocol::Check;
using CopyWorkerProtocol::Data;

Data EncodeCommand(Command command,const Data& payload)
{
    Data request;
    request.put(uint64_t(command));
    request.bytes.insert(request.bytes.end(),payload.bytes.begin(),payload.bytes.end());
    return request;
}

Command DecodeCommand(Data& packet,bool primaryAttributes,bool secondaryAttributes)
{
    const auto opcode=packet.get();
#ifdef HFV_TESTING
    constexpr auto maximum=Command::TestCrashPut;
#else
    constexpr auto maximum=Command::SetBasicMetadata;
#endif
    Check(opcode<=uint64_t(maximum),"Invalid volume session command");
    const auto command=Command(opcode);
    const bool carriesAttributes=command==Command::SetAttributes||command==Command::Put||
        command==Command::TestCrashPut||command==Command::FinalizeDirectory||command==Command::ValidateEntry;
    Check(primaryAttributes==carriesAttributes&&!secondaryAttributes,"Unexpected volume session attributes");
    return command;
}

static uint32_t CatalogNumber(Data& packet)
{
    const auto value=packet.get();
    Check(value<=UINT32_MAX,"Invalid volume session catalog number");
    return uint32_t(value);
}

Data Encode(const ListRequest& value)
{
    Data payload;
    payload.put(value.parent);
    return payload;
}

ListRequest DecodeList(Data& packet)
{
    const auto parent=CatalogNumber(packet);
    packet.end();
    return {parent};
}

Data Encode(const StatRequest& value)
{
    Data payload;
    payload.put(value.id);
    return payload;
}

StatRequest DecodeStat(Data& packet)
{
    const auto id=CatalogNumber(packet);
    packet.end();
    return {id};
}

Data Encode(const ReadRequest& value)
{
    Data payload;
    payload.put(value.id);
    payload.put(value.fork);
    payload.put(value.offset);
    payload.put(value.count);
    return payload;
}

ReadRequest DecodeRead(Data& packet)
{
    const auto id=CatalogNumber(packet);
    const auto fork=CatalogNumber(packet);
    const auto offset=packet.get();
    const auto count=packet.get();
    packet.end();
    Check(fork<=1&&count<=ReadLimit,"Invalid volume read range");
    return {id,fork,offset,count};
}

Data Encode(const RenameRequest& value)
{
    Data payload;
    payload.put(value.id);
    payload.put(value.parent);
    payload.u16(value.name);
    return payload;
}

RenameRequest DecodeRename(Data& packet)
{
    const auto id=CatalogNumber(packet);
    const auto parent=CatalogNumber(packet);
    auto name=packet.u16();
    packet.end();
    return {id,parent,std::move(name)};
}

Data Encode(const Reply& value)
{
    Data payload;
    payload.put(value.code);
    payload.put(value.message);
    payload.put(uint64_t(value.rollback));
    payload.put(value.error);
    payload.put(value.retained);
    return payload;
}

Reply DecodeReply(Data& packet)
{
    const auto code=packet.get();
    auto message=packet.text();
    const auto rollback=packet.get();
    const auto error=packet.get();
    const auto retained=packet.flag();
    packet.end();
    Check(code<=2&&rollback<=uint64_t(Storage::Rollback::Failed)&&error<=MAXDWORD&&
          (code==1 ? error!=0 : error==0&&!retained),"Invalid volume session result");
    return {unsigned(code),std::move(message),Storage::Rollback(rollback),DWORD(error),retained};
}

Data Encode(const CatalogItem& request)
{
    Data payload;
    payload.put(request.id);
    return payload;
}

CatalogItem DecodeItem(Data& packet)
{
    const auto id=CatalogNumber(packet);
    packet.end();
    return {id};
}

Data Encode(const DirectoryName& request)
{
    Data payload;
    payload.put(request.parent);
    payload.u16(request.name);
    return payload;
}

DirectoryName DecodeDirectoryName(Data& packet)
{
    const auto parent=CatalogNumber(packet);
    auto name=packet.u16();
    packet.end();
    return {parent,std::move(name)};
}

Data Encode(const HardLinkRequest& request)
{
    Data payload;
    payload.put(request.source);
    payload.put(request.parent);
    payload.u16(request.name);
    payload.put(request.replace);
    return payload;
}

HardLinkRequest DecodeHardLink(Data& packet)
{
    HardLinkRequest request{CatalogNumber(packet),CatalogNumber(packet),packet.u16(),packet.flag()};
    packet.end();
    return request;
}

Data Encode(const CopyCheck& request)
{
    Data payload;
    payload.put(request.id);
    payload.put(request.parent);
    return payload;
}

CopyCheck DecodeCopyCheck(Data& packet)
{
    const CopyCheck request{CatalogNumber(packet),CatalogNumber(packet)};
    packet.end();
    return request;
}

Data Encode(const FindChildRequest& request)
{
    Data payload;
    payload.put(request.parent);
    payload.u16(request.name);
    payload.put(request.version.has_value());
    if(request.version)payload.put(*request.version);
    return payload;
}

FindChildRequest DecodeFindChild(Data& packet)
{
    FindChildRequest request{CatalogNumber(packet),packet.u16(),{}};
    if(packet.flag())request.version=CatalogNumber(packet);
    packet.end();
    return request;
}

Data Encode(const CompressionRequest& request)
{
    Data payload;
    payload.put(request.id);
    payload.put(uint64_t(request.method));
    return payload;
}

CompressionRequest DecodeCompression(Data& packet)
{
    using Mode=HfsPlus::Volume::CompressionMode;
    const auto id=CatalogNumber(packet);
    const auto method=packet.get();
    packet.end();
    Check(method<=uint64_t(Mode::StoredInline),"Invalid volume compression method");
    return {id,Mode(method)};
}

Data Encode(const CompressionProgress& progress)
{
    Data payload;
    payload.put(progress.done);
    payload.put(progress.total);
    return payload;
}

CompressionProgress DecodeCompressionProgress(Data& packet)
{
    const CompressionProgress progress{packet.get(),packet.get()};
    packet.end();
    return progress;
}

Data Encode(const ForkRange& range)
{
    Data payload;
    payload.put(range.fork);
    payload.put(range.offset);
    payload.put(range.count);
    return payload;
}

ForkRange DecodeForkRange(Data& packet,const MacBrowser::Entry& entry)
{
    const ForkRange range{CatalogNumber(packet),packet.get(),packet.get()};
    packet.end();
    Check(range.fork<=1&&range.count&&range.count<=ReadLimit,"Invalid fork transfer range");
    const auto length=entry.forks[range.fork].size;
    Check(range.offset<=length&&range.count<=length-range.offset,"Fork transfer exceeds source length");
    return range;
}

Data EncodePut(const PutHeader& header,const MacBrowser::Entry& entry)
{
    Data payload;
    payload.put(header.parent);
    payload.put(header.replace);
    payload.put(header.attributesKnown);
    payload.put(header.original);
    payload.put(header.source);
    CopyWorkerProtocol::EncodeEntry(payload,entry);
    return payload;
}

PutRequest DecodePut(Data& packet)
{
    const PutHeader header{CatalogNumber(packet),packet.flag(),packet.flag(),CatalogNumber(packet),CatalogNumber(packet)};
    Check(!header.original||!header.source,"Conflicting in-worker fork sources");
    auto entry=CopyWorkerProtocol::DecodeEntry(packet);
    packet.end();
    Check(!header.original||(header.replace&&entry.id==header.original),"Invalid resource fork replacement");
    Check(!header.source||entry.id==header.source,"Invalid in-worker fork source");
    entry.attributesKnown=header.attributesKnown;
    return {header,std::move(entry)};
}

Data EncodeDirectoryUpdate(const DirectoryUpdateHeader& header,const MacBrowser::Entry& desired,
                           const MacBrowser::Entry& expected)
{
    Data payload;
    payload.put(header.id);
    payload.put(header.merge);
    payload.put(header.attributesKnown);
    CopyWorkerProtocol::EncodeEntry(payload,desired);
    CopyWorkerProtocol::EncodeEntry(payload,expected);
    return payload;
}

DirectoryUpdate DecodeDirectoryUpdate(Data& packet)
{
    const DirectoryUpdateHeader header{CatalogNumber(packet),packet.flag(),packet.flag()};
    auto desired=CopyWorkerProtocol::DecodeEntry(packet);
    auto expected=CopyWorkerProtocol::DecodeEntry(packet);
    packet.end();
    desired.attributesKnown=header.attributesKnown;
    return {header,std::move(desired),std::move(expected)};
}

Data EncodeEntryCheck(const MacBrowser::Entry& entry)
{
    Data payload;
    CopyWorkerProtocol::EncodeEntry(payload,entry);
    return payload;
}

MacBrowser::Entry DecodeEntryCheck(Data& packet)
{
    auto entry=CopyWorkerProtocol::DecodeEntry(packet);
    packet.end();
    return entry;
}

Data EncodeBasicMetadata(uint32_t id,const MacBrowser::Entry& entry)
{
    Data payload;
    payload.put(id);
    CopyWorkerProtocol::EncodeEntry(payload,entry);
    return payload;
}

BasicMetadataRequest DecodeBasicMetadata(Data& packet)
{
    const auto id=CatalogNumber(packet);
    auto entry=CopyWorkerProtocol::DecodeEntry(packet);
    packet.end();
    return {id,std::move(entry)};
}

Data EncodeClassicProperties(uint32_t id,const hfsdirent& entry)
{
    Data payload;
    payload.put(id);
    CopyWorkerProtocol::EncodeClassic(payload,entry);
    return payload;
}

ClassicPropertiesRequest DecodeClassicProperties(Data& packet)
{
    const auto id=CatalogNumber(packet);
    const auto properties=CopyWorkerProtocol::DecodeClassic(packet);
    packet.end();
    return {id,properties};
}

bool DecodeContinue(CopyWorkerProtocol::Message reply,Data& packet)
{
    packet.end();
    Check(reply==CopyWorkerProtocol::Cancel||reply==CopyWorkerProtocol::SessionContinue,
          "Invalid volume streaming decision");
    return reply==CopyWorkerProtocol::SessionContinue;
}

bool DecodeForkReply(CopyWorkerProtocol::Message reply,Data& packet,size_t count,bool local)
{
    if(local||reply==CopyWorkerProtocol::Cancel)return DecodeContinue(reply,packet);
    Check(reply==CopyWorkerProtocol::SessionBytes&&packet.bytes.size()==count,"Invalid fork transfer data");
    return true;
}

}
