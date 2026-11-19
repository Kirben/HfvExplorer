// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DmgImage.h"
#include "DmgEncryption.h"
#include "HfsPlusDisk.h"
#include "CommandStorage.h"
#include <wincrypt.h>
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

void WriteDmg(Source& input,NewImage& output,Storage::Job& job,Storage::Progress& progress)
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

void WriteRaw(Source& input,NewImage& output,Storage::Job& job,Storage::Progress& progress)
{
    Buffer storage(input.ProcessLocal());
    auto& buffer=storage.bytes;
    output.Resize(input.Size());
    for(uint64_t at=0;at<input.Size();){
        if(job.cancelled())throw Storage::Cancelled();
        const auto bytes=size_t((std::min)(uint64_t(buffer.size()),input.Size()-at));
        input.Read(at,buffer.data(),bytes);
        if(std::any_of(buffer.begin(),buffer.begin()+bytes,[](auto b){return b!=0;}))
            output.Write(at,buffer.data(),bytes);
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
        Check(format!=Output::RawImage||dmg||encrypted,"The input does not contain a DMG disk image");
        auto output=HfsPlus::CreateStagedOutput(destination,512);
        output->Resize(0);
        Storage::Progress progress;
        progress.phase=format==Output::RawImage?"Extracting raw disk image":"Creating compressed DMG";
        progress.totalBytes=input->Size();
        progress.scanning=false;
        progress.byteTransfer=true;
        if(format==Output::RawImage)WriteRaw(*input,*output,current,progress);
        else WriteDmg(*input,*output,current,progress);
        if(current.cancelled())throw Storage::Cancelled();
        output->PublishTo(destination);
        output->Keep();
        return Storage::Result{};
    },{changed})(job);
}
}
