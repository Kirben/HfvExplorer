// SPDX-License-Identifier: GPL-2.0-or-later
#include "NdifImage.h"
#include "NdifKenCode.h"
#include "NdifShrinkWrap.h"
#include "NdifLzh.h"
#include "TransferConversion.h"
#include "DmgImage.h"
#include "HfsPlusDisk.h"
#include <windows.h>
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
enum ChunkType : uint8_t { Zero=0,Raw=2,KenCode=0x80,Rle=0x81,Lzh=0x82,Adc=0x83,ShrinkWrap=0xf0,End=0xff };
struct Fork {uint64_t at=0,length=0;};
struct Forks {Fork data,resource;bool writableType=false,rawDisk=false;};
struct Part {
    std::unique_ptr<Source> dataFile,resourceFile;
    Fork data;
    uint64_t start=0;
    uint32_t checksum=0;
};
struct Chunk {uint64_t at,length,packedAt,packedLength;uint8_t type;};
struct Layout {uint64_t length;uint32_t checksum;std::vector<Chunk> chunks;};

std::optional<Forks> MacBinary(Source& input)
{
    if(input.Size()<128)return {};
    auto h=Disk::Read(input,0,128);
    const int version=macbinary_version(h.data());
    const auto p=h.data();
    const bool rawDisk=!memcmp(p+65,"DDimddsk",8);
    if(!version||(!rawDisk&&!U32(p+87)))return {};
    const bool writableType=(rawDisk||!memcmp(p+65,"dimgddsk",8))&&!p[81];
    const auto aligned=[](uint64_t n){return (n+127)&~uint64_t(127);};
    const uint64_t data=128+(version==MacBinaryI?0:aligned(U16(h.data()+120)));
    const uint64_t length=U32(h.data()+83),resource=U32(h.data()+87);
    const uint64_t resourceAt=data+aligned(length);
    if(!Range(data,length,input.Size())||(resource&&!Range(resourceAt,resource,input.Size()))){
        Check(memcmp(h.data()+69,"ddsk",4)!=0,"Truncated NDIF MacBinary forks");
        return {};
    }
    return Forks{{data,length},{resourceAt,resource},writableType,rawDisk};
}


std::optional<Forks> AppleContainer(Source& source,uint32_t magic)
{
    if(source.Size()<26)return {};
    const auto h=Disk::Read(source,0,26);
    const auto version=U32(h.data()+4);
    if(U32(h.data())!=magic||(version!=0x10000&&version!=0x20000))return {};
    const uint64_t bytes=uint64_t(U16(h.data()+24))*12;
    if(!Range(26,bytes,source.Size()))return {};
    const auto entries=Disk::Read(source,26,size_t(bytes));
    const auto first=26+bytes;
    Forks forks;
    std::set<uint32_t> ids;
    std::vector<std::pair<uint64_t,uint64_t>> ranges;
    for(size_t i=0;i<entries.size();i+=12){
        const auto p=entries.data()+i;
        const auto id=U32(p);
        const uint64_t at=U32(p+4),length=U32(p+8);
        if(!ids.insert(id).second||!Range(at,length,source.Size())||(length&&at<first))return {};
        if(length)ranges.emplace_back(at,at+length);
        if(id==1)forks.data={at,length};
        if(id==2)forks.resource={at,length};
    }
    std::sort(ranges.begin(),ranges.end());
    for(size_t i=1;i<ranges.size();++i)
        if(ranges[i-1].second>ranges[i].first)return {};
    if(magic==0x00051607&&ids.count(1))return {};
    return forks;
}

std::unique_ptr<Source> OptionalFile(const std::wstring& path)
{
    // An NTFS stream and an AppleDouble companion are both optional.
    const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                 nullptr,OPEN_EXISTING,0,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return {};
    CloseHandle(handle);
    return HfsPlus::OpenImage(path.c_str(),false);
}

