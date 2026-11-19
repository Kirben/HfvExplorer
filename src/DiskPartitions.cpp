// SPDX-License-Identifier: GPL-2.0-or-later
#include "DiskPartitions.h"
#include "HfsPlusDisk.h"
#include <set>
#include <string_view>
namespace {
using namespace HfsPlus;
namespace B=HfsPlus::Disk;
uint32_t Le32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint64_t Le64(const uint8_t* p){return Le32(p)|uint64_t(Le32(p+4))<<32;}
uint32_t Crc(const Bytes& b){uint32_t crc=~0u;for(auto byte:b){crc^=byte;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&uint32_t(-int(crc&1)));}return ~crc;}
void Ranges(std::vector<std::pair<uint64_t,uint64_t>>& ranges){std::sort(ranges.begin(),ranges.end());for(size_t i=1;i<ranges.size();++i)B::Check(ranges[i-1].second<ranges[i].first,"Overlapping disk partitions");}
bool MacType(const uint8_t* p,DiskPartitions::GptType type)
{
    constexpr uint8_t hfs[]={0,0x53,0x46,0x48,0,0,0xaa,0x11,0xaa,0x11,0,0x30,0x65,0x43,0xec,0xac};
    constexpr uint8_t apfs[]={0xef,0x57,0x34,0x7c,0,0,0xaa,0x11,0xaa,0x11,0,0x30,0x65,0x43,0xec,0xac};
    return !memcmp(p,type==DiskPartitions::GptType::AppleApfs?apfs:hfs,16);
}
}

