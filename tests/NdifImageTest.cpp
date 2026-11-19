// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "NdifImage.h"
#include "DmgImage.h"
#include "TransferConversion.h"
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

Bytes Resource(const Bytes& bcem,const Bytes& segment={})
{
    struct Item {const char* type;const Bytes* bytes;};
    std::vector<Item> items;
    if(!bcem.empty())items.push_back({"bcem",&bcem});
    if(!segment.empty())items.push_back({"bcm#",&segment});
    size_t map=256;
    for(const auto& item:items)map+=4+item.bytes->size();
    const auto mapBytes=30+items.size()*20;
    Bytes out(map+mapBytes);
    const auto p=out.data();
    P32(p,256);P32(p+4,uint32_t(map));P32(p+8,uint32_t(map-256));P32(p+12,uint32_t(mapBytes));
    const auto m=p+map;
    memcpy(m,p,16);P16(m+24,28);P16(m+26,uint16_t(mapBytes));
    P16(m+28,uint16_t(items.size()-1));
    size_t at=256;
    for(size_t i=0;i<items.size();++i){
        const auto& item=items[i];
        const auto type=m+30+i*8;
        memcpy(type,item.type,4);P16(type+6,uint16_t(2+items.size()*8+i*12));
        auto ref=m+30+items.size()*8+i*12;
        P16(ref,128);P16(ref+2,0xffff);P32(ref+4,uint32_t(at-256));
        P32(p+at,uint32_t(item.bytes->size()));
        memcpy(p+at+4,item.bytes->data(),item.bytes->size());
        at+=4+item.bytes->size();
    }
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


Bytes Apple(const Bytes& data,const Bytes& resource,bool single)
{
    const auto first=single?50u:38u;
    Bytes packed(first+(single?data.size():0)+resource.size());
    auto p=packed.data();
    P32(p,single?0x00051600:0x00051607);P32(p+4,0x00020000);P16(p+24,single?2:1);
    unsigned offset=26;
    if(single){
        P32(p+offset,1);P32(p+offset+4,first);P32(p+offset+8,uint32_t(data.size()));
        memcpy(p+first,data.data(),data.size());
        offset+=12;
    }
    const auto start=first+(single?data.size():0);
    P32(p+offset,2);P32(p+offset+4,uint32_t(start));P32(p+offset+8,uint32_t(resource.size()));
    memcpy(p+start,resource.data(),resource.size());
    return packed;
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
        Bytes bits;
        unsigned bit=0;
        auto emit=[&](unsigned value,unsigned width){
            for(unsigned i=width;i;--i){
                if(bit%8==0)bits.push_back(0);
                bits.back()|=uint8_t(((value>>(i-1))&1)<<(7-bit%8));
                ++bit;
            }
        };
        emit(0,3);emit('K',8); // one literal followed by distance-one copies
        const auto shortDistance=[&](unsigned offset){
            constexpr unsigned cutoffs[]={10,20,40,80,160,672};
            unsigned width=1;
            for(auto threshold:cutoffs)if(offset>threshold)++width;
            emit(0,width);
        };
        for(unsigned at=1;at<508;at+=3){
            emit(at==1?0:1,2);
            shortDistance(at);
        }
        emit(8,4);shortDistance(508); // length code 1000 gives a four-byte copy
        auto ken=Map(1,2);
        Row(ken,0,0,0x80,0,uint32_t(bits.size()));Row(ken,1,1,0xff,0,0);
        std::unique_ptr<Source> kenSource=std::make_unique<Memory>(Pack(bits,Resource(ken)));
        Check(Ndif::Unwrap(kenSource)&&Disk::Read(*kenSource,0,512)==Bytes(512,'K'),
              "NDIF KenCode expansion differed");
        bits.resize(bits.size()-4);
        Row(ken,0,0,0x80,0,uint32_t(bits.size()));
        kenSource=std::make_unique<Memory>(Pack(bits,Resource(ken)));
        Check(Ndif::Unwrap(kenSource),"Truncated NDIF KenCode fixture did not reach the codec");
        Reject([&]{Disk::Read(*kenSource,0,512);},"NDIF accepted truncated KenCode data");
        fprintf(report,"PASS: NDIF KenCode literals, overlapping copies and truncated stream rejection.\n");
        auto segmented=bcem;P32(segmented.data()+84,1);
        const auto segment=[](unsigned part){
            Bytes meta(24);
            P16(meta.data(),uint16_t(part));P16(meta.data()+2,2);
            meta[4]=0x5a;
            return meta;
        };
        const auto primary=Save(root+"lead.smi.bin",Pack(Bytes(data.begin(),data.begin()+523),Resource(segmented,segment(1))));
        auto open=[&]{
            auto source=HfsPlus::OpenImage(primary.c_str(),false);
            Check(Ndif::Unwrap(source),"Expected segmented NDIF");
            return source;
        };
        Reject([&]{open();},"NDIF accepted a missing segment");
        const auto next=Save(root+"continuation.part.bin",
                             Pack(Bytes(data.begin()+523,data.end()),Resource({},segment(2))));
        auto joined=open();
        Check(joined->WorkerFiles().size()==2&&Disk::Read(*joined,1024,512)==Bytes(512,'B'),
              "NDIF compressed chunk did not cross the physical segment boundary");
        joined.reset();
        Save(root+"duplicate.part.bin",Pack(Bytes(data.begin()+523,data.end()),Resource({},segment(2))));
        Reject([&]{open();},"NDIF accepted duplicate segments");
        if(const auto real=_wgetenv(L"HFV_NDIF_SEGMENT_TEST_IMAGE")){
            auto update=MacBrowser::OpenBrowserImage(real);
            Check(update&&update->Name()==u"Mac OS 8.1 Update"&&!update->List().empty(),"Apple multipart NDIF failed to open");
            const auto dest=VolumeCoord::Wide(root+"apple-segments.img");
            Storage::Job job;
            const auto result=Dmg::Convert(real,dest,Dmg::Output::RawImage,job);
            Check(!result.code,result.message.c_str());
            fprintf(report,"PASS: 13-part Apple Mac OS 8.1 update, including original logical checksum.\n");
        }
        fprintf(report,"PASS: NDIF segment identity, missing/duplicate rejection and a chunk crossing parts.\n");
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
        P32(rawMap.data()+80,Ndif::Checksum(UINT32_MAX,volumeBytes.data(),volumeBytes.size()));
        auto path=Save(root+"volume.img.bin",Pack(volumeBytes,Resource(rawMap)));
        const auto resource=Resource(rawMap);
        auto single=Save(root+"volume.as",Apple(volumeBytes,resource,true));
        auto doublePath=Save(root+"sidecar.img",volumeBytes);
        Save(root+"._sidecar.img",Apple({},resource,false));
        auto streamPath=Save(root+"stream.img",volumeBytes);
        auto stream=CreateFileW((streamPath+L":AFP_Resource").c_str(),GENERIC_WRITE,0,
                                nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        Check(stream!=INVALID_HANDLE_VALUE,"Cannot create the NTFS resource-fork fixture");
        DWORD wrote=0;
        const bool saved=WriteFile(stream,resource.data(),DWORD(resource.size()),&wrote,nullptr)!=FALSE;
        CloseHandle(stream);
        Check(saved&&wrote==resource.size(),"Cannot write the NTFS resource-fork fixture");
        for(const auto& location:{single,doublePath,streamPath}){
            auto image=MacBrowser::OpenBrowserImage(location);
            Check(image&&image->Name()==u"NDIF Fixture"&&!image->Writable(),
                  "NDIF AppleSingle, AppleDouble or named resource fork did not mount");
        }
        fprintf(report,"PASS: AppleSingle, AppleDouble and NTFS AFP_Resource NDIF images opened read-only.\n");
        const auto carrier=VolumeCoord::Wide(root+"carrier.raw");
        {
            auto target=CreateFormatOutput(carrier.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*target,u"Carrier",false);
            HfsPlus::Volume volume(*target,true);
            volume.Put(2,u"Wrapped disk",volumeBytes.size(),resource.size(),[&](unsigned fork,uint64_t at,void* bytes,size_t n){
                memcpy(bytes,(fork?resource:volumeBytes).data()+size_t(at),n);
            });
            target->Keep();
        }
        auto carrierVolume=MacBrowser::OpenImage(carrier);
        auto forkEntry=carrierVolume->List().at(0);
        Storage::Job encodeBinHex;
        auto encoded=TransferConversion::Adapt('b',false,*carrierVolume,forkEntry,encodeBinHex);
        Bytes binhex(size_t(forkEntry.forks[0].size));
        for(size_t at=0;at<binhex.size();){
            const auto count=(std::min)(size_t(65536),binhex.size()-at);
            encoded(0,at,binhex.data()+at,count);
            at+=count;
        }
        const auto binHexPath=Save(root+"volume.smi.hqx",binhex);
        auto fromBinHex=MacBrowser::OpenBrowserImage(binHexPath);
        Check(fromBinHex&&fromBinHex->Name()==u"NDIF Fixture"&&!fromBinHex->Writable(),"NDIF in BinHex was not detected");
        binhex.resize(binhex.size()-12);
        std::unique_ptr<Source> damaged=std::make_unique<Memory>(std::move(binhex));
        Reject([&]{Ndif::Unwrap(damaged);},"NDIF accepted an incomplete BinHex resource fork");
        fprintf(report,"PASS: BinHex NDIF mount, fork CRCs and truncated carrier rejection.\n");
        auto mounted=MacBrowser::OpenBrowserImage(path);
        Check(mounted&&mounted->Name()==u"NDIF Fixture"&&!mounted->Writable(),"NDIF did not mount in the browser");

        const auto extractedPath=VolumeCoord::Wide(root+"extracted.img");
        Storage::Job extract;
        const auto extracted=Dmg::Convert(path,extractedPath,Dmg::Output::RawImage,extract);
        Check(!extracted.code,extracted.message.c_str());
        auto result=HfsPlus::OpenImage(extractedPath.c_str(),false);
        Check(Disk::Read(*result,0,size_t(result->Size()))==volumeBytes,"NDIF raw conversion changed sectors");
        Bytes patterns(512*512*2);
        std::fill(patterns.begin(),patterns.begin()+512*512,'Q');
        for(size_t i=512*512;i<patterns.size()-2048;++i)
            patterns[i]=uint8_t((i/5+i/199+i*2)%251);
        const auto inputPath=Save(root+"create-input.raw",patterns);
        uint64_t storedSize=0;
        for(auto format:{Dmg::Output::StoredNdif,Dmg::Output::CompressedNdif}){
            const bool compressed=format==Dmg::Output::CompressedNdif;
            const auto encoded=VolumeCoord::Wide(root+(compressed?"created-compressed.bin":"created-stored.bin"));
            Storage::Job encode;
            const auto created=Dmg::Convert(inputPath,encoded,format,encode);
            Check(!created.code,created.message.c_str());
            auto file=HfsPlus::OpenImage(encoded.c_str(),false);
            if(compressed)Check(file->Size()<storedSize,"NDIF ADC creation did not compress the repeated data");
            else storedSize=file->Size();
            Check(Ndif::Unwrap(file)&&Disk::Read(*file,0,patterns.size())==patterns,
                  "Generated NDIF did not match its source sectors");
            const auto verified=VolumeCoord::Wide(root+(compressed?"verified-compressed.img":"verified-stored.img"));
            Storage::Job decode;
            const auto completed=Dmg::Convert(encoded,verified,Dmg::Output::RawImage,decode);
            Check(!completed.code,completed.message.c_str());
        }
        fprintf(report,"PASS: generated stored and ADC-compressed MacBinary NDIF with exact round-trip and valid checksums.\n");
        auto badData=volumeBytes;
        badData.back()^=1;
        const auto badPath=Save(root+"corrupt.img.bin",Pack(badData,Resource(rawMap)));
        const auto unpublished=VolumeCoord::Wide(root+"should-not-exist.img");
        Storage::Job badJob;
        bool refused=false;
        try{refused=Dmg::Convert(badPath,unpublished,Dmg::Output::RawImage,badJob).code!=0;}
        catch(const Error& error){refused=strstr(error.what(),"checksum")!=nullptr;}
        Check(refused&&GetFileAttributesW(unpublished.c_str())==INVALID_FILE_ATTRIBUTES,
              "NDIF checksum error published a file");
        fprintf(report,"PASS: NDIF raw conversion is byte-exact and checksum failures leave no published output.\n");

        if(const auto authentic=_wgetenv(L"HFV_NDIF_TEST_IMAGE")){
            auto apple=MacBrowser::OpenBrowserImage(authentic);
            Check(apple&&std::string(apple->Kind())=="HFS"&&!apple->Writable()&&!apple->List().empty(),
                  "Apple Disk Copy self-mounting image could not be browsed");
            const auto converted=VolumeCoord::Wide(root+"apple.dmg");
            Storage::Job convert;
            const auto done=Dmg::Convert(authentic,converted,Dmg::Output::CompressedDmg,convert);
            Check(!done.code,done.message.c_str());
            const auto reopened=MacBrowser::OpenBrowserImage(converted);
            Check(reopened&&reopened->Name()==apple->Name()&&reopened->List().size()==apple->List().size(),
                  "Apple NDIF did not survive conversion to compressed DMG");
            fprintf(report,"PASS: Apple Disk Copy 6.3.3 self-mounting image mounted and passed its original checksum during DMG conversion.\n");
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
