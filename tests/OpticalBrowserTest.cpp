// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "OpticalReader.h"
#include "CopyProcess.h"
#include "HfsPlusDisk.h"
#include "CdImage.h"
#include "VolumeCoordinator.h"
#include <atlbase.h>
#include <imapi2.h>
#include <imapi2fs.h>

namespace {
using HfsPlus::Bytes;
using HfsPlus::Disk::Check;
constexpr size_t Block=2048;
const Bytes payload={'o','p','t','i','c','a','l',' ','d','a','t','a'};

void Le(uint8_t* p,uint64_t value,size_t bytes)
{
    for(size_t i=0;i<bytes;++i)p[i]=uint8_t(value>>(8*i));
}
void Tag(uint8_t* p,unsigned id,uint32_t at,size_t bytes,unsigned version)
{
    Le(p,id,2);Le(p+2,version,2);p[4]=0;Le(p+6,1,2);
    uint16_t crc=0;
    for(size_t i=16;i<bytes;++i){
        crc^=uint16_t(p[i])<<8;
        for(unsigned bit=0;bit<8;++bit)
            crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0));
    }
    Le(p+8,crc,2);Le(p+10,bytes-16,2);Le(p+12,at,4);
    for(size_t i=0;i<16;++i)if(i!=4)p[4]+=p[i];
}
void Entity(uint8_t* p,const char* value){memcpy(p+1,value,strlen(value));}
void Ad(uint8_t* p,uint32_t bytes,uint32_t lba,unsigned map)
{
    Le(p,bytes,4);Le(p+4,lba,4);Le(p+8,map,2);
}
enum class Map {Physical,Sparable,Virtual,Metadata};
Bytes Udf(unsigned revision,Map layout,bool mirror=false)
{
    constexpr unsigned sectors=512,physicalStart=300;
    const unsigned version=revision<0x200?2:3;
    Bytes disk(sectors*Block);
    auto sector=[&](unsigned i){return disk.data()+i*Block;};
    memcpy(sector(16)+1,"BEA01",5);sector(16)[6]=1;
    memcpy(sector(17)+1,revision<0x200?"NSR02":"NSR03",5);sector(17)[6]=1;
    memcpy(sector(18)+1,"TEA01",5);sector(18)[6]=1;
    auto anchor=sector(256);
    Le(anchor+16,3*Block,4);Le(anchor+20,257,4);
    Le(anchor+24,3*Block,4);Le(anchor+28,260,4);
    Tag(anchor,2,256,512,version);
    auto part=sector(257);
    Le(part+188,physicalStart,4);Le(part+192,sectors-physicalStart,4);
    Tag(part,5,257,512,version);
    auto lvd=sector(258);
    lvd[84]=8;memcpy(lvd+85,"UDF fixture",11);lvd[211]=12;
    Le(lvd+212,Block,4);Entity(lvd+216,"*OSTA UDF Compliant");Le(lvd+240,revision,2);
    const bool two=layout==Map::Virtual||layout==Map::Metadata;
    const unsigned selected=two?1:0;
    Ad(lvd+248,Block,0,selected);
    const unsigned mapBytes=two?70:layout==Map::Physical?6:64;
    Le(lvd+264,mapBytes,4);Le(lvd+268,two?2:1,4);
    lvd[440]=1;lvd[441]=6;Le(lvd+442,1,2);
    auto map=lvd+440+(two?6:0);
    if(layout!=Map::Physical){
        map[0]=2;map[1]=64;Le(map+36,1,2);
        Le(map+28,revision,2);
    }
    unsigned actual=physicalStart;
    if(layout==Map::Sparable){
        Entity(map+4,"*UDF Sparable Partition");
        Le(map+40,32,2);map[42]=1;Le(map+44,64,4);Le(map+48,270,4);
        auto spare=sector(270);Entity(spare+16,"*UDF Sparing Table");
        Le(spare+48,1,2);Le(spare+56,0,4);Le(spare+60,400,4);
        Tag(spare,0,270,64,version);
        actual=400;
    }else if(layout==Map::Virtual){
        Entity(map+4,"*UDF Virtual Partition");
        auto vat=sector(sectors-1);
        vat[27]=revision<0x200?0:248;
        Le(vat+34,3,2);
        const unsigned header=revision<0x200?0:152;
        const unsigned bytes=header+20+(revision<0x200?36:0);
        Le(vat+56,bytes,8);Le(vat+172,bytes,4);
        if(header)Le(vat+176,header,2);
        for(unsigned i=0;i<5;++i)Le(vat+176+header+4*i,32+i,4);
        if(!header)Entity(vat+176+20,"*UDF Virtual Alloc Tbl");
        Tag(vat,261,sectors-1-physicalStart,176+bytes,version);
        actual=physicalStart+32;
    }else if(layout==Map::Metadata){
        Entity(map+4,"*UDF Metadata Partition");
        Le(map+40,10,4);Le(map+44,11,4);Le(map+48,UINT32_MAX,4);
        const unsigned where=mirror?11:10;
        auto metadata=sector(physicalStart+where);metadata[27]=mirror?251:250;
        Le(metadata+56,8*Block,8);Le(metadata+212,8,4);
        Le(metadata+216,8*Block,4);Le(metadata+220,32,4);
        Tag(metadata,266,where,224,version);
        actual=physicalStart+32;
    }
    Tag(lvd,6,258,440+mapBytes,version);
    Tag(sector(259),8,259,512,version);
    memcpy(sector(260),part,Block);Tag(sector(260),5,260,512,version);
    memcpy(sector(261),lvd,Block);Tag(sector(261),6,261,440+mapBytes,version);
    Tag(sector(262),8,262,512,version);
    auto fsd=sector(actual);
    Ad(fsd+400,Block,1,selected);
    Tag(fsd,256,0,512,version);
    auto root=sector(actual+1),fid=sector(actual+2);
    root[27]=4;
    const std::array<uint8_t,9> name={16,0x03,0xa9,0,'F',0,'i',0,'l'};
    memcpy(fid+38,name.data(),name.size());fid[19]=uint8_t(name.size());
    Le(fid+16,1,2);Ad(fid+20,Block,3,selected);
    Tag(fid,257,2,48,version);
    Le(root+56,48,8);Le(root+172,8,4);Le(root+176,48,4);Le(root+180,2,4);
    Tag(root,261,1,184,version);
    auto file=sector(actual+3);
    file[27]=5;
    // Exercise EFE on UDF 2.x, FE on UDF 1.x, and long allocation descriptors.
    const auto fixed=version==2?176:216;
    Le(file+34,1,2);Le(file+56,payload.size(),8);Le(file+fixed-4,16,4);
    Ad(file+fixed,uint32_t(payload.size()),4,selected);
    Tag(file,version==2?261:266,3,fixed+16,version);
    memcpy(sector(actual+4),payload.data(),payload.size());
    return disk;
}