std::optional<Fork> Resource(Source& input,Fork resource,const char* type)
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
        if(memcmp(p,type,4))continue;
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
    Check(p[4]<=63&&U32(p+84)<=1,"Invalid NDIF image name or segment flag");
    Check(!U32(p+116),"Encrypted ShrinkWrap NDIF images are unsupported");
    const uint64_t sectors=U32(p+68),maxChunk=U32(p+72),base=U32(p+76),count=U32(p+124);
    Check(sectors&&sectors<=0xffffff&&base<=data.length&&count<=sectors+1&&
          count*ChunkBytes==map.length-HeaderBytes,"Invalid NDIF block map geometry");
    Layout out{sectors*Sector,U32(p+80),{}};
    // Older read-only NDIF can store a contiguous data fork without a table.
    if(!count){
        Check(!U32(p+84)&&Range(base,out.length,data.length),"Truncated contiguous NDIF disk");
        out.chunks.push_back({0,out.length,data.at+base,out.length,Raw});
        return out;
    }
    Check(count>=2,"Missing NDIF block descriptors");
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
                    Check(type==Raw||type==KenCode||type==Rle||type==Lzh||type==Adc||type==ShrinkWrap,"Unsupported NDIF chunk encoding");
                    Check(packed&&Range(at,packed,data.length-base),"NDIF chunk exceeds its data fork");
                }
                out.chunks.push_back({sector*Sector,0,data.at+base+at,packed,type});
            }
        }
        index+=n;
    }
    for(const auto& chunk:out.chunks){
        if(chunk.type==Raw)Check(chunk.length==chunk.packedLength,"Incorrect NDIF raw chunk length");
        if(chunk.type!=Raw&&chunk.type!=Zero)Check(chunk.length<=maxChunk*Sector&&chunk.length<=MaxCompressedChunk&&
                                chunk.packedLength<=MaxCompressedChunk,"Invalid NDIF compressed chunk size");
    }
    return out;
}



void DecodeRle(const Bytes& input,Bytes& output)
{
    size_t at=0,produced=0;
    while(at<input.size()){
        Check(Range(at,2,input.size()),"Truncated NDIF RLE control word");
        const auto encoded=U16(input.data()+at);
        const bool repeated=(encoded&0x8000)!=0;
        const size_t length=2*(repeated?0x10000u-encoded:encoded);
        at+=2;
        if(!length)continue;
        const auto packed=repeated?2:length;
        Check(Range(at,packed,input.size())&&Range(produced,length,output.size()),
              "NDIF RLE run exceeds its chunk");
        if(repeated){
            for(size_t i=0;i<length;i+=2){
                output[produced+i]=input[at];
                output[produced+i+1]=input[at+1];
            }
        }else memcpy(output.data()+produced,input.data()+at,length);
        at+=packed;
        produced+=length;
    }
    Check(produced==output.size(),"Incomplete NDIF RLE chunk");
}

struct Segment {
    unsigned number,count;
    std::array<uint8_t,16> group;
    uint32_t checksum;
};
std::optional<Segment> SegmentInfo(Source& file,Fork resources)
{
    const auto record=Resource(file,resources,"bcm#");
    if(!record)return {};
    Check(record->length==24,"Invalid NDIF segment descriptor");
    const auto bytes=Disk::Read(file,record->at,24);
    Segment result{U16(bytes.data()),U16(bytes.data()+2),{},U32(bytes.data()+20)};
    memcpy(result.group.data(),bytes.data()+4,result.group.size());
    Check(result.number&&result.number<=result.count,"Invalid NDIF segment number");
    return result;
}
std::wstring Parent(const std::wstring& path)
{
    const auto split=path.find_last_of(L"\\/");
    return split==std::wstring::npos?L"":path.substr(0,split+1);
}

