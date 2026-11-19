// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DmgImage.h"
#include "DmgEncryption.h"
#include "NdifImage.h"
#include "HfsPlusDisk.h"
#include "CommandStorage.h"
#include <wincrypt.h>
extern "C" {
#include "hfs/macbinary.h"
}
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"

namespace Dmg {
namespace {
using namespace HfsPlus;
using namespace HfsPlus::Disk;
constexpr uint32_t Zero=0, Raw=1, Deflate=0x80000005, End=UINT32_MAX;
constexpr size_t Chunk=1024*1024, Sector=512, MapHeader=204, RunBytes=40;
constexpr size_t MaxMap=16*1024*1024;


class ConversionInput {
    Source& source_;
    const std::optional<uint32_t> expected_;
    uint32_t checksum_=UINT32_MAX;
public:
    explicit ConversionInput(Source& source):source_(source),expected_(Ndif::ExpectedChecksum(source)){}
    uint64_t Size()const{return source_.Size();}
    bool ProcessLocal()const{return source_.ProcessLocal();}
    void Read(uint64_t at,void* out,size_t count){
        source_.Read(at,out,count);
        if(expected_)checksum_=Ndif::Checksum(checksum_,out,count);
    }
    void Verify()const{
        Check(!expected_||checksum_==*expected_,"NDIF image checksum mismatch. The output was discarded.");
    }
};

struct Buffer {
    Bytes bytes=Bytes(Chunk);
    const bool sensitive;
    explicit Buffer(bool value):sensitive(value){}
    ~Buffer(){if(sensitive)SecureZeroMemory(bytes.data(),bytes.size());}
};

struct Run {
    uint32_t type;
    uint64_t start,bytes,offset,packed;
};

std::string Base64(const Bytes& map)
{
    DWORD size=0;
    constexpr DWORD format=CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF;
    Check(CryptBinaryToStringA(map.data(),DWORD(map.size()),format,nullptr,&size),
          "Cannot size the DMG block-map plist");
    std::string result(size,0);
    Check(CryptBinaryToStringA(map.data(),DWORD(map.size()),format,result.data(),&size),
          "Cannot encode the DMG block-map plist");
    result.resize(size);
    return result;
}

size_t Compress(Bytes& input,size_t used,Bytes& output)
{
    z_stream stream{};
    stream.zalloc=[](void*,uInt n,uInt size)->void*{return calloc(n,size);};
    stream.zfree=[](void*,void* p){free(p);};
    Check(deflateInit(&stream,6)==Z_OK,"Cannot initialize DMG compression");
    stream.next_in=input.data();stream.avail_in=uInt(used);
    stream.next_out=output.data();stream.avail_out=uInt(used);
    const auto status=deflate(&stream,Z_FINISH);
    const auto written=size_t(stream.total_out);
    deflateEnd(&stream);
    Check(status==Z_STREAM_END||status==Z_OK||status==Z_BUF_ERROR,"DMG compression failed");
    return status==Z_STREAM_END&&written<used?written:0;
}

void CrcField(uint8_t* field,uint32_t value)
{
    P32(field,2);  // UDIF CRC-32
    P32(field+4,32);
    P32(field+8,value);
}

void Append(NewImage& output,const void* bytes,size_t size)
{
    const auto at=output.Size();
    Check(Range(at,size,INT64_MAX),"DMG output is too large");
    output.Resize(at+size);
    output.Write(at,bytes,size);
}

void WriteDmg(ConversionInput& input,NewImage& output,Storage::Job& job,Storage::Progress& progress)
{
    const auto length=input.Size();
    Check(length&&length%Sector==0,"DMG creation requires a raw image made of complete 512-byte sectors");
    Buffer plainBuffer(input.ProcessLocal()),packedBuffer(input.ProcessLocal());
    auto& plain=plainBuffer.bytes;
    auto& packed=packedBuffer.bytes;
    std::vector<Run> runs;
    uint32_t dataCrc=0,logicalCrc=0;
    for(uint64_t at=0;at<length;){
        if(job.cancelled())throw Storage::Cancelled();
        const auto count=size_t((std::min)(uint64_t(Chunk),length-at));
        input.Read(at,plain.data(),count);
        logicalCrc=uint32_t(crc32(logicalCrc,plain.data(),uInt(count)));
        const bool zero=std::all_of(plain.begin(),plain.begin()+count,[](auto b){return !b;});
        const auto compressed=zero?0:Compress(plain,count,packed);
        const auto type=zero?Zero:compressed?Deflate:Raw;
        const auto stored=zero?0:compressed?compressed:count;
        const auto offset=output.Size();
        if(!runs.empty()&&type!=Deflate&&runs.back().type==type){
            runs.back().bytes+=count;
            runs.back().packed+=stored;
        }else{
            Check(runs.size()<(MaxMap-MapHeader)/RunBytes-1,"DMG output requires too many compression runs");
            runs.push_back({type,at,count,offset,stored});
        }
        if(stored){
            const auto bytes=compressed?packed.data():plain.data();
            Append(output,bytes,stored);
            dataCrc=uint32_t(crc32(dataCrc,bytes,uInt(stored)));
        }
        at+=count;
        progress.bytes=progress.processed=at;
        job.progress(progress);
    }

    Bytes map(MapHeader+(runs.size()+1)*RunBytes,0);
    memcpy(map.data(),"mish",4);P32(map.data()+4,1);
    P64(map.data()+16,length/Sector);
    P32(map.data()+32,Chunk/Sector);
    P32(map.data()+36,UINT32_MAX);
    CrcField(map.data()+64,logicalCrc);
    P32(map.data()+200,uint32_t(runs.size()+1));
    for(size_t i=0;i<runs.size();++i){
        const auto& r=runs[i];
        const auto p=map.data()+MapHeader+i*RunBytes;
        P32(p,r.type);P64(p+8,r.start/Sector);P64(p+16,r.bytes/Sector);
        P64(p+24,r.offset);P64(p+32,r.packed);
    }
    auto end=map.data()+MapHeader+runs.size()*RunBytes;
    P32(end,End);P64(end+8,length/Sector);P64(end+24,output.Size());
    const std::string xml="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>resource-fork</key><dict><key>blkx</key><array><dict>"
        "<key>Attributes</key><string>0x0050</string><key>ID</key><integer>0</integer>"
        "<key>Name</key><string>Whole disk</string><key>Data</key><data>"+
        Base64(map)+"</data></dict></array></dict></dict></plist>";
    const auto dataBytes=output.Size();
    Append(output,xml.data(),xml.size());
    Bytes footer(512);
    memcpy(footer.data(),"koly",4);P32(footer.data()+4,4);P32(footer.data()+8,512);P32(footer.data()+12,1);
    P64(footer.data()+32,dataBytes);
    P32(footer.data()+56,1);P32(footer.data()+60,1);
    GUID id{};
    Check(SUCCEEDED(CoCreateGuid(&id)),"Cannot create DMG segment identifier");
    memcpy(footer.data()+64,&id,sizeof(id));
    CrcField(footer.data()+80,dataCrc);
    P64(footer.data()+216,dataBytes);P64(footer.data()+224,xml.size());
    CrcField(footer.data()+352,uint32_t(crc32(0,map.data()+72,4)));
    P32(footer.data()+488,1);P64(footer.data()+492,length/Sector);
    Append(output,footer.data(),footer.size());
}


Bytes EncodeAdc(const Bytes& input,size_t size)
{
    std::vector<size_t> recent(65536,SIZE_MAX);
    Bytes output;
    size_t literalAt=0;
    const auto literals=[&](size_t end){
        while(literalAt<end){
            const auto count=(std::min)(size_t(128),end-literalAt);
            output.push_back(uint8_t(0x80|(count-1)));
            output.insert(output.end(),input.begin()+literalAt,input.begin()+literalAt+count);
            literalAt+=count;
        }
    };
    for(size_t at=0;at+3<=size;){
        const auto hash=(uint32_t(input[at])*317+uint32_t(input[at+1])*149+input[at+2])&0xffff;
        const auto previous=recent[hash];
        recent[hash]=at;
        if(previous==SIZE_MAX||at-previous>65536){++at;continue;}
        const auto distance=at-previous-1;
        const auto limit=(std::min)(size_t(67),size-at);
        size_t length=0;
        while(length<limit&&input[previous+length]==input[at+length])++length;
        if(length<3||(length==3&&distance>=1024)){++at;continue;}
        literals(at);
        if(distance<1024&&length<=18){
            output.push_back(uint8_t(((length-3)<<2)|(distance>>8)));
        }else{
            output.push_back(uint8_t(0x40|(length-4)));
            output.push_back(uint8_t(distance>>8));
        }
        output.push_back(uint8_t(distance));
        at+=length;
        literalAt=at;
        if(output.size()>=size)return {};
    }
    literals(size);
    return output.size()<size?std::move(output):Bytes{};
}

Bytes NdifResource(const std::vector<Run>& runs,uint32_t sectors,uint32_t maxChunk,uint32_t packed,uint32_t checksum)
{
    constexpr uint32_t data=256,header=128,mapBytes=50;
    const auto length=uint32_t(header+12*(runs.size()+1));
    const auto mapAt=data+4+length;
    Bytes out(mapAt+mapBytes);
    auto p=out.data();
    P32(p,data);P32(p+4,mapAt);P32(p+8,length+4);P32(p+12,mapBytes);
    P32(p+data,length);
    auto bcem=p+data+4;
    P16(bcem,11);bcem[4]=10;memcpy(bcem+5,"Disk Image",10);
    P32(bcem+68,sectors);P32(bcem+72,maxChunk);P32(bcem+80,checksum);
    P32(bcem+124,uint32_t(runs.size()+1));
    for(size_t i=0;i<runs.size();++i){
        const auto& run=runs[i];
        const auto row=bcem+header+i*12;
        P32(row,(uint32_t(run.start/512)<<8)|run.type);
        P32(row+4,uint32_t(run.offset));P32(row+8,uint32_t(run.packed));
    }
    const auto tail=bcem+header+runs.size()*12;
    P32(tail,(sectors<<8)|0xff);P32(tail+4,packed);
    const auto map=p+mapAt;
    memcpy(map,p,16);P16(map+24,28);P16(map+26,50);
    memcpy(map+30,"bcem",4);P16(map+36,10);
    P16(map+38,128);P16(map+40,0xffff);
    return out;
}

constexpr uint64_t macBinaryHeader=128;

void FinishNdif(NewImage& output,uint32_t dataBytes,const Bytes& resource,const char* type,bool locked)
{
    const auto aligned=[](uint64_t n){return (n+127)&~uint64_t(127);};
    output.Resize(macBinaryHeader+aligned(dataBytes));
    Append(output,resource.data(),resource.size());
    output.Resize(aligned(output.Size()));
    Bytes header(size_t(macBinaryHeader),0);
    header[1]=10;memcpy(header.data()+2,"Disk Image",10);
    memcpy(header.data()+65,type,8);
    header[81]=locked?1:0;
    P32(header.data()+83,dataBytes);P32(header.data()+87,uint32_t(resource.size()));
    macbinary_finish_header(header.data(),MacBinaryII);
    output.Write(0,header.data(),header.size());
}

void WriteNdif(ConversionInput& input,NewImage& output,Storage::Job& job,Storage::Progress& progress,bool compress)
{
    constexpr size_t unit=512*Sector;
    const auto length=input.Size();
    output.Resize(macBinaryHeader);
    Buffer storage(input.ProcessLocal());
    auto& plain=storage.bytes;
    std::vector<Run> runs;
    uint32_t checksum=UINT32_MAX;
    for(uint64_t at=0;at<length;){
        if(job.cancelled())throw Storage::Cancelled();
        const auto count=size_t((std::min)(uint64_t(unit),length-at));
        input.Read(at,plain.data(),count);
        checksum=Ndif::Checksum(checksum,plain.data(),count);
        const auto zero=std::all_of(plain.begin(),plain.begin()+count,[](uint8_t byte){return byte==0;});
        auto packed=compress&&!zero?EncodeAdc(plain,count):Bytes{};
        const auto type=zero?0u:packed.empty()?2u:0x83u;
        const auto stored=zero?0:packed.empty()?count:packed.size();
        const auto offset=zero?0:output.Size()-macBinaryHeader;
        if(stored)Append(output,packed.empty()?plain.data():packed.data(),stored);
        if(input.ProcessLocal()&&!packed.empty())SecureZeroMemory(packed.data(),packed.size());
        runs.push_back({type,at,count,offset,stored});
        at+=count;
        progress.bytes=progress.processed=at;
        job.progress(progress);
    }
    const auto dataBytes=uint32_t(output.Size()-macBinaryHeader);
    const auto resource=NdifResource(runs,uint32_t(length/Sector),unit/Sector+1,dataBytes,checksum);
    FinishNdif(output,dataBytes,resource,compress?"hdcmddsk":"rohdddsk",true);
}

void WriteRaw(ConversionInput& input,NewImage& output,Storage::Job& job,Storage::Progress& progress,
              uint64_t start=0)
{
    Buffer storage(input.ProcessLocal());
    auto& buffer=storage.bytes;
    output.Resize(start+input.Size());
    for(uint64_t at=0;at<input.Size();){
        if(job.cancelled())throw Storage::Cancelled();
        const auto bytes=size_t((std::min)(uint64_t(buffer.size()),input.Size()-at));
        input.Read(at,buffer.data(),bytes);
        if(std::any_of(buffer.begin(),buffer.begin()+bytes,[](auto b){return b!=0;}))
            output.Write(start+at,buffer.data(),bytes);
        at+=bytes;
        progress.bytes=progress.processed=at;
        job.progress(progress);
    }
}
}

Storage::Result Convert(const std::wstring& source,const std::wstring& destination,
                        Output format,Storage::Job& job,const Password* password)
{
    const auto inputKey=VolumeCoord::ImageKey(source);
    auto access=CommandStorage::ImageWrite(destination);
    if(access.front().key==inputKey)
        return {1,ERROR_INVALID_PARAMETER,"Choose a different destination for the converted image."};
    const auto changed=access.back().key;
    access.push_back({inputKey,VolumeCoord::Mode::Read});
    return CommandStorage::Guard(std::move(access),[&](Storage::Job& current){
        auto input=HfsPlus::OpenImage(source.c_str(),false);
        const bool encrypted=Unlock(input,password);
        const bool dmg=Unwrap(input);
        const bool ndif=!dmg&&!encrypted&&Ndif::Unwrap(input,&current);
        Check(format!=Output::RawImage||dmg||encrypted||ndif,"The input does not contain a DMG or NDIF disk image");
        auto output=HfsPlus::CreateStagedOutput(destination,512);
        output->Resize(0);
        Storage::Progress progress;
        const bool editable=format==Output::EditableNdif;
        const bool outputNdif=editable||format==Output::StoredNdif||format==Output::CompressedNdif;
        if(outputNdif)Check(input->Size()&&!(input->Size()%Sector)&&input->Size()<0x80000000,
                            "NDIF creation requires complete 512-byte sectors and a disk under 2 GiB");
        progress.phase=format==Output::RawImage?"Extracting raw disk image":
                       outputNdif?"Creating NDIF disk image":"Creating compressed DMG";
        progress.totalBytes=input->Size();
        progress.scanning=false;
        progress.byteTransfer=true;
        ConversionInput stream(*input);
        if(format==Output::RawImage)WriteRaw(stream,*output,current,progress);
        else if(editable){
            WriteRaw(stream,*output,current,progress,macBinaryHeader);
            const auto bytes=uint32_t(stream.Size());
            const auto sectors=bytes/uint32_t(Sector);
            const std::vector<Run> runs{{2,0,bytes,0,bytes}};
            FinishNdif(*output,bytes,NdifResource(runs,sectors,sectors,bytes,0),"dimgddsk",false);
        }else if(outputNdif)WriteNdif(stream,*output,current,progress,format==Output::CompressedNdif);
        else WriteDmg(stream,*output,current,progress);
        stream.Verify();
        if(ndif)Ndif::VerifySegmentChecksums(*input,current);
        if(current.cancelled())throw Storage::Cancelled();
        output->PublishTo(destination);
        output->Keep();
        return Storage::Result{};
    },{changed})(job);
}
}
