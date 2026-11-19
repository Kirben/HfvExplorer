// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "SparseDisk.h"
#include "HfsPlusDisk.h"
#include <mutex>

namespace SparseDisk {
namespace {
using namespace HfsPlus;
using namespace Disk;
constexpr uint64_t HeaderSize=4096;
constexpr uint64_t SectorSize=512;
using Bands=std::map<uint64_t,uint64_t>;

class SparseImage final : public Source {
    std::unique_ptr<Source> file_;
    const uint64_t length_, bandSize_;
    const Bands bands_;
    std::mutex mutex_;
public:
    SparseImage(std::unique_ptr<Source> file,uint64_t length,uint64_t bandSize,Bands bands)
        :file_(std::move(file)),length_(length),bandSize_(bandSize),bands_(std::move(bands)){}
    uint64_t Size()const override{return length_;}
    bool Writable()const override{return false;}
    bool ProcessLocal()const override{return file_->ProcessLocal();}
    void CheckMedia()override{file_->CheckMedia();}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return file_->WorkerFiles();}

    void Read(uint64_t offset,void* bytes,size_t count)override{
        Check(Range(offset,count,length_),"Read outside the sparse image");
        std::lock_guard<std::mutex> lock(mutex_);
        auto out=static_cast<uint8_t*>(bytes);
        while(count){
            const auto number=offset/bandSize_, inside=offset%bandSize_;
            const auto band=bands_.lower_bound(number);
            size_t take;
            if(band!=bands_.end()&&band->first==number){
                take=size_t((std::min)(uint64_t(count),bandSize_-inside));
                file_->Read(band->second+inside,out,take);
            }else{
                const auto next=band==bands_.end()?length_:band->first*bandSize_;
                take=size_t((std::min)(uint64_t(count),next-offset));
                memset(out,0,take);
            }
            offset+=take;out+=take;count-=take;
        }
    }
    void Write(uint64_t,const void*,size_t)override{throw Error("Sparse disk images are read-only");}
    void Flush()override{}
};
}

bool Unwrap(std::unique_ptr<Source>& file)
{
    if(file->Size()<4||memcmp(Disk::Read(*file,0,4).data(),"sprs",4))return false;
    const auto fileSize=file->Size();
    Check(fileSize>=HeaderSize,"Truncated sparse image header");
    auto header=Disk::Read(*file,0,HeaderSize);
    Check(U32(header.data()+4)==3,"Unsupported sparse image version");
    const uint64_t sectors=U64(header.data()+28), perBand=U32(header.data()+8);
    Check(perBand&&sectors&&sectors<=uint64_t(INT64_MAX)/SectorSize&&
          uint32_t(sectors)==U32(header.data()+16),"Invalid sparse image geometry");

    const auto length=sectors*SectorSize, bandSize=perBand*SectorSize;
    const auto bandCount=(sectors-1)/perBand+1;
    Bands bands;
    uint64_t at=0;
    uint32_t sequence=0;
    while(true){
        // hdiutil writes the first table at byte 64 and continuation
        // tables at byte 56. A slot gives a one-based logical band.
        const auto next=U64(header.data()+(at?12:20));
        Check(!next||(next>=at+HeaderSize&&next%SectorSize==0&&Range(next,HeaderSize,fileSize)),
              "Invalid sparse image continuation offset");
        const auto dataStart=at+HeaderSize, limit=next?next:fileSize;
        const size_t table=at?56:64;
        for(size_t field=table;field<HeaderSize;field+=4){
            const uint64_t band=U32(header.data()+field);
            if(!band)continue;
            Check(band<=bandCount,"Sparse image band lies outside the disk");
            const uint64_t relative=((field-table)/4)*bandSize;
            const auto bytes=(std::min)(bandSize,length-(band-1)*bandSize);
            Check(Range(relative,bytes,limit-dataStart),"Sparse image band is truncated or overlaps another header");
            Check(bands.emplace(band-1,dataStart+relative).second,"Duplicate sparse image band");
        }
        if(!next)break;
        at=next;
        header=Disk::Read(*file,at,HeaderSize);
        Check(!memcmp(header.data(),"sprs",4)&&U32(header.data()+4)==sequence++,
              "Invalid sparse image continuation header");
    }
    file=std::make_unique<SparseImage>(std::move(file),length,bandSize,std::move(bands));
    return true;
}
}
