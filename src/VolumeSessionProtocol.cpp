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

uint32_t CatalogNumber(Data& packet)
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
    payload.put(value.error);
    payload.put(uint64_t(value.rollback));
    return payload;
}

Reply DecodeReply(Data& packet)
{
    const auto code=packet.get();
    auto error=packet.text();
    const auto rollback=packet.get();
    packet.end();
    Check(code<=2&&rollback<=uint64_t(Storage::Rollback::Failed),"Invalid volume session result");
    return {unsigned(code),std::move(error),Storage::Rollback(rollback)};
}
}
