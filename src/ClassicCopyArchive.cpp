// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "ClassicCopyArchive.h"
#include "CopyWorkerProtocol.h"
#include "HfsPlusCompression.h"
#include <bcrypt.h>
#include <algorithm>
#include <set>

namespace ClassicCopyArchive {
namespace {
using CopyWorkerProtocol::Data;
using CopyWorkerProtocol::Check;
using MacBrowser::Entry;
using HfsPlus::AttributeData;

constexpr uint64_t signatureV1=0x3141564648534648;
constexpr uint64_t signatureV2=0x3241564648534648;
constexpr uint64_t signatureV3=0x3341564648534648;
constexpr uint64_t signatureV4=0x3441564648534648;
constexpr uint64_t maximumFork=0x7fffffff;
constexpr uint64_t maximumHeader=1024*1024;

void Cancel(Storage::Job& job)
{
    if(job.cancelled())throw Storage::Cancelled();
}

void PutDate(Data& data,std::optional<uint64_t> date)
{
    data.put(date.has_value());
    if(date)data.put(*date);
}

std::optional<uint64_t> GetDate(Data& data)
{
    if(!data.flag())return {};
    return data.get();
}

class Hash {
    BCRYPT_ALG_HANDLE algorithm_=nullptr;
    BCRYPT_HASH_HANDLE hash_=nullptr;
    HfsPlus::Bytes state_;
public:
    Hash(){
        Check(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,
              "Cannot initialize classic archive hashing");
        try{
            DWORD size=0,read=0;
            Check(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<BYTE*>(&size),
                                    sizeof(size),&read,0)>=0&&read==sizeof(size),
                  "Cannot size classic archive hash state");
            state_.resize(size);
            Check(BCryptCreateHash(algorithm_,&hash_,state_.data(),size,nullptr,0,0)>=0,
                  "Cannot create classic archive hash");
        }catch(...){
            BCryptCloseAlgorithmProvider(algorithm_,0);
            throw;
        }
    }
    ~Hash(){BCryptDestroyHash(hash_);BCryptCloseAlgorithmProvider(algorithm_,0);}
    Hash(const Hash&)=delete;
    Hash& operator=(const Hash&)=delete;
    void Add(const void* bytes,size_t count){
        Check(count<=ULONG_MAX&&BCryptHashData(hash_,const_cast<BYTE*>(static_cast<const BYTE*>(bytes)),
                                              ULONG(count),0)>=0,"Cannot hash classic archive bytes");
    }
    Digest Finish(){
        Digest digest{};
        Check(BCryptFinishHash(hash_,digest.data(),DWORD(digest.size()),0)>=0,"Cannot finish classic archive hash");
        return digest;
    }
};
}

static Digest Fingerprint(MacBrowser::Volume& volume,const Entry& entry,Storage::Job& job,bool nativeEdits)
{
    Data identity;
    identity.put(entry.resolvedId);
    identity.u16(entry.name);
    identity.put(entry.directory);
    if(!nativeEdits){
        identity.put(entry.protectedFile);
        identity.put(entry.created);
        identity.put(entry.modified);
        PutDate(identity,entry.additionalDates.backup);
        identity.put(std::string(reinterpret_cast<const char*>(entry.finder.data()),entry.finder.size()));
    }
    for(const auto& fork:entry.forks)identity.put(fork.size);
    Hash hash;
    hash.Add(identity.bytes.data(),identity.bytes.size());
    if(!entry.directory){
        HfsPlus::Bytes buffer(64*1024);
        for(unsigned fork=0;fork<2;++fork){
            for(uint64_t offset=0;offset<entry.forks[fork].size;){
                Cancel(job);
                const auto count=size_t((std::min)(uint64_t(buffer.size()),entry.forks[fork].size-offset));
                volume.Read(entry.id,fork,offset,buffer.data(),count);
                hash.Add(buffer.data(),count);
                offset+=count;
            }
        }
    }
    Cancel(job);
    return hash.Finish();
}