void RequireHr(HRESULT hr,const char* context)
{
    if(FAILED(hr))throw HfsPlus::Error(std::string(context)+" ("+std::to_string(uint32_t(hr))+")");
}
Bytes Master(FsiFileSystems filesystems,LONG revision=0x102)
{
    CComPtr<IFileSystemImage> image;
    RequireHr(image.CoCreateInstance(__uuidof(MsftFileSystemImage)),"Create Windows image mastering object");
    RequireHr(image->ChooseImageDefaultsForMediaType(IMAPI_MEDIA_TYPE_CDR),"Initialize mastering");
    RequireHr(image->put_FileSystemsToCreate(filesystems),"Choose optical filesystems");
    if(filesystems&FsiFileSystemUDF)
        RequireHr(image->put_UDFRevision(revision),"Set mastering UDF revision");
    RequireHr(image->put_VolumeName(CComBSTR(L"OPTICAL_TEST")),"Name optical image");
    CComPtr<IFsiDirectoryItem> root;
    RequireHr(image->get_Root(&root),"Get mastering root");
    RequireHr(root->AddDirectory(CComBSTR(L"SUBDIR")),"Add fixture folder");
    CComPtr<IStream> stream;
    RequireHr(CreateStreamOnHGlobal(nullptr,TRUE,&stream),"Create fixture data stream");
    ULONG count=0;
    RequireHr(stream->Write(payload.data(),ULONG(payload.size()),&count),"Write fixture file contents");
    LARGE_INTEGER zero{};
    RequireHr(stream->Seek(zero,STREAM_SEEK_SET,nullptr),"Rewind fixture stream");
    const auto name=filesystems==FsiFileSystemISO9660?L"FILE.TXT":L"\u03a9 File.txt";
    RequireHr(root->AddFile(CComBSTR((std::wstring(L"SUBDIR\\")+name).c_str()),stream),"Add fixture file");
    CComPtr<IFileSystemImageResult> result;
    RequireHr(image->CreateResultImage(&result),"Master optical image");
    CComPtr<IStream> output;
    RequireHr(result->get_ImageStream(&output),"Read mastered image");
    LONG blocks=0;
    RequireHr(result->get_TotalBlocks(&blocks),"Find mastered image size");
    Check(blocks>0&&blocks<100000,"Invalid fixture image size");
    Bytes bytes(size_t(blocks)*Block);
    size_t offset=0;
    while(offset<bytes.size()){
        const auto request=ULONG((std::min)(size_t(65536),bytes.size()-offset));
        RequireHr(output->Read(bytes.data()+offset,request,&count),"Read mastered image bytes");
        Check(count&&count<=request,"Mastered image is truncated");
        offset+=count;
    }
    return bytes;
}

