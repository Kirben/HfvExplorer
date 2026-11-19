// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "DmgImage.h"
#include "HfsPlusDisk.h"
#include "CopyProcess.h"
#include "VolumeCoordinator.h"
#include <wincrypt.h>
#define BZ_EXPORT
#include "compression/bzip2/bzlib.h"
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"
#include "compression/lzfse/lzfse.h"

namespace {
using namespace HfsPlus;
using namespace HfsPlus::Disk;
constexpr uint32_t Raw=1,Zero=0,Adc=0x80000004,Zlib=0x80000005,Bzip=0x80000006,Lzfse=0x80000007,Xz=0x80000008;
constexpr size_t SectorBytes=512;

std::string Base64(const Bytes& source)
{
    DWORD size=0;
    Check(CryptBinaryToStringA(source.data(),DWORD(source.size()),CRYPT_STRING_BASE64,nullptr,&size)!=FALSE,
          "Size fixture base64");
    std::string out(size,0);
    Check(CryptBinaryToStringA(source.data(),DWORD(source.size()),CRYPT_STRING_BASE64,out.data(),&size)!=FALSE,
          "Encode fixture base64");
    out.resize(size);
    return out;
}
Bytes ZlibBlock(const Bytes& plain)
{
    Bytes packed(plain.size()+1024,0);
    z_stream stream{};
    stream.zalloc=[](void*,uInt n,uInt size)->void*{return calloc(n,size);};
    stream.zfree=[](void*,void* ptr){free(ptr);};
    Check(deflateInit(&stream,6)==Z_OK,"Initialize fixture zlib compression");
    stream.next_in=const_cast<uint8_t*>(plain.data());stream.avail_in=uInt(plain.size());
    stream.next_out=packed.data();stream.avail_out=uInt(packed.size());
    const auto status=deflate(&stream,Z_FINISH);
    const auto bytes=stream.total_out;
    deflateEnd(&stream);
    Check(status==Z_STREAM_END,"Build fixture zlib block");
    packed.resize(bytes);
    return packed;
}
Bytes BzipBlock(Bytes plain)
{
    Bytes packed(plain.size()+1024,0);
    auto size=unsigned(packed.size());
    Check(BZ2_bzBuffToBuffCompress(reinterpret_cast<char*>(packed.data()),&size,
          reinterpret_cast<char*>(plain.data()),unsigned(plain.size()),9,0,0)==BZ_OK,
          "Build fixture bzip2 block");
    packed.resize(size);
    return packed;
}
Bytes LzfseBlock(const Bytes& plain)
{
    Bytes packed(plain.size()+1024,0);
    const auto size=lzfse_encode_buffer(packed.data(),packed.size(),plain.data(),plain.size(),nullptr);
    Check(size>0,"Build fixture LZFSE block");
    packed.resize(size);
    return packed;
}
Bytes XzBlock()
{
    // Generated with Python's liblzma binding, 512 copies of ASCII X.
    const std::string hex="fd377a585a000004e6d6b4460200210116000000742fe5a3e001ff00085d002c6ffbbeb25bb00000c6731b3d3a6194a9000124800400000038b4ec61b1c467fb020000000004595a";
    Bytes out;
    for(size_t i=0;i<hex.size();i+=2)out.push_back(uint8_t(std::stoul(hex.substr(i,2),nullptr,16)));
    return out;
}
struct Run {uint32_t type;uint64_t start,sectors;Bytes packed;};
using Map=std::vector<Run>;
Bytes Pack(const std::vector<Map>& maps,uint64_t sectors)
{
    constexpr size_t dataOffset=64;
    Bytes file(dataOffset,0);
    std::string xml="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>resource-fork</key><dict><key>blkx</key><array>";
    for(const auto& runs:maps){
        const auto first=runs.front().start;
        const auto last=runs.back().start+runs.back().sectors;
        const auto dataBase=file.size()-dataOffset;
        Bytes mish(204+40*(runs.size()+1),0);
        memcpy(mish.data(),"mish",4);
        P32(mish.data()+4,1);P64(mish.data()+8,first);P64(mish.data()+16,last-first);
        P64(mish.data()+24,dataBase);P32(mish.data()+200,uint32_t(runs.size()+1));
        for(size_t i=0;i<runs.size();++i){
            const auto& run=runs[i];
            auto entry=mish.data()+204+i*40;
            P32(entry,run.type);P64(entry+8,run.start-first);P64(entry+16,run.sectors);
            P64(entry+24,file.size()-dataOffset-dataBase);P64(entry+32,run.packed.size());
            file.insert(file.end(),run.packed.begin(),run.packed.end());
        }
        P32(mish.data()+204+runs.size()*40,UINT32_MAX);
        xml+="<dict><key>Name</key><string>Fixture</string><key>Data</key><data>\n"+
             Base64(mish)+"</data></dict>";
    }
    xml+="</array></dict></dict></plist>";
    const auto xmlAt=file.size();
    file.insert(file.end(),xml.begin(),xml.end());
    Bytes footer(512);
    memcpy(footer.data(),"koly",4);P32(footer.data()+4,4);P32(footer.data()+8,512);
    P64(footer.data()+24,dataOffset);P64(footer.data()+32,xmlAt-dataOffset);
    P32(footer.data()+56,1);P32(footer.data()+60,1);
    P64(footer.data()+216,xmlAt);P64(footer.data()+224,xml.size());P64(footer.data()+492,sectors);
    file.insert(file.end(),footer.begin(),footer.end());
    return file;
}
std::wstring Save(const std::string& path,const Bytes& file)
{
    auto name=VolumeCoord::Wide(path);
    auto disk=CreateFormatOutput(name.c_str(),file.size());
    disk->Write(0,file.data(),file.size());disk->Flush();disk->Keep();
    return name;
}
std::unique_ptr<Source> Open(const std::wstring& path)
{
    auto disk=HfsPlus::OpenImage(path.c_str(),false);
    Check(Dmg::Unwrap(disk)&&!disk->Writable(),"DMG source did not open read-only");
    return disk;
}
template<class F>void Reject(F f,const char* error)
{
    try{f();}catch(const HfsPlus::Error&){return;}
    throw HfsPlus::Error(error);
}
}

int RunDmgImageTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const std::string root=directory;
    FILE* report=fopen((root+"\\dmg-image-report.txt").c_str(),"w");
    if(!report)return 2;
    int status=0;
    try{
        // ADC literal, short back-reference and long repeating back-references.
        Bytes adc{0x80,'A',0,0};
        for(unsigned i=0;i<7;++i){adc.push_back(0x7f);adc.push_back(0);adc.push_back(0);}
        adc.insert(adc.end(),{uint8_t(0x40|(512-4-7*67-4)),0,0});
        const Map first{{Raw,0,1,Bytes(SectorBytes,'R')},{Zero,1,1,{}},{Adc,2,1,adc}};
        const Map second{{Zlib,3,1,ZlibBlock(Bytes(SectorBytes,'Z'))},
                         {Bzip,4,1,BzipBlock(Bytes(SectorBytes,'B'))},
                         {Lzfse,5,1,LzfseBlock(Bytes(SectorBytes,'L'))},{Xz,6,1,XzBlock()}};
        auto disk=Open(Save(root+"\\codecs.dmg",Pack({first,second},7)));
        Bytes decoded(7*SectorBytes);
        disk->Read(0,decoded.data(),decoded.size());
        for(size_t i=0;i<7;++i)
            Check(std::all_of(decoded.begin()+i*SectorBytes,decoded.begin()+(i+1)*SectorBytes,
                  [&](uint8_t ch){return ch==std::array<uint8_t,7>{'R',0,'A','Z','B','L','X'}[i];}),
                  "DMG compression method returned wrong data");
        Bytes tail(6);
        disk->Read(2*SectorBytes-3,tail.data(),tail.size());
        Check(tail==Bytes({0,0,0,'A','A','A'}),"Read across DMG chunk boundary");
        Reject([&]{disk->Write(0,tail.data(),tail.size());},"DMG source accepted a write");
        Reject([&]{disk->Read(disk->Size(),tail.data(),tail.size());},"DMG source accepted an out-of-range read");
        auto overlap=Save(root+"\\overlap.dmg",Pack({first,first},3));
        Reject([&]{Open(overlap);},"Overlapping DMG runs accepted");
        auto corrupt=Save(root+"\\truncated.dmg",Pack({{{Zlib,0,1,{0x78,0x9c}}}},1));
        Reject([&]{auto bad=Open(corrupt);bad->Read(0,tail.data(),tail.size());},"Truncated compressed run was accepted");
        auto underfilled=Save(root+"\\short-output.dmg",Pack({{{Xz,0,2,XzBlock()}}},2));
        Reject([&]{auto bad=Open(underfilled);bad->Read(0,tail.data(),tail.size());},"Short decoder output was accepted");
        fprintf(report,"PASS: UDIF XML, raw, zero, ADC, zlib, bzip2, LZFSE and XZ runs, multiple relative block maps, reads across boundaries and rejection of malformed input.\n");
        fflush(report);

        const auto rawPath=VolumeCoord::Wide(root+"\\volume.img");
        const Bytes data={'H','F','S','+',' ','i','n',' ','D','M','G'};
        Bytes raw;
        {
            auto image=CreateImage(rawPath.c_str(),8*1024*1024);
            Volume::Format(*image,u"DMG fixture",true);
            Volume volume(*image,true);
            volume.Put(2,u"Fixture",data.size(),1,[&](unsigned fork,uint64_t at,void* p,size_t count){
                if(fork)memset(p,0x5a,count);else memcpy(p,data.data()+at,count);
            });
            raw=Disk::Read(*image,0,size_t(image->Size()));
        }
        const size_t boundary=64*1024;
        Bytes head(raw.begin(),raw.begin()+boundary);
        Bytes body(raw.begin()+boundary,raw.end());
        const auto path=Save(root+"\\browser.dmg",Pack({{{Zlib,0,head.size()/SectorBytes,ZlibBlock(head)},
                                              {Raw,head.size()/SectorBytes,body.size()/SectorBytes,std::move(body)}}},raw.size()/SectorBytes));
        auto volume=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry entry;
        Check(volume&&!volume->Writable()&&volume->Name()==u"DMG fixture"&&volume->FindChild(2,u"Fixture",entry),
              "Open compressed DMG through normal browser image detection");
        const auto host=VolumeCoord::Wide(root+"\\export");
        Check(CreateDirectoryW(host.c_str(),nullptr)!=FALSE,"Create DMG export folder");
        FileOperations::Request request=FileOperations::OrdinaryCopy{
            {FileOperations::Location::Mounted(volume,entry.id)},FileOperations::Location::Windows(host)};
        request.background=true;
        CopyProcess::enabled=true;
        auto operation=FileOperations::Submit(std::move(request));
        const auto start=GetTickCount();
        while(!operation.Job()->done()){
            Check(GetTickCount()-start<15000,"DMG worker copy timed out");
            Sleep(5);
        }
        const auto result=operation.Job()->result();
        Check(!result.code,result.message.c_str());
        Check(CopyProcess::lastProcess!=0,"DMG export did not use the copy subprocess");
        auto target=MacBrowser::OpenWindows(host);
        Check(target->FindChild(2,u"Fixture",entry)&&entry.forks[0].size==data.size()&&entry.forks[1].size==1,
              "DMG copy worker lost a fork");
        Bytes exported(data.size());
        target->Read(entry.id,0,0,exported.data(),exported.size());
        Check(exported==data,"DMG copy worker changed file contents");
        fprintf(report,"PASS: HFS+ in a read-only compressed DMG through the normal browser and both-fork copy subprocess.\n");
    }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());status=1;}
    CopyProcess::enabled=false;
    fclose(report);
    return status;
}
#endif