namespace {
struct Envelope {
    Data prefix;
    uint64_t bodySize;
};

Envelope MakeEnvelope(const Metadata& metadata,const Digest& binding,const Entry& projected,Storage::Job& job)
{
    const auto& entry=metadata.entry;
    Data header;
    header.put(std::string(reinterpret_cast<const char*>(binding.data()),binding.size()));
    header.put(projected.created);
    header.put(projected.modified);
    header.put(projected.additionalDates.backup.value_or(0));
    header.put(projected.protectedFile);
    header.put(std::string(reinterpret_cast<const char*>(projected.finder.data()),projected.finder.size()));
    header.put(entry.directory);
    header.u16(entry.name);
    header.put(entry.datesValid);
    header.put(entry.created);
    header.put(entry.modified);
    PutDate(header,entry.additionalDates.accessed);
    PutDate(header,entry.additionalDates.changed);
    PutDate(header,entry.additionalDates.backup);
    header.put(entry.apfsTimestamps.has_value());
    if(entry.apfsTimestamps)for(auto date:*entry.apfsTimestamps)header.put(date);
    header.put(entry.posix.has_value());
    if(entry.posix){
        header.put(entry.posix->uid);header.put(entry.posix->gid);header.put(entry.posix->mode);
        header.put(entry.posix->protection);header.put(entry.posix->flags);
        header.put(uint8_t(entry.posix->special));header.put(entry.posix->device);
    }
    header.put(metadata.symbolicLinkTarget.has_value());
    if(metadata.symbolicLinkTarget){
        const auto& target=*metadata.symbolicLinkTarget;
        header.put(std::string(reinterpret_cast<const char*>(target.data()),target.size()));
    }
    header.put(metadata.linkGroup);
    header.put(metadata.compressionType);
    header.put(std::string(entry.inlineCompression.begin(),entry.inlineCompression.end()));
    header.put(std::string(reinterpret_cast<const char*>(entry.finder.data()),entry.finder.size()));
    Check(header.bytes.size()<=maximumHeader,"Classic metadata companion header is too large");
    header.put(entry.attributes.size());
    uint64_t payloadSize=0;
    for(const auto& [name,value]:entry.attributes){
        Cancel(job);
        Check(value.size()<=maximumFork-payloadSize,"Classic metadata companion exceeds the fork limit");
        payloadSize+=value.size();
        header.u16(name);
        header.put(value.size());
        Check(header.bytes.size()<=maximumHeader,"Classic metadata companion header is too large");
    }
    Data prefix;
    prefix.put(signatureV4);
    prefix.put(header.bytes.size());
    prefix.bytes.insert(prefix.bytes.end(),header.bytes.begin(),header.bytes.end());
    Check(payloadSize<=maximumFork-prefix.bytes.size()-Digest{}.size(),
          "Classic metadata companion exceeds the fork limit");
    const auto bodySize=payloadSize+prefix.bytes.size();
    return {std::move(prefix),bodySize};
}
}

uint64_t EncodedSize(const Metadata& metadata,Storage::Job& job)
{
    return MakeEnvelope(metadata,{},metadata.entry,job).bodySize+Digest{}.size();
}

