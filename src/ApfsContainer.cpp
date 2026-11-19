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

Container Superblock(Source& source,uint64_t offset,uint64_t length)
{
    Check(Range(offset,length,source.Size())&&length>=4096,"Truncated APFS container");
    const auto prefix=Disk::Read(source,offset,48);
    Check(!memcmp(prefix.data()+MagicOffset,"NXSB",4),"Missing APFS container signature");
    const auto blockSize=Le32(prefix.data()+36);
    const auto blocks=Le64(prefix.data()+40);
    Check(blockSize>=4096&&blockSize<=65536&&(blockSize&(blockSize-1))==0&&
          blocks>=MinimumSize/blockSize&&blocks<=length/blockSize,"Invalid APFS block geometry");
    const auto block=Disk::Read(source,offset,blockSize);
    const auto data=block.data();
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
        return Superblock(source,part.offset,part.length);
    }
    if(memcmp(boot.data()+MagicOffset,"NXSB",4))return {};
    return Superblock(source,0,source.Size());
}
}
