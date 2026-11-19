// SPDX-License-Identifier: GPL-2.0-or-later
#include "NdifImage.h"
#include "DmgImage.h"
#include "HfsPlusDisk.h"
#include <mutex>
#include <optional>
extern "C" {
#include "hfs/macbinary.h"
}

namespace Ndif {
namespace {
using namespace HfsPlus;
using namespace Disk;
constexpr uint64_t Sector=512;
constexpr auto HeaderBytes=128u;
constexpr auto ChunkBytes=12u;
constexpr uint64_t MaxCompressedChunk=64*1024*1024;
enum ChunkType : uint8_t { Zero=0,Raw=2,Adc=0x83,End=0xff };
struct Fork {uint64_t at=0,length=0;};
struct Forks {Fork data,resource;};
struct Chunk {uint64_t at,length,packedAt,packedLength;uint8_t type;};
struct Layout {uint64_t length;std::vector<Chunk> chunks;};

std::optional<Forks> MacBinary(Source& input)
{
    if(input.Size()<128)return {};
    auto h=Disk::Read(input,0,128);
    const int version=macbinary_version(h.data());
    if(!version||!U32(h.data()+87))return {};
    const auto aligned=[](uint64_t n){return (n+127)&~uint64_t(127);};
    const uint64_t data=128+(version==MacBinaryI?0:aligned(U16(h.data()+120)));
    const uint64_t length=U32(h.data()+83),resource=U32(h.data()+87);
    const uint64_t resourceAt=data+aligned(length);
    if(!Range(data,length,input.Size())||!Range(resourceAt,resource,input.Size())){
        Check(memcmp(h.data()+69,"ddsk",4)!=0,"Truncated NDIF MacBinary forks");
        return {};
    }
    return Forks{{data,length},{resourceAt,resource}};
}

std::optional<Fork> BlockMap(Source& input,Fork resource)
{
    if(resource.length<16)return {};
    const auto header=Disk::Read(input,resource.at,16);
    const uint64_t data=U32(header.data()),map=U32(header.data()+4);
    const uint64_t dataBytes=U32(header.data()+8),mapBytes=U32(header.data()+12);
    if(data<16||map<16||mapBytes<30||!Range(data,dataBytes,resource.length)||
       !Range(map,mapBytes,resource.length)||!(data+dataBytes<=map||map+mapBytes<=data))
        return {};
    const auto mapAt=resource.at+map;
    const auto head=Disk::Read(input,mapAt,28);
    const uint64_t types=U16(head.data()+24),names=U16(head.data()+26);
    if(types<28||names>mapBytes||!Range(types,2,names))return {};
    const auto count=uint16_t(U16(Disk::Read(input,mapAt+types,2).data())+1u);
    const uint64_t refs=types+2+uint64_t(count)*8;
    if(refs>names)return {};
    const auto list=Disk::Read(input,mapAt+types+2,size_t(count)*8);
    std::optional<Fork> found;
    for(unsigned i=0;i<count;++i){
        const auto p=list.data()+i*8;
        if(memcmp(p,"bcem",4))continue;
        const auto entries=uint16_t(U16(p+4)+1u);
        const uint64_t start=types+U16(p+6);
        Check(start>=refs&&Range(start,uint64_t(entries)*12,names),"Invalid NDIF resource references");
        const auto records=Disk::Read(input,mapAt+start,size_t(entries)*12);
        for(unsigned j=0;j<entries;++j){
            const auto record=records.data()+j*12;
            if(U16(record)!=128)continue;
            const auto offset=U32(record+4)&0xffffff;
            Check(!found&&Range(offset,4,dataBytes),"Invalid or duplicate NDIF block map");
            const auto length=U32(Disk::Read(input,resource.at+data+offset,4).data());
            Check(Range(uint64_t(offset)+4,length,dataBytes),"Truncated NDIF block map");
            found=Fork{resource.at+data+offset+4,length};
        }
    }
    return found;
}

Layout Map(Source& input,Fork map,Fork data)
{
    Check(map.length>=HeaderBytes,"Truncated NDIF header");
    const auto header=Disk::Read(input,map.at,HeaderBytes);
    const auto p=header.data();
    const auto version=U16(p);
    Check(version>=10&&version<=12,"Unsupported NDIF header version");
    Check(p[4]<=63&&U32(p+84)==0,"Unsupported segmented NDIF image or invalid name");
    const uint64_t sectors=U32(p+68),maxChunk=U32(p+72),base=U32(p+76),count=U32(p+124);
    Check(sectors&&sectors<=0xffffff&&base<=data.length&&count>=2&&count<=sectors+1&&
          count*ChunkBytes==map.length-HeaderBytes,"Invalid NDIF block map geometry");
    Layout out{sectors*Sector,{}};
    constexpr unsigned Batch=1024;
    out.chunks.reserve(size_t(count-1));
    for(uint64_t index=0;index<count;){
        const auto n=(std::min)(uint64_t(Batch),count-index);
        const auto rows=Disk::Read(input,map.at+HeaderBytes+index*ChunkBytes,size_t(n*ChunkBytes));
        for(uint64_t j=0;j<n;++j){
            const auto row=rows.data()+j*ChunkBytes;
            const uint64_t sector=U32(row)>>8;
            const auto type=row[3];
            const uint64_t at=U32(row+4),packed=U32(row+8);
            Check(out.chunks.empty()?sector==0:sector>out.chunks.back().at/Sector,
                  "Unordered NDIF block map");
            Check(sector<=sectors,"NDIF chunk begins outside the disk");
            if(!out.chunks.empty())out.chunks.back().length=sector*Sector-out.chunks.back().at;
            if(index+j+1==count){
                Check(type==End&&sector==sectors&&!packed,"Invalid NDIF terminator");
            }else{
                Check(sector<sectors,"Empty NDIF chunk");
                if(type==Zero)Check(!packed,"Zero NDIF chunk contains data");
                else{
                    Check(type==Raw||type==Adc,"Unsupported NDIF chunk encoding");
                    Check(packed&&Range(at,packed,data.length-base),"NDIF chunk exceeds its data fork");
                }
                out.chunks.push_back({sector*Sector,0,data.at+base+at,packed,type});
            }
        }
        index+=n;
    }
    for(const auto& chunk:out.chunks){
        if(chunk.type==Raw)Check(chunk.length==chunk.packedLength,"Incorrect NDIF raw chunk length");
        if(chunk.type==Adc)Check(chunk.length<=maxChunk*Sector&&chunk.length<=MaxCompressedChunk&&
                                chunk.packedLength<=MaxCompressedChunk,"Invalid NDIF compressed chunk size");
    }
    return out;
}

class Image final : public Source {
    std::unique_ptr<Source> input_;
    Layout layout_;
    std::mutex mutex_;
    size_t cached_=SIZE_MAX;
    Bytes decoded_;
public:
    Image(std::unique_ptr<Source> input,Layout layout):input_(std::move(input)),layout_(std::move(layout)){}
    uint64_t Size()const override{return layout_.length;}
    bool Writable()const override{return false;}
    bool ProcessLocal()const override{return input_->ProcessLocal();}
    void CheckMedia()override{input_->CheckMedia();}
    auto WorkerFiles()const -> std::vector<std::pair<std::wstring,uintptr_t>> override{return input_->WorkerFiles();}
    void Write(uint64_t,const void*,size_t)override{throw Error("NDIF images are read-only");}
    void Flush()override{}
    void Read(uint64_t offset,void* bytes,size_t count)override{
        Check(Range(offset,count,Size())&&(!count||bytes),"Read outside NDIF image");
        std::lock_guard<std::mutex> lock(mutex_);
        auto out=static_cast<uint8_t*>(bytes);
        while(count){
            const auto it=std::upper_bound(layout_.chunks.begin(),layout_.chunks.end(),offset,
                [](uint64_t n,const Chunk& chunk){return n<chunk.at;});
            const auto chunk=std::prev(it);
            const auto within=offset-chunk->at;
            const auto take=size_t((std::min)(uint64_t(count),chunk->length-within));
            if(chunk->type==Zero)memset(out,0,take);
            else if(chunk->type==Raw)input_->Read(chunk->packedAt+within,out,take);
            else{
                const auto index=size_t(chunk-layout_.chunks.begin());
                if(index!=cached_){
                    const auto packed=Disk::Read(*input_,chunk->packedAt,size_t(chunk->packedLength));
                    Bytes decoded(size_t(chunk->length));
                    Check(Dmg::DecodeAppleDataCompression(packed,decoded),"Corrupt NDIF ADC data");
                    decoded_=std::move(decoded);
                    cached_=index;
                }
                memcpy(out,decoded_.data()+size_t(within),take);
            }
            offset+=take;
            out+=take;
            count-=take;
        }
    }
};
}

bool Unwrap(std::unique_ptr<Source>& input)
{
    auto forks=MacBinary(*input);
    if(!forks)return false;
    const auto map=BlockMap(*input,forks->resource);
    if(!map)return false;
    auto layout=Map(*input,*map,forks->data);
    input=std::make_unique<Image>(std::move(input),std::move(layout));
    return true;
}
}