AttributeData Encode(const Metadata& metadata,const Digest& binding,Storage::Job& job,const Entry& projected)
{
    const auto [prefix,bodySize]=MakeEnvelope(metadata,binding,projected,job);
    const auto& entry=metadata.entry;
    auto next=entry.attributes.begin();
    uint64_t payloadOffset=0;
    Hash checksum;
    std::optional<Digest> digest;
    return AttributeData::Snapshot(bodySize+Digest{}.size(),[&](uint64_t at,void* output,size_t requested){
        Cancel(job);
        auto* bytes=static_cast<uint8_t*>(output);
        const auto bodyCount=at<bodySize?size_t((std::min)(uint64_t(requested),bodySize-at)):0;
        auto count=bodyCount;
        if(at<prefix.bytes.size()){
            const auto take=(std::min)(count,prefix.bytes.size()-size_t(at));
            memcpy(bytes,prefix.bytes.data()+at,take);
            at+=take;bytes+=take;count-=take;
        }
        while(count){
            const auto offset=at-prefix.bytes.size();
            while(next!=entry.attributes.end()&&offset-payloadOffset>=next->second.size()){
                payloadOffset+=next->second.size();
                ++next;
            }
            Check(next!=entry.attributes.end(),"Missing classic metadata companion payload");
            const auto take=size_t((std::min)(uint64_t(count),next->second.size()-(offset-payloadOffset)));
            next->second.Read(offset-payloadOffset,bytes,take);
            at+=take;bytes+=take;count-=take;
        }
        if(bodyCount)checksum.Add(output,bodyCount);
        if(requested>bodyCount){
            if(!digest)digest=checksum.Finish();
            memcpy(bytes,digest->data()+size_t(at-bodySize),requested-bodyCount);
        }
    });
}

std::u16string CompanionName(uint32_t catalogId)
{
    constexpr char16_t digits[]=u"0123456789ABCDEF";
    std::u16string name=u"HFV Metadata ";
    for(unsigned shift=32;shift;shift-=4)name.push_back(digits[(catalogId>>(shift-4))&15]);
    return name;
}

bool ReservedName(std::u16string_view name)
{
    constexpr std::u16string_view prefix=u"HFV Metadata ";
    if(name.size()!=prefix.size()+8)return false;
    if(HfsPlus::CompareNames(std::u16string(name.substr(0,prefix.size())),std::u16string(prefix),false)!=0)
        return false;
    return std::all_of(name.begin()+prefix.size(),name.end(),[](char16_t ch){
        return (ch>=u'0'&&ch<=u'9')||(ch>=u'A'&&ch<=u'F')||(ch>=u'a'&&ch<=u'f');
    });
}

namespace {
bool CompanionType(const Entry& entry)
{
    constexpr std::array<uint8_t,8> finderType{'H','F','V','M','H','F','V','E'};
    return !entry.directory&&std::equal(finderType.begin(),finderType.end(),entry.finder.begin());
}

std::optional<Pair> CutPair(MacBrowser::Volume& volume,const Entry& item,Storage::Job& job)
{
    Entry companion;
    if(!volume.FindChild(item.parent,CompanionName(item.resolvedId),companion)||!CompanionType(companion))return {};
    return RequireVerifiedPair(volume,item,job);
}

bool CheckCut(MacBrowser::Volume& volume,const Entry& item,Storage::Job& job,unsigned depth)
{
    Cancel(job);
    Check(depth<=128,"Excessive directory depth while checking classic archive companions");
    Check(!(ReservedName(item.name)&&CompanionType(item)),
          "Select the archived item rather than its metadata companion. Source retained.");
    bool archived=CutPair(volume,item,job).has_value();
    if(!item.directory)return archived;
    const auto children=volume.List(item.id);
    std::set<std::string> expected;
    for(const auto& child:children){
        if(ReservedName(child.name)&&CompanionType(child))continue;
        if(CheckCut(volume,child,job,depth+1)){
            expected.insert(MacBrowser::ClassicCopyNameKey(CompanionName(child.resolvedId)));
            archived=true;
        }
    }
    for(const auto& child:children)
        if(ReservedName(child.name)&&CompanionType(child))
            Check(expected.count(MacBrowser::ClassicCopyNameKey(child.name))!=0,
                  "A classic metadata companion has no verified item. Source retained.");
    return archived;
}
}

bool ValidateCut(MacBrowser::Volume& volume,const Entry& item,Storage::Job& job)
{
    return CheckCut(volume,item,job,0);
}