std::vector<LONG> MasteringRevisions()
{
    CComPtr<IFileSystemImage> image;
    RequireHr(image.CoCreateInstance(__uuidof(MsftFileSystemImage)),"Create Windows image mastering object");
    SAFEARRAY* revisions=nullptr;
    RequireHr(image->get_UDFRevisionsSupported(&revisions),"Read supported mastering revisions");
    struct Array { SAFEARRAY* value; ~Array(){if(value)SafeArrayDestroy(value);} } owner{revisions};
    LONG first=0,last=-1;
    RequireHr(SafeArrayGetLBound(revisions,1,&first),"Read mastering revision lower bound");
    RequireHr(SafeArrayGetUBound(revisions,1,&last),"Read mastering revision upper bound");
    VARTYPE type=VT_EMPTY;
    RequireHr(SafeArrayGetVartype(revisions,&type),"Read mastering revision type");
    std::vector<LONG> result;
    for(LONG i=first;i<=last;++i){
        LONG revision=0;
        if(type==VT_I4)RequireHr(SafeArrayGetElement(revisions,&i,&revision),"Read mastering revision");
        else {
            CComVariant value;
            Check(type==VT_VARIANT,"Unexpected mastering revision type");
            RequireHr(SafeArrayGetElement(revisions,&i,&value),"Read mastering revision");
            RequireHr(value.ChangeType(VT_I4),"Convert mastering revision");
            revision=value.lVal;
        }
        result.push_back(revision);
    }
    return result;
}

std::wstring Save(const std::string& filename,const Bytes& bytes)
{
    const auto path=VolumeCoord::Wide(filename);
    auto image=HfsPlus::CreateFormatOutput(path.c_str(),bytes.size());
    image->Write(0,bytes.data(),bytes.size());
    image->Flush();
    image->Keep();
    return path;
}

class Disc final:public MediaImageSource {
    Bytes bytes_;
public:
    DWORD generation=1;
    bool closed=false;
    explicit Disc(Bytes bytes):bytes_(std::move(bytes)){}
    bool Open(int,std::string&)override{closed=false;return true;}
    ULONGLONG Size()const override{return bytes_.size();}
    bool Verify(DWORD& observed)override{observed=generation;return !closed;}
    DWORD Error()const override{return ERROR_MEDIA_CHANGED;}
    bool Read(ULONGLONG at,DWORD length,void* output,DWORD& got)override{
        if(closed||!HfsPlus::Disk::Range(at,length,bytes_.size()))return false;
        memcpy(output,bytes_.data()+at,length);got=length;return true;
    }
    void Close() override{closed=true;}
};

template<class F>void Reject(F f,const char* reason)
{
    try{f();}catch(const HfsPlus::Error&){return;}
    throw HfsPlus::Error(reason);
}

MacBrowser::Entry File(MacBrowser::Volume& volume,uint32_t directory,const std::u16string& name)
{
    MacBrowser::Entry file;
    Check(volume.FindChild(directory,name,file)&&!file.directory&&!file.forks[1].size&&
          file.forks[0].size==payload.size(),"Optical file listing or fork size is incorrect");
    Bytes actual(payload.size());
    volume.Read(file.id,0,0,actual.data(),actual.size());
    Check(actual==payload,"Optical file payload differs");
    Check(!volume.Writable(),"Optical browser exposed writes");
    Reject([&]{volume.Remove(file.id);},"Optical file deletion was allowed");
    return file;
}

