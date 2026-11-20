// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "NdifImage.h"
#include "VolumeSession.h"
#include "NdifLzh.h"
#include "DmgImage.h"
#include "CopyProcess.h"
#include "FileOperations.h"
#include "MfsImage.h"
#include "WorkerEndpoint.h"
#include "TransferConversion.h"
#include "ReadOnlyCdBackend.h"
#include "HfsPlusDisk.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include <cstdio>
extern "C" {
#include "hfs/macbinary.h"
#include "hfs/libhfs/hfs.h"
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
    if(!resources.empty())memcpy(p+forkAt,resources.data(),resources.size());
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


Bytes Apple(const Bytes& data,const Bytes& resource,bool single,bool rdwr=false,bool locked=false)
{
    const unsigned entries=(single?2:1)+(rdwr?2:0);
    const auto first=26+entries*12;
    Bytes packed(first+(single?data.size():0)+resource.size()+(rdwr?36:0));
    auto p=packed.data();
    P32(p,single?0x00051600:0x00051607);P32(p+4,0x00020000);P16(p+24,uint16_t(entries));
    unsigned offset=26;
    if(single){
        P32(p+offset,1);P32(p+offset+4,first);P32(p+offset+8,uint32_t(data.size()));
        memcpy(p+first,data.data(),data.size());
        offset+=12;
    }
    const auto start=first+(single?data.size():0);
    P32(p+offset,2);P32(p+offset+4,uint32_t(start));P32(p+offset+8,uint32_t(resource.size()));
    memcpy(p+start,resource.data(),resource.size());
    if(rdwr){
        const auto info=uint32_t(start+resource.size());
        offset+=12;
        P32(p+offset,9);P32(p+offset+4,info);P32(p+offset+8,32);
        memcpy(p+info,"dimgddsk",8);
        offset+=12;
        P32(p+offset,10);P32(p+offset+4,info+32);P32(p+offset+8,4);
        P32(p+info+32,locked?1:0);
    }
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

namespace {
void RdWrCase(const std::string& root,const Bytes& disk,const char* kind,bool rawCarrier)
{
    const auto name=std::string(kind);
    auto bcem=Map(uint32_t(disk.size()/512),0);
    auto wrapped=Pack(disk,rawCarrier?Bytes{}:Resource(bcem));
    if(rawCarrier)memcpy(wrapped.data()+65,"DDimddsk",8);
    macbinary_finish_header(wrapped.data(),MacBinaryIII);
    const auto path=Save(root+"writable-"+name+".bin",wrapped);
    auto volume=MacBrowser::OpenBrowserImage(path);
    Check(volume&&std::string(volume->Kind())==kind&&volume->Writable()&&
          MacBrowser::ExecutionFor(*volume).Persistent(),"NDIF did not open on a writable volume worker");
    Check(MacBrowser::OpenBrowserImage(path)==volume,"NDIF mount was not shared");
    Reject([&]{HfsPlus::OpenImage(path.c_str(),true);},"A second NDIF writer obtained a conflicting handle");

    MacBrowser::Entry entry;entry.name=u"Two Forks";
    entry.forks[0].size=4;entry.forks[1].size=4;
    const auto reader=[](unsigned fork,uint64_t at,void* target,size_t length){
        memcpy(target,(fork?"RSRC":"DATA")+size_t(at),length);
    };
    const auto id=volume->Put(2,entry,reader,false);
    volume->Rename(id,2,u"Updated");
    const auto other=volume->Put(2,entry,reader,false);
    volume->Remove(other);
    Reject([&]{
        auto failed=entry;failed.name=u"Updated";
        volume->Put(2,failed,[](unsigned,uint64_t,void*,size_t){throw Error("Injected NDIF source failure");},true);
    },"Failed overwrite was reported as successful");
    MacBrowser::Entry updated;
    Check(volume->FindChild(2,u"Updated",updated)&&!volume->FindChild(2,u"Two Forks",entry),
          "NDIF did not preserve rename/delete/failed-overwrite results");
    for(unsigned fork=0;fork<2;++fork){
        char data[4];
        volume->Read(updated.id,fork,0,data,4);
        Check(!memcmp(data,fork?"RSRC":"DATA",4),"NDIF write altered a fork");
    }
    volume.reset();

    const auto before=wrapped;
    auto file=HfsPlus::OpenImage(path.c_str(),true);
    const auto after=Disk::Read(*file,0,size_t(file->Size()));
    const size_t end=128+disk.size();
    Check(after.size()==before.size()&&std::equal(before.begin(),before.begin()+128,after.begin())&&
          std::equal(before.begin()+end,before.end(),after.begin()+end),
          "NDIF edit wrote outside the data fork");
    Check(Ndif::Unwrap(file)&&file->Writable(),"Reopened NDIF lost write support");
    char data=0;
    Reject([&]{file->Write(file->Size(),&data,1);},"NDIF allowed a write beyond its payload");
    file.reset();
    auto reopened=MacBrowser::OpenBrowserImage(path);
    Check(reopened->FindChild(2,u"Updated",updated),"NDIF edit did not survive reopening");

    auto locked=before;
    locked[81]=1;
    macbinary_finish_header(locked.data(),MacBinaryIII);
    const auto protectedPath=Save(root+"locked-"+name+".bin",locked);
    auto protectedVolume=MacBrowser::OpenBrowserImage(protectedPath);
    Check(protectedVolume&&!protectedVolume->Writable(),"Locked NDIF image became writable");
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
        auto rle=Map(1,2);
        const Bytes inputRle{0xff,0x01,'R','L',0,1,'E','!'};
        Row(rle,0,0,0x81,0,uint32_t(inputRle.size()));Row(rle,1,1,0xff,0,0);
        std::unique_ptr<Source> rleSource=std::make_unique<Memory>(Pack(inputRle,Resource(rle)));
        Check(Ndif::Unwrap(rleSource),"NDIF RLE did not open");
        const auto repeated=Disk::Read(*rleSource,0,512);
        for(size_t i=0;i<510;i+=2)
            Check(repeated[i]=='R'&&repeated[i+1]=='L',"NDIF RLE word repetition differed");
        Check(repeated[510]=='E'&&repeated[511]=='!',"NDIF RLE literal differs");
        auto badRle=inputRle;badRle[1]=0xff;badRle[0]=0xfe;
        rleSource=std::make_unique<Memory>(Pack(badRle,Resource(rle)));
        Check(Ndif::Unwrap(rleSource),"Oversized NDIF RLE fixture did not reach its codec");
        Reject([&]{Disk::Read(*rleSource,0,512);},"NDIF accepted an oversized RLE run");
        fprintf(report,"PASS: NDIF RLE word runs and decoding bounds.\n");
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
        Storage::Job verifySegments;
        Ndif::VerifySegmentChecksums(*joined,verifySegments);
        joined.reset();
        auto wrongChecksum=segment(2);P32(wrongChecksum.data()+20,1);
        const auto otherRoot=VolumeCoord::Wide(root+"bad-segment");
        Check(CreateDirectoryW(otherRoot.c_str(),nullptr)!=FALSE,"Could not create checksum fixture directory");
        const auto badPrimary=Save(root+"bad-segment\\lead.smi.bin",
            Pack(Bytes(data.begin(),data.begin()+523),Resource(segmented,segment(1))));
        Save(root+"bad-segment\\continuation.part.bin",Pack(Bytes(data.begin()+523,data.end()),Resource({},wrongChecksum)));
        const auto badOutput=VolumeCoord::Wide(root+"bad-segment-extracted.img");
        Storage::Job verifyBad;
        bool rejected=false;
        try{rejected=Dmg::Convert(badPrimary,badOutput,Dmg::Output::RawImage,verifyBad).code!=0;}
        catch(const Error& error){rejected=strstr(error.what(),"segment 2 checksum")!=nullptr;}
        Check(rejected&&GetFileAttributesW(badOutput.c_str())==INVALID_FILE_ATTRIBUTES,
              "Bad NDIF segment checksum published extracted data");
        Save(root+"duplicate.part.bin",Pack(Bytes(data.begin()+523,data.end()),Resource({},segment(2))));
        Reject([&]{open();},"NDIF accepted duplicate segments");
        if(const auto real=_wgetenv(L"HFV_NDIF_SEGMENT_TEST_IMAGE")){
            auto update=MacBrowser::OpenBrowserImage(real);
            Check(update&&update->Name()==u"Mac OS 8.1 Update"&&!update->List().empty(),"Apple multipart NDIF failed to open");
            const auto dest=VolumeCoord::Wide(root+"apple-segments.img");
            Storage::Job job;
            const auto result=Dmg::Convert(real,dest,Dmg::Output::RawImage,job);
            Check(!result.code,result.message.c_str());
            fprintf(report,"PASS: 13-part Apple Mac OS 8.1 update, including original logical and per-segment checksums.\n");
        }
        fprintf(report,"PASS: NDIF segment identity, missing/duplicate rejection and a chunk crossing parts.\n");
        if(const auto fixture=_wgetenv(L"HFV_NDIF_SHRINKWRAP_TEST")){
            auto file=HfsPlus::OpenImage(fixture,false);
            const auto packed=Disk::Read(*file,0,size_t(file->Size()));
            auto sw=Map(128,2);P32(sw.data()+72,128);
            Row(sw,0,0,0xf0,0,uint32_t(packed.size()));Row(sw,1,128,0xff,0,0);
            std::unique_ptr<Source> image=std::make_unique<Memory>(Pack(packed,Resource(sw)));
            Check(Ndif::Unwrap(image),"NDIF ShrinkWrap sample did not open");
            const auto output=Disk::Read(*image,0,65536);
            uint32_t crc=UINT32_MAX;
            for(const auto byte:output){
                crc^=byte;
                for(unsigned i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
            }
            Check(~crc==0x93a5e340,"NDIF ShrinkWrap output differs from the published checksum");
            auto damaged=packed;damaged.resize(40);P32(damaged.data(),0x28000000);
            Row(sw,0,0,0xf0,0,uint32_t(damaged.size()));
            image=std::make_unique<Memory>(Pack(damaged,Resource(sw)));
            Check(Ndif::Unwrap(image),"Truncated ShrinkWrap fixture did not reach the codec");
            Reject([&]{Disk::Read(*image,0,65536);},"NDIF accepted a truncated ShrinkWrap stream");
            fprintf(report,"PASS: NDIF ShrinkWrap SIT2 sample, plus truncated stream rejection.\n");
        }
        if(const auto fixture=_wgetenv(L"HFV_NDIF_LZH_TEST")){
            auto file=HfsPlus::OpenImage(fixture,false);
            unsigned cases=0;
            constexpr size_t bytes=20960;
            for(uint64_t at=0;at<file->Size();){
                const auto size=U32(Disk::Read(*file,at,4).data());
                at+=4;
                auto packed=Disk::Read(*file,at,size);
                at+=size;
                const auto expected=Disk::Read(*file,at,bytes);
                at+=bytes;
                Bytes expanded(bytes);
                Ndif::DecodeLzh(packed,expanded);
                Check(expanded==expected,"Apple LZH fixture disagrees with its separately decoded RLE copy");
                packed.resize(1);
                Reject([&]{Ndif::DecodeLzh(packed,expanded);},"Apple LZH accepted truncated input");
                ++cases;
            }
            Check(cases>0,"No Apple LZH fixtures were checked");
            fprintf(report,"PASS: %u Apple DART LZH chunks matched the corresponding RLE chunks byte-for-byte.\n",cases);
        }
        auto contiguous=Map(1,0);
        P32(contiguous.data()+76,5);
        std::unique_ptr<Source> noTable=std::make_unique<Memory>(Pack(data,Resource(contiguous)));
        Check(Ndif::Unwrap(noTable)&&Disk::Read(*noTable,0,512)==Bytes(512,'A'),
              "Contiguous legacy NDIF did not read its designated data fork");
        Bytes shortData(data.begin(),data.begin()+516);
        noTable=std::make_unique<Memory>(Pack(shortData,Resource(contiguous)));
        Reject([&]{Ndif::Unwrap(noTable);},"Truncated contiguous NDIF accepted");
        P32(contiguous.data()+116,1);
        noTable=std::make_unique<Memory>(Pack(data,Resource(contiguous)));
        Reject([&]{Ndif::Unwrap(noTable);},"An encrypted NDIF was treated as ordinary data");
        fprintf(report,"PASS: contiguous legacy NDIF, bounds and encrypted-image rejection.\n");
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
        const std::array<Bytes,2> forks{Bytes{'D','A','T','A'},Bytes{'R','S','R','C'}};
        {
            HfsPlus::Volume fixture(*raw,true);
            fixture.Put(2,u"Both forks",4,4,[&](unsigned fork,uint64_t at,void* target,size_t size){
                memcpy(target,forks[fork].data()+size_t(at),size);
            });
        }
        const auto volumeBytes=Disk::Read(*raw,0,size_t(raw->Size()));
        RdWrCase(root,volumeBytes,"HFS+",false);
        const auto classic=Save(root+"classic-rdwr.raw",Bytes(4*1024*1024));
        auto native=root+"classic-rdwr.raw";
        char label[]="Classic RdWr";
        Check(hfs_format(native.data(),0,label)==0,"Could not format classic NDIF fixture");
        auto classicFile=HfsPlus::OpenImage(classic.c_str(),false);
        RdWrCase(root,Disk::Read(*classicFile,0,size_t(classicFile->Size())),"HFS",false);
        const auto mfsPath=Save(root+"mfs-rdwr.raw",Bytes(400*1024));
        {
            MfsImageSource image;
            std::string error;
            Check(image.Open(mfsPath.c_str(),error,true),"Could not open MFS fixture");
            Check(MfsImage::Format(image,"MFS RdWr",400*1024,false,error),error.c_str());
        }
        auto mfsFile=HfsPlus::OpenImage(mfsPath.c_str(),false);
        RdWrCase(root,Disk::Read(*mfsFile,0,size_t(mfsFile->Size())),"MFS",true);
        fprintf(report,"PASS: HFS+, HFS and MFS in-place NDIF edits, exclusive writers, wrapper preservation and failed overwrite recovery.\n");
        const auto partitionCase=[&](const char* name,HfsPlus::Source& source,bool readOnly){
            const size_t start=readOnly?0:4096;
            Bytes disk(start+size_t(source.Size()));
            source.Read(0,disk.data()+start,size_t(source.Size()));
            P16(disk.data(),0x4552);P16(disk.data()+2,512);
            auto map=disk.data()+512;
            P16(map,0x504d);P32(map+4,1);
            P32(map+8,uint32_t(start/512));P32(map+12,uint32_t(source.Size()/512));
            memcpy(map+48,"Apple_HFS",9);
            const auto path=Save(root+name+".bin",Pack(disk,Resource(Map(uint32_t(disk.size()/512),0))));
            if(!readOnly){
                auto encoded=HfsPlus::OpenImage(path.c_str(),false);
                Check(Ndif::Unwrap(encoded),"Open NDIF for partition selection test");
                auto selected=DiskPartitions::FindLegacy(*encoded);
                Check(selected&&selected->partition.offset==start&&!selected->partition.readOnly,
                      "Missing original NDIF partition selection");
                encoded.reset();
                selected->partition.offset+=512;
                const auto format=selected->filesystem==DiskPartitions::Filesystem::Classic?
                    VolumeSession::ImageFormat::NdifClassic:VolumeSession::ImageFormat::NdifMfs;
                Reject([&]{VolumeSession::MountPersistentImage(path,VolumeCoord::ImageKey(path),{},format,*selected);},
                       "The worker mounted a partition different from the captured selection");
            }
            {
                auto volume=MacBrowser::OpenBrowserImage(path);
                Check(volume&&volume->Writable()!=readOnly,"NDIF ignored partition write boundaries");
                MacBrowser::Entry file;file.name=u"Inside Partition";file.forks[0].size=4;
                const auto put=[&]{
                    return volume->Put(2,file,[](unsigned,uint64_t at,void* bytes,size_t length){
                        memcpy(bytes,"APM!"+size_t(at),length);
                    },false);
                };
                if(readOnly)Reject(put,"NDIF allowed a write into the partition map");
                else put();
            }
            if(!readOnly){
                auto volume=MacBrowser::OpenBrowserImage(path);
                MacBrowser::Entry entry;
                Check(volume->FindChild(2,u"Inside Partition",entry)&&entry.forks[0].size==4,
                      "Partitioned NDIF edit did not persist");
                char data[4];
                volume->Read(entry.id,0,0,data,sizeof(data));
                Check(!memcmp(data,"APM!",4),"Partitioned NDIF wrote the wrong data");
            }
            auto encoded=HfsPlus::OpenImage(path.c_str(),false);
            Check(Ndif::Unwrap(encoded),"NDIF edit damaged its wrapper");
            const auto count=readOnly?disk.size():start;
            const auto unchanged=Disk::Read(*encoded,0,count);
            Check(std::equal(unchanged.begin(),unchanged.end(),disk.begin()),"NDIF modified bytes outside its writable partition");
        };
        partitionCase("partitioned-classic",*classicFile,false);
        partitionCase("partitioned-mfs",*mfsFile,false);
        partitionCase("protected-partition",*mfsFile,true);
        fprintf(report,"PASS: validated APM classic HFS and MFS partitions for NDIF writes, with map protection and stale worker selection rejection.\n");
        const auto sectors=uint32_t(raw->Size()/512);
        const auto rdwrMap=Resource(Map(sectors,0));
        const auto appleSingle=Save(root+"editable.as",Apple(volumeBytes,rdwrMap,true,true));
        auto appleDouble=Save(root+"editable.img",volumeBytes);
        auto sidecar=Apple({},rdwrMap,false,true);
        const auto sidecarPath=Save(root+"._editable.img",sidecar);
        const auto nativePath=Save(root+"editable-ntfs.img",volumeBytes);
        const auto saveStream=[&](const wchar_t* name,const Bytes& bytes){
            const auto handle=CreateFileW((nativePath+name).c_str(),GENERIC_WRITE,0,nullptr,
                                          CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            Check(handle!=INVALID_HANDLE_VALUE,"Cannot write the NTFS writable NDIF fixture");
            DWORD actual=0;
            const bool okay=WriteFile(handle,bytes.data(),DWORD(bytes.size()),&actual,nullptr)!=FALSE;
            CloseHandle(handle);
            Check(okay&&actual==bytes.size(),"Cannot save the NTFS writable NDIF fixture");
        };
        saveStream(L":AFP_Resource",rdwrMap);
        Bytes afp(60);
        memcpy(afp.data(),"AFP",3);afp[6]=1;memcpy(afp.data()+16,"dimgddsk",8);
        saveStream(L":AFP_AfpInfo",afp);
        for(const auto& path:{appleSingle,appleDouble,nativePath}){
            auto volume=MacBrowser::OpenBrowserImage(path);
            Check(volume&&volume->Writable(),"Preserved Finder metadata did not enable the NDIF editor");
            volume->MakeDirectory(2,u"NDIF edits");
            volume.reset();
            volume=MacBrowser::OpenBrowserImage(path);
            MacBrowser::Entry present;
            Check(volume->FindChild(2,u"NDIF edits",present),"Companion NDIF edit did not survive reopening");
        }
        auto untouched=HfsPlus::OpenImage(sidecarPath.c_str(),false);
        Check(Disk::Read(*untouched,0,sidecar.size())==sidecar,"NDIF edit changed its AppleDouble companion");
        const auto lockedApple=Save(root+"locked.as",Apple(volumeBytes,rdwrMap,true,true,true));
        Check(!MacBrowser::OpenBrowserImage(lockedApple)->Writable(),"AppleSingle lock did not disable NDIF writes");
        fprintf(report,"PASS: AppleSingle, AppleDouble and NTFS NDIF editing with preserved Finder metadata.\n");
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
        CopyProcess::enabled=true;
        unsigned number=0;
        std::vector<std::wstring> carriers{path,single,doublePath,streamPath,binHexPath};
        if(const auto original=_wgetenv(L"HFV_NDIF_TEST_IMAGE"))carriers.emplace_back(original);
        if(const auto multipart=_wgetenv(L"HFV_NDIF_SEGMENT_TEST_IMAGE"))carriers.emplace_back(multipart);
        for(const auto& wrapped:carriers){
            auto source=MacBrowser::OpenBrowserImage(wrapped);
            const auto entries=source->List();
            const auto selected=std::find_if(entries.begin(),entries.end(),[](const auto& item){
                return !item.directory;
            });
            Check(selected!=entries.end(),"NDIF worker fixture has no files");
            const auto file=*selected;
            const auto dest=VolumeCoord::Wide(root+"export-"+std::to_string(number++));
            Check(CreateDirectoryW(dest.c_str(),nullptr)!=FALSE,"Create NDIF worker destination");
            FileOperations::Request request=FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(source,file.id)},FileOperations::Location::Windows(dest)};
            request.background=true;
            CopyProcess::lastProcess=0;
            auto operation=FileOperations::Submit(std::move(request));
            const auto start=GetTickCount();
            while(!operation.Job()->done()){
                Check(GetTickCount()-start<15000,"NDIF worker export timed out");
                Sleep(5);
            }
            const auto copied=operation.Job()->result();
            Check(!copied.code,copied.message.c_str());
            Check(CopyProcess::lastProcess!=0,"NDIF export did not use the subprocess");
            auto windows=MacBrowser::OpenWindows(dest);
            const auto output=windows->List().at(0);
            for(unsigned fork=0;fork<2;++fork){
                Bytes bytes(size_t(file.forks[fork].size)),expected(bytes.size());
                Check(output.forks[fork].size==bytes.size(),"NDIF export lost a fork");
                if(!bytes.empty()){
                    source->Read(file.id,fork,0,expected.data(),expected.size());
                    windows->Read(output.id,fork,0,bytes.data(),bytes.size());
                    Check(bytes==expected,"NDIF export changed a fork");
                }
            }
        }
        fprintf(report,"PASS: real copy-worker export from NDIF in all five fork carriers, preserving both forks.\n");

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
        const auto source=Save(root+"editable-source.raw",volumeBytes);
        const auto createdRdWr=VolumeCoord::Wide(root+"created-rdwr.bin");
        Storage::Job createRdWr;
        const auto resultRdWr=Dmg::Convert(source,createdRdWr,Dmg::Output::EditableNdif,createRdWr);
        Check(!resultRdWr.code,resultRdWr.message.c_str());
        {
            auto encoded=HfsPlus::OpenImage(createdRdWr.c_str(),false);
            Check(Ndif::Unwrap(encoded)&&Ndif::IsEditable(*encoded)&&!Ndif::ExpectedChecksum(*encoded)&&
                  Disk::Read(*encoded,0,volumeBytes.size())==volumeBytes,"Created RdWr sectors differ from the source");
        }
        {
            auto volume=MacBrowser::OpenBrowserImage(createdRdWr);
            Check(volume&&volume->Writable(),"Created RdWr could not mount writable");
            volume->MakeDirectory(2,u"Created writable");
        }
        {
            auto volume=MacBrowser::OpenBrowserImage(createdRdWr);
            MacBrowser::Entry entry;
            Check(volume->FindChild(2,u"Created writable",entry),"Created RdWr edit did not persist");
            auto original=HfsPlus::OpenImage(source.c_str(),false);
            Check(Disk::Read(*original,0,volumeBytes.size())==volumeBytes,"Creating and editing RdWr changed the source");
        }
        const auto cancelledRdWr=VolumeCoord::Wide(root+"cancelled-rdwr.bin");
        Storage::Job cancelRdWr;
        cancelRdWr.transport([&](const Storage::Progress&){cancelRdWr.cancel();},{});
        bool stopped=false;
        try{stopped=Dmg::Convert(source,cancelledRdWr,Dmg::Output::EditableNdif,cancelRdWr).code!=0;}
        catch(const Storage::Cancelled&){stopped=true;}
        Check(stopped&&GetFileAttributesW(cancelledRdWr.c_str())==INVALID_FILE_ATTRIBUTES,
              "Cancelled RdWr creation published an image");
        fprintf(report,"PASS: created RdWr image round-trip, writable reopen, unchanged source and cancelled publication.\n");

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
    CopyProcess::enabled=false;
    fclose(report);
    return status;
}
#endif