std::vector<Part> MoreParts(const Source& first,const Segment& lead)
{
    const auto files=first.WorkerFiles();
    Check(!files.empty()&&lead.number==1&&lead.count>1,"Open the first segment of the NDIF set");
    const auto directory=Parent(files.front().first);
    WIN32_FIND_DATAW data{};
    const auto handle=FindFirstFileW((directory+L"*").c_str(),&data);
    Check(handle!=INVALID_HANDLE_VALUE,"Cannot enumerate the NDIF segment directory");
    struct Listing {HANDLE handle;~Listing(){FindClose(handle);}} listing{handle};
    std::vector<Part> parts(lead.count);
    do{
        if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        const std::wstring name=data.cFileName;
        const auto extension=name.rfind(L'.');
        if(extension==std::wstring::npos)continue;
        const auto suffix=name.c_str()+extension;
        if(_wcsicmp(suffix,L".bin")&&_wcsicmp(suffix,L".part")&&_wcsicmp(suffix,L".as"))continue;
        const auto path=directory+name;
        if(CompareStringOrdinal(path.c_str(),-1,files.front().first.c_str(),-1,TRUE)==CSTR_EQUAL)continue;
        std::unique_ptr<Source> candidate;
        try{candidate=HfsPlus::OpenImage(path.c_str(),false);}catch(const Error&){continue;}
        auto forks=MacBinary(*candidate);
        if(!forks)forks=AppleContainer(*candidate,0x00051600);
        if(!forks)continue;
        const auto info=SegmentInfo(*candidate,forks->resource);
        if(!info||info->group!=lead.group||info->number==1)continue;
        Check(info->count==lead.count,"Conflicting NDIF segment set counts");
        auto& part=parts[info->number-1];
        Check(!part.dataFile&&forks->data.length,"Duplicate or empty NDIF segment");
        part.dataFile=std::move(candidate);
        part.data=forks->data;
        part.checksum=info->checksum;
    }while(FindNextFileW(handle,&data));
    for(size_t i=1;i<parts.size();++i)
        Check(bool(parts[i].dataFile),("Missing NDIF segment "+std::to_string(i+1)+" of "+std::to_string(parts.size())).c_str());
    return parts;
}


class BinHexForks final : public Source {
    std::unique_ptr<Source> origin_;
    MacBrowser::Reader read_;
    uint64_t data_,resource_;
public:
    BinHexForks(MacBrowser::Reader reader,uint64_t data,uint64_t resource)
        :read_(std::move(reader)),data_(data),resource_(resource){}
    void Pin(std::unique_ptr<Source> origin){origin_=std::move(origin);}
    uint64_t Size()const override{return data_+resource_;}
    bool Writable()const override{return false;}
    bool ProcessLocal()const override{return origin_&&origin_->ProcessLocal();}
    void CheckMedia()override{if(origin_)origin_->CheckMedia();}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{
        return origin_?origin_->WorkerFiles():std::vector<std::pair<std::wstring,uintptr_t>>{};
    }
    void Write(uint64_t,const void*,size_t)override{throw Error("BinHex NDIF images are read-only");}
    void Flush()override{}
    void Read(uint64_t offset,void* bytes,size_t count)override{
        Check(Range(offset,count,Size())&&(!count||bytes),"Read outside BinHex forks");
        auto out=static_cast<uint8_t*>(bytes);
        if(offset<data_&&count){
            const auto take=size_t((std::min)(uint64_t(count),data_-offset));
            read_(0,offset,out,take);
            offset+=take;count-=take;out+=take;
        }
        if(count)read_(1,offset-data_,out,count);
    }
};