void RockRidgeFixtures(const std::wstring& fixtures,const std::string& output,FILE* report)
{
    using namespace HfsPlus;
    const auto expected=std::string("independently mastered Rock Ridge data\n");
    const auto fileName=u"Caf\u00e9 notes.txt";
    const auto read=[&](MacBrowser::Volume& volume,uint32_t folder,const std::u16string& name){
        MacBrowser::Entry file;
        Check(volume.FindChild(folder,name,file)&&!file.directory&&file.forks[0].size==expected.size(),
              "Rock Ridge file or its length is missing");
        std::string bytes(expected.size(),0);
        volume.Read(file.id,0,0,bytes.data(),bytes.size());
        Check(bytes==expected,"Rock Ridge file bytes differ");
        return file;
    };
    const auto load=[&](const wchar_t* name){
        auto path=fixtures+L"\\"+name;
        auto image=HfsPlus::OpenImage(path.c_str(),false);
        return Disk::Read(*image,0,size_t(image->Size()));
    };
    for(const auto revision:{L"rrip-109.iso",L"rrip-112.iso"}){
        const auto path=fixtures+L"\\"+revision;
        auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&std::string(volume->Kind())=="ISO 9660 / Rock Ridge"&&!volume->Writable(),
              "Independent Rock Ridge image did not open read-only");
        MacBrowser::Entry folder,link;
        Check(volume->FindChild(2,u"Unix docs",folder)&&folder.directory,"Rock Ridge directory name missing");
        const auto file=read(*volume,folder.id,fileName);
        Check(file.datesValid&&file.created==3782844800ULL&&file.modified==3782844920ULL&&
              volume->PropertiesDetails(file.id).find("POSIX mode 0640")!=std::string::npos,
              "Rock Ridge timestamp or POSIX mode was not exposed");
        read(*volume,folder.id,std::u16string(225,u'a')+u".txt");
        Check(volume->FindChild(2,u"Unix link",link)&&!link.directory&&!link.readError.empty()&&
              volume->PropertiesDetails(link.id)=="Symbolic link to /Unix docs/Caf\xc3\xa9 notes.txt",
              "Rock Ridge link target is missing or the link is readable");
        char byte=0;
        Reject([&]{volume->Read(link.id,0,0,&byte,0);},"Rock Ridge symlink was readable");
        Reject([&]{volume->Remove(file.id);},"Rock Ridge allowed a filesystem mutation");
        uint32_t parent=2;
        for(unsigned depth=1;depth<=10;++depth){
            const auto number=std::to_wstring(depth);
            const std::u16string name=u"Long level "+std::u16string(number.begin(),number.end());
            Check(volume->FindChild(parent,name,folder)&&folder.directory,"Relocated Rock Ridge directory missing");
            parent=folder.id;
        }
        read(*volume,parent,u"Deep file.txt");

        const auto pathOut=output+"\\rock-ridge-export-"+(revision[6]==L'0'?"109":"112");
        Check(CreateDirectoryA(pathOut.c_str(),nullptr),"Create Rock Ridge output directory");
        FileOperations::Request request=FileOperations::OrdinaryCopy{
            {FileOperations::Location::Mounted(volume,file.id)},
            FileOperations::Location::Windows(VolumeCoord::Wide(pathOut))};
        request.background=true;
        CopyProcess::enabled=true;
        CopyProcess::lastProcess=0;
        const auto copy=FileOperations::Submit(std::move(request));
        Check(WaitForSingleObject(copy.Job()->event(),15000)==WAIT_OBJECT_0,"Rock Ridge copy timed out");
        CopyProcess::enabled=false;
        const auto result=copy.Job()->result();
        Check(!result.code&&CopyProcess::lastProcess,result.message.c_str());
        auto exported=MacBrowser::OpenWindows(VolumeCoord::Wide(pathOut));
        read(*exported,2,fileName);
    }

    const auto dual=load(L"rock-ridge-joliet.iso");
    const auto joliet=MacBrowser::OpenBrowserImage(fixtures+L"\\rock-ridge-joliet.iso");
    MacBrowser::Entry folder;
    Check(joliet&&std::string(joliet->Kind())=="ISO 9660 / Joliet"&&
          joliet->FindChild(2,u"Joliet docs",folder),"Adding Rock Ridge changed Joliet precedence");
    read(*joliet,folder.id,u"Joliet file.txt");

    // Combine the independently mastered dual-tree ISO with a valid Apple
    // partition in its system area, leaving every ISO/Joliet record intact.
    const auto hfsPath=VolumeCoord::Wide(output+"\\hybrid-hfs.img");
    constexpr size_t hfsBytes=8*1024*1024;
    auto hfs=CreateFormatOutput(hfsPath.c_str(),hfsBytes);
    Volume::Format(*hfs,u"Mac hybrid",false);
    const auto base=(dual.size()+4095)&~size_t(4095);
    auto hybrid=dual;
    hybrid.resize(base+hfsBytes);
    hfs->Read(0,hybrid.data()+base,hfsBytes);
    Disk::P16(hybrid.data(),0x4552);
    Disk::P16(hybrid.data()+2,512);
    Disk::P32(hybrid.data()+4,uint32_t(hybrid.size()/512));
    for(unsigned i=1;i<=2;++i){
        auto map=hybrid.data()+i*512;
        Disk::P16(map,0x504d);
        Disk::P32(map+4,2);
        Disk::P32(map+8,i==1?1:uint32_t(base/512));
        Disk::P32(map+12,i==1?2:uint32_t(hfsBytes/512));
        strcpy(reinterpret_cast<char*>(map+48),i==1?"Apple_partition_map":"Apple_HFS");
    }
    auto mounted=MacBrowser::OpenBrowserImage(Save(output+"\\mac-hybrid.iso",hybrid));
    Check(mounted&&mounted->Name()==u"Mac hybrid"&&!strcmp(mounted->Kind(),"HFS+"),
          "Rock Ridge changed the Macintosh side of a hybrid image");
    auto medium=std::make_shared<Disc>(hybrid);
    auto mac=MacBrowser::OpenCd(3,medium);
    Check(mac&&mac->Name()==u"Mac hybrid"&&!strcmp(mac->Kind(),"HFS+"),
          "Rock Ridge changed the Macintosh side of a hybrid disc");

    auto corrupt=load(L"rrip-112.iso");
    const auto lba=Optical::Le32(corrupt.data()+16*Block+158);
    auto dot=corrupt.data()+size_t(lba)*Block;
    size_t ce=0;
    for(size_t at=34;at+4<=dot[0];){
        const auto width=dot[at+2];
        if(width<4||at+width>dot[0])break;
        if(dot[at]=='C'&&dot[at+1]=='E'&&width==28){
            Le(dot+at+4,UINT32_MAX,4);
            Disk::P32(dot+at+8,UINT32_MAX);
            ce=at;
            break;
        }
        at+=width;
    }
    Check(ce!=0,"Independent fixture lacks a SUSP continuation");
    Reject([&]{MacBrowser::OpenBrowserImage(Save(output+"\\bad-continuation.iso",corrupt));},
           "Rock Ridge accepted a continuation outside the volume");
    const auto setBoth=[&](size_t field,uint32_t value){
        Le(dot+ce+field,value,4);
        Disk::P32(dot+ce+field+4,value);
    };
    setBoth(4,lba);
    setBoth(12,34);
    setBoth(20,uint32_t(ce+28-34));
    Reject([&]{MacBrowser::OpenBrowserImage(Save(output+"\\cyclic-continuation.iso",corrupt));},
           "Rock Ridge accepted a cyclic continuation");
    fprintf(report,"PASS: independently mastered RRIP 1.09 and 1.12 names, continuations, deep directories, timestamps, POSIX mode, safe links, worker export, Joliet and Mac hybrid priority, and out-of-volume and cyclic continuation rejection.\n");
}
}

int RunOpticalBrowserTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const std::string root=directory;
    FILE* report=fopen((root+"\\optical-browser-report.txt").c_str(),"w");
    if(!report)return 2;
    const auto co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int status=0;
    try{
        if(const auto fixtures=_wgetenv(L"HFV_ROCK_RIDGE_FIXTURES"))RockRidgeFixtures(fixtures,root,report);
        for(auto fs:{FsiFileSystemISO9660,FsiFileSystems(FsiFileSystemISO9660|FsiFileSystemJoliet),FsiFileSystemUDF,
                     FsiFileSystems(FsiFileSystemISO9660|FsiFileSystemUDF)}){
            const auto bytes=Master(fs);
            const auto path=Save(root+"\\master-"+std::to_string(fs)+".iso",bytes);
            auto volume=MacBrowser::OpenBrowserImage(path);
            Check(bool(volume),"Open the mastered ISO or UDF image");
            const char* kind=fs==FsiFileSystemISO9660?"ISO 9660":
                (fs&FsiFileSystemJoliet)?"ISO 9660 / Joliet":"UDF";
            Check(!strcmp(volume->Kind(),kind),"Wrong mastered image filesystem");
            const auto name=fs==FsiFileSystemISO9660?u"FILE.TXT":u"\u03a9 File.txt";
            MacBrowser::Entry folder;
            Check(volume->FindChild(2,u"SUBDIR",folder)&&folder.directory,"Missing mastered image folder");
            const auto file=File(*volume,folder.id,name);
            const auto output=root+"\\export-"+std::to_string(fs);
            Check(CreateDirectoryA(output.c_str(),nullptr),"Create optical export folder");
            auto target=MacBrowser::OpenWindows(VolumeCoord::Wide(output));
            FileOperations::Request request=FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(volume,file.id)},
                FileOperations::Location::Windows(VolumeCoord::Wide(output))};
            request.background=true;
            CopyProcess::enabled=true;
            const auto copy=FileOperations::Submit(std::move(request));
            const auto start=GetTickCount();
            while(!copy.Job()->done()){
                Check(GetTickCount()-start<15000,"Optical copy worker timed out");
                Sleep(5);
            }
            CopyProcess::enabled=false;
            const auto copiedResult=copy.Job()->result();
            Check(!copiedResult.code&&CopyProcess::lastProcess,copiedResult.message.c_str());
            File(*volume,folder.id,name);
            MacBrowser::Entry copied;
            Check(target->FindChild(2,name,copied)&&copied.forks[0].size==payload.size(),"Missing exported optical file");
            Bytes exported(payload.size());
            target->Read(copied.id,0,0,exported.data(),exported.size());
            Check(exported==payload,"Exported optical file data differs");
            auto media=std::make_shared<Disc>(bytes);
            auto disc=MacBrowser::OpenCd(3,media);
            Check(disc&&disc->CdDrive()==3&&disc->FindChild(2,u"SUBDIR",folder),
                  "CD drive did not mount the optical filesystem");
            File(*disc,folder.id,name);
            ++media->generation;
            Reject([&]{disc->List();},"Cached optical directory survived a media swap");
            disc->ReleaseMedia();
            Check(media->closed,"Releasing optical media left the source open");
        }
        fprintf(report,"PASS: Windows-mastered ISO 9660, Joliet, UDF 1.02 and UDF/ISO bridge images and simulated drives, folder browsing, Unicode names, export and changed media.\n");
        fflush(report);
        for(const auto revision:MasteringRevisions()){
            if(revision==0x102)continue; // already tested in both pure and bridge images
            const auto file=Save(root+"\\master-udf-"+std::to_string(revision)+".udf",Master(FsiFileSystemUDF,revision));
            const auto volume=MacBrowser::OpenImage(file);
            Check(volume&&!strcmp(volume->Kind(),"UDF"),"Open additional Windows-mastered UDF revision");
            MacBrowser::Entry folder;
            Check(volume->FindChild(2,u"SUBDIR",folder)&&folder.directory,"List additional mastered UDF revision");
            File(*volume,folder.id,u"\u03a9 File.txt");
            fprintf(report,"PASS: Windows-mastered UDF revision %x.\n",unsigned(revision));
        }
        for(const auto choice:{std::pair{0x100,Map::Physical},std::pair{0x101,Map::Physical},
                              std::pair{0x102,Map::Physical},std::pair{0x150,Map::Sparable},
                              std::pair{0x150,Map::Virtual},std::pair{0x200,Map::Virtual},
                              std::pair{0x201,Map::Physical},std::pair{0x250,Map::Metadata},
                              std::pair{0x260,Map::Metadata}}){
            const auto bytes=Udf(choice.first,choice.second,choice.first==0x260);
            const auto file=Save(root+"\\udf-"+std::to_string(choice.first)+"-"+std::to_string(int(choice.second))+".udf",bytes);
            const auto volume=MacBrowser::OpenImage(file);
            Check(volume&&!strcmp(volume->Kind(),"UDF"),"Open UDF revision fixture");
            File(*volume,2,u"\u03a9Fil");
        }
        auto broken=Udf(0x102,Map::Physical);
        broken[258*Block+8]^=1;
        const auto reserve=MacBrowser::OpenImage(Save(root+"\\reserve.udf",broken));
        Check(bool(reserve),"UDF reserve descriptor sequence did not recover the volume");
        File(*reserve,2,u"\u03a9Fil");
        broken[261*Block+8]^=1;
        const auto corrupt=Save(root+"\\corrupt.udf",broken);
        Reject([&]{MacBrowser::OpenImage(corrupt);},"Corrupt UDF descriptor sequences were accepted");
        fprintf(report,"PASS: UDF revisions 1.00 through 2.60, physical/sparable/VAT/metadata maps, metadata mirror, reserve descriptors and CRC rejection.\n");
    }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());status=1;}
    CopyProcess::enabled=false;
    if(SUCCEEDED(co))CoUninitialize();
    fclose(report);
    return status;
}
#endif
