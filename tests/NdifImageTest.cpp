// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "NdifImage.h"
#include "ReadOnlyCdBackend.h"
#include "HfsPlusDisk.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include <cstdio>
extern "C" {
#include "hfs/macbinary.h"
}
namespace {
using namespace HfsPlus;
using namespace HfsPlus::Disk;

class Memory final : public Source {
public:
    Bytes bytes;
    explicit Memory(Bytes b):bytes(std::move(b)){}
    uint64_t Size()const override{return bytes.size();}
    bool Writable()const override{return false;}
    void Flush()override{}
    void Write(uint64_t,const void*,size_t)override{throw Error("Unexpected fixture write");}
    void Read(uint64_t at,void* out,size_t n)override{
        Check(Range(at,n,bytes.size()),"Read outside test input");
        if(n)memcpy(out,bytes.data()+at,n);
    }
};

Bytes Resource(const Bytes& bcem)
{
    const size_t data=256,map=data+4+bcem.size();
    Bytes out(map+50);
    auto p=out.data();
    P32(p,uint32_t(data));P32(p+4,uint32_t(map));P32(p+8,uint32_t(bcem.size()+4));P32(p+12,50);
    P32(p+data,uint32_t(bcem.size()));memcpy(p+data+4,bcem.data(),bcem.size());
    auto m=p+map;
    memcpy(m,p,16);P16(m+24,28);P16(m+26,50);
    memcpy(m+30,"bcem",4);P16(m+36,10);
    P16(m+38,128);P16(m+40,0xffff);
    return out;
}

Bytes Pack(const Bytes& data,const Bytes& resources,int version=MacBinaryIII)
{
    const size_t forkAt=128+((data.size()+127)&~size_t(127));
    Bytes packed(forkAt+resources.size());
    auto p=packed.data();
    p[1]=4;memcpy(p+2,"NDIF",4);memcpy(p+65,"dimgddsk",8);
    P32(p+83,uint32_t(data.size()));P32(p+87,uint32_t(resources.size()));
    macbinary_finish_header(p,version);
    memcpy(p+128,data.data(),data.size());
    memcpy(p+forkAt,resources.data(),resources.size());
    return packed;
}

Bytes Map(uint32_t sectors,unsigned count)
{
    Bytes b(128+count*12);
    P16(b.data(),11);b[4]=4;memcpy(b.data()+5,"NDIF",4);
    P32(b.data()+68,sectors);P32(b.data()+72,2);P32(b.data()+124,count);
    return b;
}

void Row(Bytes& map,unsigned i,uint32_t start,uint8_t type,uint32_t at,uint32_t length)
{
    auto row=map.data()+128+i*12;
    P32(row,(start<<8)|type);P32(row+4,at);P32(row+8,length);
}

template<class F>void Reject(F fn,const char* why)
{
    try{fn();}catch(const Error&){return;}
    throw Error(why);
}

std::wstring Save(const std::string& path,const Bytes& bytes)
{
    auto filename=VolumeCoord::Wide(path);
    auto file=CreateFormatOutput(filename.c_str(),bytes.size());
    file->Write(0,bytes.data(),bytes.size());file->Flush();file->Keep();
    return filename;
}
}
int RunNdifImageTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const auto root=std::string(directory)+"\\";
    auto report=fopen((root+"ndif-image-report.txt").c_str(),"w");
    if(!report)return 2;
    int status=0;
    readonly_cd_backend_init();
    try{
        Bytes data(5+512,'A');
        // ADC literal followed by overlapping backreferences, one sector.
        data.insert(data.end(),{0x80,'B'});
        for(unsigned i=0;i<7;++i)data.insert(data.end(),{0x7f,0,0});
        data.insert(data.end(),{0x66,0,0});
        auto bcem=Map(3,4);
        P32(bcem.data()+76,5);
        Row(bcem,0,0,2,0,512);Row(bcem,1,1,0,0,0);
        Row(bcem,2,2,0x83,512,uint32_t(data.size()-517));Row(bcem,3,3,0xff,0,0);
        for(int version:{MacBinaryI,MacBinaryII,MacBinaryIII}){
            std::unique_ptr<Source> source=std::make_unique<Memory>(Pack(data,Resource(bcem),version));
            Check(Ndif::Unwrap(source)&&source->Size()==1536&&!source->Writable(),"NDIF MacBinary did not open");
            auto result=Disk::Read(*source,500,1036);
            Check(result[0]=='A'&&result[11]=='A'&&result[12]==0&&result[523]==0&&result[524]=='B'&&
                  result.back()=='B',"NDIF cross-chunk read differed");
            Reject([&]{source->Write(0,data.data(),1);},"NDIF allowed a write");
        }
        auto invalid=bcem;
        Row(invalid,2,1,0x83,512,24);
        std::unique_ptr<Source> input=std::make_unique<Memory>(Pack(data,Resource(invalid)));
        Reject([&]{Ndif::Unwrap(input);},"NDIF accepted duplicate chunk position");
        invalid=bcem;Row(invalid,0,0,2,0,511);
        input=std::make_unique<Memory>(Pack(data,Resource(invalid)));
        Reject([&]{Ndif::Unwrap(input);},"NDIF accepted a short direct block");
        data.back()=1;
        invalid=bcem;Row(invalid,2,2,0x83,512,3);
        input=std::make_unique<Memory>(Pack(data,Resource(invalid)));
        Check(Ndif::Unwrap(input),"NDIF ADC test did not mount");
        Reject([&]{Disk::Read(*input,1024,1);},"NDIF decoded a truncated ADC stream");

        const auto rawPath=VolumeCoord::Wide(root+"volume.raw");
        auto raw=CreateFormatOutput(rawPath.c_str(),8*1024*1024);
        HfsPlus::Volume::Format(*raw,u"NDIF Fixture",false);
        const auto volumeBytes=Disk::Read(*raw,0,size_t(raw->Size()));
        const auto sectors=uint32_t(raw->Size()/512);
        auto rawMap=Map(sectors,2);
        Row(rawMap,0,0,2,0,uint32_t(volumeBytes.size()));Row(rawMap,1,sectors,0xff,0,0);
        auto path=Save(root+"volume.img.bin",Pack(volumeBytes,Resource(rawMap)));
        auto mounted=MacBrowser::OpenBrowserImage(path);
        Check(mounted&&mounted->Name()==u"NDIF Fixture"&&!mounted->Writable(),"NDIF did not mount in the browser");

        if(const auto authentic=_wgetenv(L"HFV_NDIF_TEST_IMAGE")){
            auto apple=MacBrowser::OpenBrowserImage(authentic);
            Check(apple&&std::string(apple->Kind())=="HFS"&&!apple->Writable()&&!apple->List().empty(),
                  "Apple Disk Copy self-mounting image could not be browsed");
            fprintf(report,"PASS: Apple Disk Copy 6.3.3 self-mounting image opened as classic HFS.\n");
        }
        fprintf(report,"PASS: NDIF MacBinary I/II/III, stored/zero/ADC runs, chunk bounds, malformed streams and normal browser routing.\n");
    }catch(const std::exception& error){
        fprintf(report,"FAIL: %s\n",error.what());
        status=1;
    }
    fclose(report);
    return status;
}
#endif
