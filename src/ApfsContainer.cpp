// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsContainer.h"
#include "DiskPartitions.h"
#include "HfsPlusDisk.h"

namespace Apfs {
namespace {
using namespace HfsPlus;
using Disk::Check;
using Disk::Range;
uint32_t Le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
uint64_t Le64(const uint8_t* p){return Le32(p) | uint64_t(Le32(p+4))<<32;}
constexpr uint64_t MinimumSize=1024*1024;
constexpr size_t MagicOffset=32;
constexpr size_t VolumeArrayOffset=184;
constexpr uint32_t MaxVolumes=100;

Container Superblock(const Bytes& block,uint64_t offset,uint64_t length)
{
    Check(block.size()>=4096&&!memcmp(block.data()+MagicOffset,"NXSB",4),"Missing APFS container signature");
    const auto data=block.data();
    const auto blockSize=Le32(data+36);
    const auto blocks=Le64(data+40);
    Check(blockSize>=4096&&blockSize<=65536&&(blockSize&(blockSize-1))==0&&
          block.size()==blockSize&&blocks>=MinimumSize/blockSize&&blocks<=length/blockSize,
          "Invalid APFS block geometry");
    Check(Le64(data)==ObjectChecksum(block),"APFS superblock checksum mismatch");
    Check(Le64(data+8)==1&&(Le32(data+24)&0xffff)==1&&Le32(data+28)==0,
          "Invalid APFS container object header");
    Container result{offset,blocks,Le64(data+16),Le64(data+160),blockSize,{},{}};
    memcpy(result.uuid.data(),data+72,result.uuid.size());
    Check(result.transaction&&result.objectMap&&result.objectMap<blocks,"Invalid APFS container object map");
    const auto capacity=Le32(data+180);
    Check(capacity<=MaxVolumes,"APFS volume table exceeds its superblock");
    std::set<uint64_t> ids;
    for(uint32_t i=0;i<MaxVolumes;++i){
        const auto id=Le64(data+VolumeArrayOffset+i*8);
        if(!id)continue;
        Check(i<capacity&&ids.insert(id).second,"Invalid or duplicate APFS volume ID");
        result.volumes.push_back(id);
    }
    return result;
}

struct Ring {
    uint64_t base;
    uint32_t capacity,index,used;
};
Ring ReadRing(const uint8_t* data,uint64_t blocks,bool descriptors)
{
    const auto base=Le64(data+(descriptors?112:120));
    const auto count=Le32(data+(descriptors?104:108));
    Check(!(count&0x80000000),"Noncontiguous APFS checkpoint areas are unsupported");
    Check(!count||(base&&Range(base,count,blocks)),"APFS checkpoint area extends outside the container");
    return {base,count,Le32(data+(descriptors?136:144)),Le32(data+(descriptors?140:148))};
}
uint64_t Slot(const Ring& ring,uint64_t index)
{
    return ring.base+index%ring.capacity;
}

bool ValidateMappings(Source& source,const Container& container,const Ring& desc,const Ring& data)
{
    constexpr size_t MapHeader=40,MappingBytes=40;
    std::set<std::pair<uint64_t,uint64_t>> objects;
    for(uint32_t i=0;i+1<desc.used;++i){
        const auto slot=Slot(desc,uint64_t(desc.index)+i);
        const auto map=Disk::Read(source,container.offset+slot*container.blockSize,container.blockSize);
        const auto p=map.data();
        const auto count=Le32(p+36);
        if(Le64(p)!=ObjectChecksum(map)||(Le32(p+24)&0xffff)!=12||Le64(p+16)!=container.transaction||
           Le32(p+32)!=unsigned(i+2==desc.used)||count>(map.size()-MapHeader)/MappingBytes)
            return false;
        for(uint32_t j=0;j<count;++j){
            const auto entry=p+MapHeader+j*MappingBytes;
            const auto size=Le32(entry+8);
            const auto volume=Le64(entry+16),id=Le64(entry+24),physical=Le64(entry+32);
            if(!size||size%container.blockSize||physical<data.base||
               !Range(physical-data.base,size/container.blockSize,data.capacity)||
               !id||!objects.emplace(volume,id).second)
                return false;
        }
    }
    return true;
}

Container ReadContainer(Source& source,uint64_t offset,uint64_t length)
{
    Check(Range(offset,length,source.Size())&&length>=4096,"Truncated APFS container");
    const auto prefix=Disk::Read(source,offset,48);
    const auto size=Le32(prefix.data()+36);
    Check(size>=4096&&size<=65536&&(size&(size-1))==0&&size<=length,"Invalid APFS block geometry");
    const auto initial=Disk::Read(source,offset,size);
    auto bootstrap=Superblock(initial,offset,length);
    Check(!(Le64(initial.data()+64)&~uint64_t(2)),"Unsupported APFS container features");
    const auto ring=ReadRing(initial.data(),bootstrap.blockCount,true);
    if(!ring.capacity)return bootstrap;
    std::optional<Container> newest;
    for(uint64_t i=0;i<ring.capacity;++i){
        const auto address=ring.base+i;
        const auto block=Disk::Read(source,offset+address*size,size);
        const auto p=block.data();
        if(memcmp(p+MagicOffset,"NXSB",4)||Le64(p+16)<bootstrap.transaction||
           (newest&&Le64(p+16)<=newest->transaction))
            continue;
        Container candidate;
        Ring desc{},data{};
        try{
            candidate=Superblock(block,offset,length);
            if(candidate.uuid!=bootstrap.uuid)continue;
            desc=ReadRing(p,candidate.blockCount,true);
            data=ReadRing(p,candidate.blockCount,false);
        }catch(const Error&){continue;}
        if(desc.base!=ring.base||desc.capacity!=ring.capacity||desc.index>=desc.capacity||
           desc.used<2||desc.used>desc.capacity||Slot(desc,uint64_t(desc.index)+desc.used-1)!=address||
           !data.capacity||data.index>=data.capacity||data.used>data.capacity||
           !(desc.base+desc.capacity<=data.base||data.base+data.capacity<=desc.base))
            continue;
        if(!ValidateMappings(source,candidate,desc,data))continue;
        Check(!(Le64(p+64)&~uint64_t(2)),"Unsupported APFS checkpoint features");
        candidate.checkpointBlock=address;
        newest=std::move(candidate);
    }
    Check(newest.has_value(),"No complete APFS checkpoint");
    return std::move(*newest);
}
}

uint64_t ObjectChecksum(const HfsPlus::Bytes& object)
{
    // APFS stores the two Fletcher check words before the data they cover.
    constexpr uint64_t modulus=UINT32_MAX;
    Check(object.size()>=8&&object.size()%4==0,"Invalid APFS object checksum range");
    uint64_t first=0,second=0;
    for(size_t i=8;i<object.size();i+=4){
        first=(first+Le32(object.data()+i))%modulus;
        second=(second+first)%modulus;
    }
    const auto low=modulus-(first+second)%modulus;
    const auto high=modulus-(first+low)%modulus;
    return low|(high<<32);
}

std::optional<Container> Probe(Source& source)
{
    if(source.Size()<512)return {};
    const auto boot=Disk::Read(source,0,512);
    using namespace DiskPartitions;
    if(auto partitions=ProbeGptImage(source,boot,GptType::AppleApfs)){
        if(partitions->empty())return {};
        const auto& part=partitions->front();
        return ReadContainer(source,part.offset,part.length);
    }
    if(memcmp(boot.data()+MagicOffset,"NXSB",4))return {};
    return ReadContainer(source,0,source.Size());
}
}