void RemoveArchive(MacBrowser::Volume& volume,uint32_t id,Storage::Job& job,bool batchActive,bool deletionProgress)
{
    const auto item=volume.Stat(id);
    if(!ValidateCut(volume,item,job)){
        MacBrowser::RemoveTree(volume,id,job,deletionProgress);
        return;
    }
    const auto pair=CutPair(volume,item,job);
    const auto remove=[&](bool durable){
        Check(durable,"Deleting a classic HFS archive requires write recovery");
        MacBrowser::RemoveTree(volume,id,job,deletionProgress);
        Cancel(job);
        if(pair)RemoveCompanion(volume,pair->companion);
        Cancel(job);
    };
    if(batchActive)remove(true);
    else volume.RunWriteBatch(job,remove);
}

static std::optional<Metadata> DecodeArchive(const AttributeData&,Storage::Job&,
    const Digest&,const Entry*,bool);

enum class Health {Missing,Unrelated,Stale,Corrupt,Verified};

struct Inspection {
    Health health;
    std::optional<Pair> pair;
};

static Inspection Inspect(MacBrowser::Volume& volume,const Entry& item,Storage::Job& job,bool preserveEdits)
{
    Entry companion;
    if(!volume.FindChild(item.parent,CompanionName(item.resolvedId),companion))
        return {Health::Missing,{}};
    if(!CompanionType(companion))return {Health::Unrelated,{}};
    if(companion.forks[1].size||companion.forks[0].size>maximumFork)
        return {Health::Corrupt,{}};
    const auto stored=AttributeData::Snapshot(companion.forks[0].size,[&](uint64_t at,void* out,size_t count){
        Cancel(job);
        volume.Read(companion.id,0,at,out,count);
    });
    // Compute the binding separately so failed reads of the native file stay I/O errors.
    Data prefix;
    if(stored.size()<16+Digest{}.size())return {Health::Corrupt,{}};
    prefix.bytes.resize(8);
    stored.Read(0,prefix.bytes.data(),prefix.bytes.size());
    const auto signature=prefix.get();
    if(signature!=signatureV1&&signature!=signatureV2&&signature!=signatureV3&&signature!=signatureV4)
        return {Health::Corrupt,{}};
    const auto binding=Fingerprint(volume,item,job,signature==signatureV4);
    try{
        auto result=DecodeArchive(stored,job,binding,&item,preserveEdits);
        if(!result)return {Health::Stale,{}};
        Check(result->entry.directory==item.directory,"Classic companion does not match its object type");
        return {Health::Verified,Pair{std::move(companion),std::move(*result)}};
    }catch(const HfsPlus::Error&){
        return {Health::Corrupt,{}};
    }
}

std::optional<Metadata> Load(MacBrowser::Volume& volume,const Entry& item,Storage::Job& job,const char** copyWarning)
{
    auto result=Inspect(volume,item,job,true);
    const char* warning=nullptr;
    if(result.health==Health::Corrupt)warning="The classic metadata companion is corrupt.";
    if(result.health==Health::Stale)warning="The classic metadata companion is stale.";
    if(copyWarning)*copyWarning=warning;
    else if(warning)throw HfsPlus::Error(std::string(warning)+" Source files were retained.");
    if(result.pair)return std::move(result.pair->metadata);
    return {};
}

std::optional<Pair> RequireVerifiedPair(MacBrowser::Volume& volume,const Entry& item,Storage::Job& job)
{
    auto result=Inspect(volume,item,job,false);
    Check(result.health!=Health::Unrelated,"An unrelated file occupies the classic metadata companion name");
    Check(result.health!=Health::Corrupt,"The existing classic metadata companion is corrupt. Both files were retained.");
    Check(result.health!=Health::Stale,"The existing classic metadata companion is stale. Both files were retained.");
    return std::move(result.pair);
}

void RemoveCompanion(MacBrowser::Volume& volume,const Entry& expected)
{
    Entry current;
    Check(volume.FindChild(expected.parent,expected.name,current)&&current.identity==expected.identity&&
          current.resolvedId==expected.resolvedId&&CompanionType(current),
          "Classic metadata companion changed during the operation");
    volume.Remove(current.id);
}