namespace DiskPartitions {
bool HasProtectiveMbr(const Bytes& boot)
{
    if (boot.size()<512 || boot[510]!=0x55 || boot[511]!=0xaa)
        return false;
    for (unsigned i=0;i<4;++i)
        if (boot[450+i*16]==0xee)
            return true;
    return false;
}

bool HasGptHeader(Source& source,uint32_t sector)
{
    return B::Range(sector,8,source.Size()) &&
           !memcmp(B::Read(source,sector,8).data(),"EFI PART",8);
}

std::vector<MacPartition> ReadGptMacPartitions(Source& source,uint32_t sector,GptType type)
{
    B::Check(sector>=512 && sector<=4096 && (sector&(sector-1))==0 && source.Size()%sector==0,
             "Invalid GPT sector geometry");
    const auto sectors=source.Size()/sector;
    B::Check(sectors>=6,"Truncated GPT disk");
    auto header=[&](uint64_t lba){
        auto bytes=B::Read(source,lba*sector,sector);
        const auto length=Le32(bytes.data()+12);
        B::Check(!memcmp(bytes.data(),"EFI PART",8) && Le32(bytes.data()+8)==0x10000 &&
                 length>=92 && length<=sector && Le32(bytes.data()+20)==0 &&
                 Le64(bytes.data()+24)==lba,"Invalid GPT header");
        bytes.resize(length);
        const auto crc=Le32(bytes.data()+16);
        memset(bytes.data()+16,0,4);
        B::Check(Crc(bytes)==crc,"GPT header checksum mismatch");
        return bytes;
    };
    const auto primary=header(1),backup=header(sectors-1);
    B::Check(Le64(primary.data()+32)==sectors-1 && Le64(backup.data()+32)==1 &&
             !memcmp(primary.data()+40,backup.data()+40,32) &&
             !memcmp(primary.data()+80,backup.data()+80,12),"GPT header copies disagree");
    const auto first=Le64(primary.data()+40),last=Le64(primary.data()+48);
    const auto count=Le32(primary.data()+80),size=Le32(primary.data()+84);
    B::Check(first>1 && last>=first && last<sectors-1 && count && count<=4096 &&
             size>=128 && size<=1024 && (size&(size-1))==0,"Invalid GPT table geometry");
    auto table=[&](const Bytes& h,bool isPrimary){
        const auto at=Le64(h.data()+72),bytes=uint64_t(count)*size;
        const auto blocks=(bytes+sector-1)/sector;
        B::Check(at>1 && at<sectors-1 && blocks<=sectors-1-at &&
                 (isPrimary ? at+blocks<=first : at>last),"GPT table overlaps usable data");
        auto entries=B::Read(source,at*sector,size_t(bytes));
        B::Check(Crc(entries)==Le32(h.data()+88),"GPT table checksum mismatch");
        return entries;
    };
    const auto entries=table(primary,true);
    B::Check(entries==table(backup,false),"GPT tables disagree");

    std::vector<MacPartition> result;
    std::vector<std::pair<uint64_t,uint64_t>> ranges;
    std::set<Bytes> identifiers;
    for (unsigned i=0;i<count;++i) {
        const auto entry=entries.data()+size_t(i)*size;
        if (std::all_of(entry,entry+16,[](uint8_t value){return value==0;}))
            continue;
        const auto begin=Le64(entry+32),end=Le64(entry+40);
        B::Check(begin>=first && end>=begin && end<=last,"GPT partition is outside usable data");
        Bytes id(entry+16,entry+32);
        B::Check(std::any_of(id.begin(),id.end(),[](uint8_t value){return value!=0;}) &&
                 identifiers.insert(id).second,"Invalid or duplicate GPT partition UUID");
        ranges.emplace_back(begin,end);
        if (MacType(entry,type))
            result.push_back({begin*sector,(end-begin+1)*sector,i+1,(Le64(entry+48)&(1ULL<<60))!=0});
    }
    Ranges(ranges);
    return result;
}

std::optional<std::vector<MacPartition>> ProbeGptImage(Source& source,const Bytes& boot,GptType type)
{
    const bool small=HasGptHeader(source,512),large=HasGptHeader(source,4096);
    B::Check(!(small&&large),"Ambiguous GPT image sector size");
    if (small || large)
        return ReadGptMacPartitions(source,small?512:4096,type);
    B::Check(!HasProtectiveMbr(boot),"GPT image has no primary header at a supported sector size");
    return {};
}
std::optional<std::vector<MacPartition>> ProbeApm(Source& source,const Bytes& boot)
{
    const bool hasDescriptor=B::U16(boot.data())==0x4552;
    const uint32_t block=hasDescriptor?B::U16(boot.data()+2):512;
    B::Check(block>=512&&block<=4096&&(block&(block-1))==0,"Invalid Apple partition block size");
    if(!B::Range(block,512,source.Size())){
        B::Check(!hasDescriptor,"Truncated Apple partition map");
        return {};
    }
    auto first=B::Read(source,block,512);
    if(B::U16(first.data())!=0x504d){
        B::Check(!hasDescriptor,"Missing Apple partition map");
        return {};
    }

    const auto count=B::U32(first.data()+4);
    B::Check(count&&count<=4096&&B::Range(uint64_t(count)*block,512,source.Size()),
             "Invalid Apple partition count");
    const auto reservedEnd=(uint64_t(count)+1)*block;
    std::vector<MacPartition> result;
    std::vector<std::pair<uint64_t,uint64_t>> ranges;
    for(uint32_t i=1;i<=count;++i){
        const auto entry=i==1?std::move(first):B::Read(source,uint64_t(i)*block,512);
        B::Check(B::U16(entry.data())==0x504d&&B::U32(entry.data()+4)==count,
                 "Inconsistent Apple partition map");
        const auto start=uint64_t(B::U32(entry.data()+8))*block;
        const auto length=uint64_t(B::U32(entry.data()+12))*block;
        if(!length)continue;
        B::Check(B::Range(start,length,source.Size()),"Apple partition lies outside disk");
        ranges.emplace_back(start,start+length-1);
        auto field=reinterpret_cast<const char*>(entry.data()+48);
        std::string_view type(field,std::find(field,field+32,'\0')-field);
        while(!type.empty()&&type.back()==' ')type.remove_suffix(1);
        if(type=="Apple_HFS"||type=="Apple_HFSX"||type=="Apple_MFS")
            result.push_back({start,length,i,start<reservedEnd});
    }
    Ranges(ranges);
    return result;
}

}