class Image final : public Source {
    std::vector<Part> parts_;
    Layout layout_;
    const std::optional<uint64_t> direct_;
    std::mutex mutex_;
    size_t cached_=SIZE_MAX;
    Bytes decoded_;
    void ReadPacked(uint64_t at,void* bytes,size_t count){
        auto out=static_cast<uint8_t*>(bytes);
        while(count){
            const auto next=std::upper_bound(parts_.begin(),parts_.end(),at,
                [](uint64_t value,const Part& part){return value<part.start;});
            const auto& part=*std::prev(next);
            const auto within=at-part.start;
            const auto take=size_t((std::min)(uint64_t(count),part.data.length-within));
            part.dataFile->Read(part.data.at+within,out,take);
            at+=take;out+=take;count-=take;
        }
    }
public:
    Image(std::vector<Part> parts,Layout layout,std::optional<uint64_t> direct={})
        :parts_(std::move(parts)),layout_(std::move(layout)),direct_(direct){}
    bool Editable()const{return direct_.has_value();}
    uint64_t Size()const override{return layout_.length;}
    uint32_t ExpectedChecksum()const{return layout_.checksum;}
    void VerifySegments(Storage::Job& job){
        if(parts_.size()<2)return;
        std::lock_guard<std::mutex> lock(mutex_);
        Bytes buffer(65536);
        for(size_t i=0;i<parts_.size();++i){
            const auto& part=parts_[i];
            if(!part.checksum)continue;
            uint32_t crc=UINT32_MAX;
            for(uint64_t at=0;at<part.data.length;){
                if(job.cancelled())throw Storage::Cancelled();
                const auto n=size_t((std::min)(uint64_t(buffer.size()),part.data.length-at));
                part.dataFile->Read(part.data.at+at,buffer.data(),n);
                crc=Checksum(crc,buffer.data(),n);
                at+=n;
            }
            Check(crc==part.checksum,("NDIF segment "+std::to_string(i+1)+
                 " checksum mismatch. The output was discarded.").c_str());
        }
    }
    bool Writable()const override{return Editable()&&parts_.front().dataFile->Writable();}
    bool ProcessLocal()const override{return parts_.front().dataFile->ProcessLocal();}
    void CheckMedia()override{
        for(const auto& part:parts_){
            part.dataFile->CheckMedia();
            if(part.resourceFile)part.resourceFile->CheckMedia();
        }
    }
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{
        std::vector<std::pair<std::wstring,uintptr_t>> files;
        const auto add=[&](const Source& source){
            const auto paths=source.WorkerFiles();
            files.insert(files.end(),paths.begin(),paths.end());
        };
        for(const auto& part:parts_){
            add(*part.dataFile);
            if(part.resourceFile)add(*part.resourceFile);
        }
        return files;
    }
    void Write(uint64_t offset,const void* bytes,size_t count)override{
        Check(Writable(),"This NDIF image is read-only");
        Check(Range(offset,count,Size())&&(!count||bytes),"Write outside NDIF data fork");
        std::lock_guard<std::mutex> lock(mutex_);
        if(count)parts_.front().dataFile->Write(*direct_+offset,bytes,count);
    }
    void Flush()override{
        if(!Writable())return;
        std::lock_guard<std::mutex> lock(mutex_);
        parts_.front().dataFile->Flush();
    }
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
            else if(chunk->type==Raw)ReadPacked(chunk->packedAt+within,out,take);
            else{
                const auto index=size_t(chunk-layout_.chunks.begin());
                if(index!=cached_){
                    Bytes packed(size_t(chunk->packedLength));
                    ReadPacked(chunk->packedAt,packed.data(),packed.size());
                    Bytes decoded(size_t(chunk->length));
                    if(chunk->type==KenCode)DecodeKenCode(packed,decoded);
                    else if(chunk->type==Rle)DecodeRle(packed,decoded);
                    else if(chunk->type==Lzh)DecodeLzh(packed,decoded);
                    else if(chunk->type==ShrinkWrap)DecodeShrinkWrap(packed,decoded);
                    else Check(Dmg::DecodeAppleDataCompression(packed,decoded),"Corrupt NDIF ADC data");
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



bool IsEditable(const Source& source)
{
    const auto image=dynamic_cast<const Image*>(&source);
    return image&&image->Editable();
}

std::optional<uint32_t> ExpectedChecksum(const Source& source)
{
    const auto image=dynamic_cast<const Image*>(&source);
    if(!image||!image->ExpectedChecksum())return {};
    return image->ExpectedChecksum();
}


void VerifySegmentChecksums(Source& source,Storage::Job& job)
{
    if(auto image=dynamic_cast<Image*>(&source))image->VerifySegments(job);
}

uint32_t Checksum(uint32_t running,const void* bytes,size_t count)
{
    // NDIF shifts right with the normal polynomial and uses no final XOR.
    static constexpr auto table=[]{
        std::array<uint32_t,256> result{};
        for(uint32_t i=0;i<result.size();++i){
            uint32_t value=i;
            for(unsigned bit=0;bit<8;++bit)
                value=(value>>1)^((value&1)?0x04c11db7u:0);
            result[i]=value;
        }
        return result;
    }();
    const auto data=static_cast<const uint8_t*>(bytes);
    for(size_t i=0;i<count;++i)running=(running>>8)^table[(running^data[i])&255];
    return running;
}

bool Unwrap(std::unique_ptr<Source>& input,Storage::Job* job)
{
    auto install=[&](Fork data,Source& metadata,Fork resource,std::unique_ptr<Source> companion={},
                     std::unique_ptr<BinHexForks> decoded={},bool writableType=false){
        const auto map=Resource(metadata,resource,"bcem");
        const auto segment=SegmentInfo(metadata,resource);
        if(!map){
            Check(!segment,"Open the first segment of the NDIF set");
            return false;
        }
        Check(map->length>=HeaderBytes,"Truncated NDIF header");
        const auto segmented=U32(Disk::Read(metadata,map->at+84,4).data());
        Check(segmented<=1,"Invalid NDIF segment flag");
        std::vector<Part> parts(1);
        if(segmented){
            Check(segment.has_value(),"Missing NDIF segment descriptor");
            parts=MoreParts(*input,*segment);
            parts[0].checksum=segment->checksum;
        }
        parts[0].data=data;
        uint64_t total=0;
        for(auto& part:parts){
            part.start=total;
            Check(part.data.length<=UINT64_MAX-total,"NDIF segment sizes overflow");
            total+=part.data.length;
        }
        auto layout=Map(metadata,*map,{0,total});
        std::optional<uint64_t> direct;
        if(writableType&&!segment&&!segmented&&!layout.checksum){
            const auto start=layout.chunks.front().packedAt;
            // Only a one-to-one stored payload can be updated without changing
            // the map, checksums, or the lengths and offsets in the carrier.
            if(Range(start,layout.length,data.length)&&
               std::all_of(layout.chunks.begin(),layout.chunks.end(),[&](const Chunk& chunk){
                   return chunk.type==Raw&&chunk.packedAt==start+chunk.at;
               }))direct=data.at+start;
        }
        if(decoded){
            decoded->Pin(std::move(input));
            parts[0].dataFile=std::move(decoded);
        }else parts[0].dataFile=std::move(input);
        parts[0].resourceFile=std::move(companion);
        input=std::make_unique<Image>(std::move(parts),std::move(layout),direct);
        return true;
    };
    if(auto forks=MacBinary(*input)){
        if(install(forks->data,*input,forks->resource,{},{},forks->writableType))return true;
        if(forks->rawDisk&&!forks->resource.length&&forks->data.length&&forks->data.length%Sector==0){
            Layout layout{forks->data.length,0,{{0,forks->data.length,0,forks->data.length,Raw}}};
            std::vector<Part> parts;
            parts.push_back({std::move(input),{},forks->data});
            const auto direct=forks->writableType?std::optional<uint64_t>(forks->data.at):std::nullopt;
            input=std::make_unique<Image>(std::move(parts),std::move(layout),direct);
            return true;
        }
    }
    if(auto forks=AppleContainer(*input,0x00051600)){
        if(install(forks->data,*input,forks->resource))return true;
    }
    MacBrowser::Entry transport;
    if(auto reader=TransferConversion::ReadBinHex(*input,transport,job)){
        const auto length=transport.forks[0].size,resource=transport.forks[1].size;
        auto decoded=std::make_unique<BinHexForks>(std::move(reader),length,resource);
        auto& source=*decoded;
        if(install({0,length},source,{length,resource},{},std::move(decoded)))return true;
    }
    const auto files=input->WorkerFiles();
    if(files.empty())return false;
    const auto& path=files.front().first;
    if(auto stream=OptionalFile(path+L":AFP_Resource")){
        const auto size=stream->Size();
        auto& resource=*stream;
        if(install({0,input->Size()},resource,{0,size},std::move(stream)))return true;
    }
    const auto split=path.find_last_of(L"\\/");
    const auto fileName=split==std::wstring::npos?0:split+1;
    if(auto sidecar=OptionalFile(path.substr(0,fileName)+L"._"+path.substr(fileName))){
        if(const auto forks=AppleContainer(*sidecar,0x00051607)){
            auto& resource=*sidecar;
            return install({0,input->Size()},resource,forks->resource,std::move(sidecar));
        }
    }
    return false;
}
}