void Save(MacBrowser::Volume& volume,uint32_t id,const Metadata& source,Storage::Job& job)
{
    const auto item=volume.Stat(id);
    Entry companion,existing;
    companion.name=CompanionName(item.resolvedId);
    Check(!volume.FindChild(item.parent,companion.name,existing),"A file already uses the classic companion name");
    const auto stored=Encode(source,Fingerprint(volume,item,job,true),job,item);
    companion.forks[0].size=stored.size();
    memcpy(companion.finder.data(),"HFVMHFVE",8);
    volume.PutForTransfer(item.parent,companion,[&](unsigned fork,uint64_t at,void* out,size_t count){
        Cancel(job);
        Check(!fork,"Unexpected classic companion resource fork");
        stored.Read(at,out,count);
    },false,job);
    Check(Load(volume,item,job).has_value(),"Classic metadata companion failed readback verification");
}

static std::optional<Metadata> DecodeArchive(const AttributeData& archive,Storage::Job& job,
    const Digest& binding,const Entry* current,bool preserveEdits)
{
    constexpr auto digestSize=Digest{}.size();
    if(archive.size()<16+digestSize||archive.size()>maximumFork)return {};
    const auto bodySize=archive.size()-digestSize;
    Data prefix;
    prefix.bytes.resize(16);
    archive.Read(0,prefix.bytes.data(),prefix.bytes.size());
    const auto signature=prefix.get();
    const bool projected=signature==signatureV4;
    if(signature!=signatureV1&&signature!=signatureV2&&signature!=signatureV3&&!projected)return {};
    const auto headerSize=prefix.get();
    Check(headerSize<=maximumHeader&&headerSize<=bodySize-16,"Invalid classic metadata companion header");
    Data header;
    header.bytes.resize(size_t(headerSize));
    archive.Read(16,header.bytes.data(),header.bytes.size());
    const auto expected=header.text();
    Hash checksum;
    HfsPlus::Bytes buffer(64*1024);
    for(uint64_t at=0;at<bodySize;){
        Cancel(job);
        const auto count=size_t((std::min)(uint64_t(buffer.size()),bodySize-at));
        archive.Read(at,buffer.data(),count);
        checksum.Add(buffer.data(),count);
        at+=count;
    }
    Digest stored{};
    archive.Read(bodySize,stored.data(),stored.size());
    Check(checksum.Finish()==stored,"Classic metadata companion checksum mismatch");
    Check(expected.size()==binding.size(),"Invalid classic companion binding size");
    if(memcmp(expected.data(),binding.data(),binding.size()))return {};
    Entry baseline;
    if(projected){
        baseline.created=header.get();
        baseline.modified=header.get();
        baseline.additionalDates.backup=header.get();
        baseline.protectedFile=header.flag();
        const auto finder=header.text();
        Check(finder.size()==baseline.finder.size(),"Invalid classic companion Finder baseline");
        memcpy(baseline.finder.data(),finder.data(),finder.size());
    }
    Metadata metadata;
    auto& entry=metadata.entry;
    entry.directory=header.flag();
    if(signature!=signatureV1)entry.name=header.u16();
    entry.datesValid=header.flag();
    entry.created=header.get();
    entry.modified=header.get();
    entry.additionalDates.accessed=GetDate(header);
    entry.additionalDates.changed=GetDate(header);
    entry.additionalDates.backup=GetDate(header);
    if(header.flag()){
        entry.apfsTimestamps.emplace();
        for(auto& date:*entry.apfsTimestamps)date=header.get();
    }
    if(header.flag()){
        const auto uid=header.get(),gid=header.get(),mode=header.get(),protection=header.get(),flags=header.get();
        Check(uid<=UINT32_MAX&&gid<=UINT32_MAX&&mode<=PosixMetadata::ModeMask&&
              protection<=PosixMetadata::ProtectionMask&&flags<=PosixMetadata::HintMask,
              "Invalid classic metadata companion permissions");
        entry.posix=PosixMetadata{uint32_t(uid),uint32_t(gid),uint16_t(mode),uint8_t(protection),uint8_t(flags)};
        if(signature==signatureV3||projected){
            const auto kind=header.get(),device=header.get();
            Check(kind<=15&&(!kind||SpecialType(unsigned(kind))!=SpecialFileType::None)&&device<=UINT32_MAX,
                  "Invalid classic companion special-file type");
            entry.posix->special=SpecialType(unsigned(kind));
            Check(!device||IsDevice(entry.posix->special),"Unexpected classic companion device identifier");
            entry.posix->device=uint32_t(device);
        }
    }
    if(signature==signatureV3||projected){
        if(header.flag()){
            const auto target=header.text();
            Check(!entry.directory&&(!entry.posix||!entry.posix->IsSpecial())&&!target.empty(),
                  "Invalid classic companion symbolic link");
            metadata.symbolicLinkTarget.emplace(target.begin(),target.end());
        }
        metadata.linkGroup=header.text();
        Check(metadata.linkGroup.empty()||metadata.linkGroup.size()==16,"Invalid classic companion link group");
        const auto type=header.get();
        Check(type<=14&&type!=2&&type!=5&&type!=6,"Invalid classic companion compression type");
        metadata.compressionType=uint32_t(type);
        const auto inlineBytes=header.text();
        Check(inlineBytes.empty()||(type&&inlineBytes.size()<=HfsPlus::InlineCompressionLimit),
              "Invalid classic companion inline compression");
        entry.inlineCompression.assign(inlineBytes.begin(),inlineBytes.end());
        const auto finder=header.text();
        Check(finder.size()==entry.finder.size(),"Invalid classic companion Finder information");
        memcpy(entry.finder.data(),finder.data(),finder.size());
    }
    const auto count=header.get();
    Check(count<=(header.bytes.size()-header.at)/16,"Invalid classic metadata companion attribute count");
    uint64_t at=16+headerSize;
    for(uint64_t i=0;i<count;++i){
        Cancel(job);
        auto name=header.u16();
        const auto size=header.get();
        Check(!name.empty()&&size<=bodySize-at,"Invalid classic metadata companion attribute");
        auto value=AttributeData::Snapshot(size,[&](uint64_t offset,void* output,size_t length){
            Cancel(job);
            archive.Read(at+offset,output,length);
        });
        Check(entry.attributes.emplace(std::move(name),std::move(value)).second,
              "Duplicate classic metadata companion attribute");
        at+=size;
    }
    header.end();
    Check(at==bodySize,"Trailing classic metadata companion payload");
    entry.attributesKnown=true;
    if(projected&&current){
        const bool created=current->created!=baseline.created;
        const bool modified=current->modified!=baseline.modified;
        const bool backup=current->additionalDates.backup.value_or(0)!=baseline.additionalDates.backup.value_or(0);
        const bool locked=current->protectedFile!=baseline.protectedFile;
        const bool finder=current->finder!=baseline.finder;
        if(!preserveEdits&&(created||modified||backup||locked||finder))return {};
        constexpr uint64_t epoch=2082844800;
        constexpr uint64_t nanos=1000000000;
        if(created){
            entry.created=current->created;
            if(entry.apfsTimestamps)(*entry.apfsTimestamps)[0]=(current->created-epoch)*nanos;
        }
        if(modified){
            entry.modified=current->modified;
            if(entry.apfsTimestamps)(*entry.apfsTimestamps)[1]=(current->modified-epoch)*nanos;
        }
        if(backup)entry.additionalDates.backup=current->additionalDates.backup;
        if(finder)entry.finder=current->finder;
        if(locked&&entry.posix){
            if(current->protectedFile)entry.posix->protection|=PosixMetadata::UserImmutable;
            else entry.posix->protection&=~PosixMetadata::UserImmutable;
        }
    }
    return metadata;
}

std::optional<Metadata> Decode(const AttributeData& archive,const Digest& binding,Storage::Job& job)
{
    return DecodeArchive(archive,job,binding,nullptr,false);
}
}
