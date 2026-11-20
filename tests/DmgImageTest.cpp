// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "DmgImage.h"
#include "DmgEncryption.h"
#include "DmgCertificate.h"
#include <wincrypt.h>
#include "SparseDisk.h"
#include "BrowserSession.h"
#include "HFVExplorer.h"
#include "HFVExplorerDoc.h"
#include "MainFrm.h"
#include "MainMacBrowser.h"
#include "StorageJobDialog.h"
#include "WorkerEndpoint.h"
#include "utils.h"
#include <bcrypt.h>
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
Bytes FrontHeader(Bytes file)
{
    Bytes footer(file.end()-512,file.end());
    file.resize(file.size()-512);
    for(size_t field:{24,40,216})
        if(U64(footer.data()+field+8))P64(footer.data()+field,U64(footer.data()+field)+512);
    file.insert(file.begin(),footer.begin(),footer.end());
    return file;
}
Bytes ResourceImage(const Bytes& contents)
{
    constexpr size_t data=256,map=204+2*40+data+4,mapBytes=50;
    Bytes fork(map+mapBytes);
    P32(fork.data(),data);P32(fork.data()+4,map);
    P32(fork.data()+8,map-data);P32(fork.data()+12,mapBytes);
    memcpy(fork.data()+map,fork.data(),16);
    auto typeMap=fork.data()+map;
    P16(typeMap+24,28);P16(typeMap+26,mapBytes);
    memcpy(typeMap+30,"blkx",4);P16(typeMap+36,10);
    P16(typeMap+38,128);P16(typeMap+40,0xffff);
    P32(fork.data()+data,map-data-4);
    auto mish=fork.data()+data+4;
    memcpy(mish,"mish",4);P32(mish+4,1);P64(mish+16,contents.size()/512);
    P32(mish+200,2);P32(mish+204,Raw);
    P64(mish+220,contents.size()/512);P64(mish+236,contents.size());
    P32(mish+244,UINT32_MAX);
    auto file=contents;
    file.insert(file.end(),fork.begin(),fork.end());
    Bytes footer(512);
    memcpy(footer.data(),"koly",4);P32(footer.data()+4,4);P32(footer.data()+8,512);
    P64(footer.data()+32,contents.size());
    P64(footer.data()+40,contents.size());P64(footer.data()+48,fork.size());
    P64(footer.data()+492,contents.size()/512);
    file.insert(file.end(),footer.begin(),footer.end());
    return file;
}
std::pair<Bytes,Bytes> Segments(const Bytes& whole,size_t cut)
{
    const auto footer=whole.data()+whole.size()-512;
    const auto at=U64(footer+24),length=U64(footer+32),xml=U64(footer+216);
    Check(cut>0&&cut<length,"Invalid DMG fixture segment size");
    Bytes first(whole.begin()+at,whole.begin()+at+cut);
    Bytes second(whole.begin()+at+cut,whole.begin()+at+length);
    second.insert(second.end(),whole.begin()+xml,whole.end()-512);
    Bytes trailer(footer,footer+512);
    trailer[64]=0xab;
    P64(trailer.data()+24,0);P32(trailer.data()+60,2);
    P32(trailer.data()+56,1);P64(trailer.data()+32,cut);
    P64(trailer.data()+216,0);P64(trailer.data()+224,0);
    first.insert(first.end(),trailer.begin(),trailer.end());
    P32(trailer.data()+56,2);P64(trailer.data()+16,cut);P64(trailer.data()+32,length-cut);
    P64(trailer.data()+216,length-cut);P64(trailer.data()+224,U64(footer+224));
    second.insert(second.end(),trailer.begin(),trailer.end());
    return {std::move(first),std::move(second)};
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
        const Bytes sample(SectorBytes,'S');
        auto older=ResourceImage(sample);
        auto resource=Open(Save(root+"\\resource-fork.dmg",older));
        Check(Disk::Read(*resource,0,sample.size())==sample,"Read resource-fork DMG map");
        auto front=Open(Save(root+"\\front-header.dmg",FrontHeader(older)));
        Check(Disk::Read(*front,0,sample.size())==sample,"Read leading UDIF header");
        const auto forkAt=U64(older.data()+older.size()-512+40);
        const auto resMap=U32(older.data()+forkAt+4);
        P32(older.data()+forkAt+resMap+42,0x00ffffff);
        Reject([&]{Open(Save(root+"\\bad-resource.dmg",older));},"Out-of-bounds resource reference was accepted");

        const auto [part1,part2]=Segments(Pack({{{Zlib,0,1,ZlibBlock(sample)}}},1),3);
        const auto firstPart=Save(root+"\\split.dmg",part1);
        Reject([&]{Open(firstPart);},"Split DMG opened with a missing segment");
        auto badPart=part2;
        badPart[badPart.size()-512+64]^=1;
        Save(root+"\\bad-split.dmg",part1);
        Save(root+"\\bad-split.002.dmgpart",badPart);
        Reject([&]{Open(VolumeCoord::Wide(root+"\\bad-split.dmg"));},"Split DMG accepted a different segment identifier");
        const auto segment=Save(root+"\\split.002.dmgpart",part2);
        Reject([&]{Open(segment);},"A later split segment opened by itself");
        auto split=Open(firstPart);
        Check(Disk::Read(*split,0,sample.size())==sample&&split->WorkerFiles().size()==2,
              "Compressed run split across files read incorrectly");
        fprintf(report,"PASS: XML and resource-fork maps, leading and trailing UDIF headers, split compression runs, missing/wrong segments, all codecs and malformed input.\n");
        fflush(report);

        const auto mfsPath=Save(root+"\\mfs-volume.img",Bytes(400*1024));
        {
            MfsImageSource mfs;
            std::string error;
            Check(mfs.Open(mfsPath.c_str(),error,true),"Open raw MFS fixture output");
            Check(MfsImage::Format(mfs,"MFS in DMG",400*1024,false,error),"Format MFS fixture");
        }
        auto rawMfs=HfsPlus::OpenImage(mfsPath.c_str(),false);
        constexpr uint32_t partition=4096;
        Bytes partitioned(partition+size_t(rawMfs->Size()));
        rawMfs->Read(0,partitioned.data()+partition,size_t(rawMfs->Size()));
        P16(partitioned.data(),0x4552);P16(partitioned.data()+2,512);
        auto map=partitioned.data()+512;
        P16(map,0x504d);P32(map+4,1);P32(map+8,partition/512);P32(map+12,uint32_t(rawMfs->Size()/512));
        memcpy(map+48,"Apple_MFS",9);
        const auto sectors=partitioned.size()/SectorBytes;
        const auto mfsDmg=Save(root+"\\partitioned-mfs.dmg",Pack({{{Raw,0,sectors,std::move(partitioned)}}},sectors));
        auto mfs=MacBrowser::OpenBrowserImage(mfsDmg);
        Check(mfs&&mfs->Name()==u"MFS in DMG"&&!mfs->Writable(),
              "DMG did not find the MFS volume inside its validated partition");
        fprintf(report,"PASS: MFS browsing from an APM partition inside a DMG.\n");

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
        const auto createdPath=VolumeCoord::Wide(root+"\\created.dmg");
        const auto extractedPath=VolumeCoord::Wide(root+"\\unpacked.img");
        Storage::Job create,extract;
        Check(!Dmg::Convert(rawPath,createdPath,Dmg::Output::CompressedDmg,create).code,
              "Create compressed DMG");
        const auto reopened=MacBrowser::OpenBrowserImage(createdPath);
        MacBrowser::Entry made;
        Check(reopened&&reopened->FindChild(2,u"Fixture",made)&&made.forks[1].size==1,
              "Created DMG could not be mounted with both forks");
        Check(!Dmg::Convert(createdPath,extractedPath,Dmg::Output::RawImage,extract).code,
              "Extract compressed DMG");
        auto unpacked=HfsPlus::OpenImage(extractedPath.c_str(),false);
        Check(unpacked->Size()==raw.size()&&Disk::Read(*unpacked,0,raw.size())==raw,
              "Compressed DMG round-trip changed raw disk bytes");
        Reject([&]{Dmg::Convert(rawPath,createdPath,Dmg::Output::CompressedDmg,create);},
               "DMG conversion overwrote an existing output");
        const auto cancelled=VolumeCoord::Wide(root+"\\cancelled.dmg");
        Storage::Job stop;
        stop.transport([&](const Storage::Progress&){stop.cancel();},{});
        bool stopped=false;
        try{Dmg::Convert(rawPath,cancelled,Dmg::Output::CompressedDmg,stop);}
        catch(const Storage::Cancelled&){stopped=true;}
        Check(stopped&&GetFileAttributesW(cancelled.c_str())==INVALID_FILE_ATTRIBUTES,
              "Cancelled DMG conversion published an output");
        fprintf(report,"PASS: native DMG creation and raw extraction, byte-for-byte round-trip, both forks, no overwrite and cancellation.\n");
        const size_t boundary=64*1024;
        Bytes head(raw.begin(),raw.begin()+boundary);
        Bytes body(raw.begin()+boundary,raw.end());
        const auto parts=Segments(Pack({{{Zlib,0,head.size()/SectorBytes,ZlibBlock(head)},
                                         {Raw,head.size()/SectorBytes,body.size()/SectorBytes,std::move(body)}}},
                                       raw.size()/SectorBytes),65536);
        const auto path=Save(root+"\\browser.dmg",parts.first);
        Save(root+"\\browser.002.dmgpart",parts.second);
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

namespace {
void Cng(NTSTATUS code,const char* error){Check(code>=0,error);}
class TestAlgorithm {
public:
 BCRYPT_ALG_HANDLE handle=nullptr;
 TestAlgorithm(const wchar_t* id,ULONG flags=0){
  Cng(BCryptOpenAlgorithmProvider(&handle,id,nullptr,flags),"Initialize encryption fixture");
 }
 ~TestAlgorithm(){BCryptCloseAlgorithmProvider(handle,0);}
 Bytes Object()const{
  ULONG size=0,actual=0;
  Cng(BCryptGetProperty(handle,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),
                       sizeof(size),&actual,0),"Size fixture crypto object");
  return Bytes(size);
 }
};
Bytes EncryptBlock(const wchar_t* method,Bytes& key,Bytes iv,Bytes data,bool padding)
{
 TestAlgorithm algorithm(method);
 auto object=algorithm.Object();
 Cng(BCryptSetProperty(algorithm.handle,BCRYPT_CHAINING_MODE,
     reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),sizeof(BCRYPT_CHAIN_MODE_CBC),0),
     "Set fixture cipher mode");
 struct Key {BCRYPT_KEY_HANDLE h=nullptr;~Key(){if(h)BCryptDestroyKey(h);}} keyObject;
 Cng(BCryptGenerateSymmetricKey(algorithm.handle,&keyObject.h,object.data(),ULONG(object.size()),
     key.data(),ULONG(key.size()),0),"Load fixture encryption key");
 Bytes encoded(data.size()+16);
 ULONG written=0;
 Cng(BCryptEncrypt(keyObject.h,data.data(),ULONG(data.size()),nullptr,iv.data(),ULONG(iv.size()),
     encoded.data(),ULONG(encoded.size()),&written,padding?BCRYPT_BLOCK_PADDING:0),"Encrypt fixture bytes");
 encoded.resize(written);
 return encoded;
}
Bytes Envelope(const Bytes& raw,unsigned bits,bool aesWrap,const Dmg::Password& password)
{
 constexpr size_t chunk=512,record=96,begin=1536;
 constexpr uint32_t aesId=0x80000001;
 Bytes file(begin,0);
 memcpy(file.data(),"encrcdsa",8);
 P32(file.data()+8,2);P32(file.data()+12,16);P32(file.data()+16,5);
 P32(file.data()+20,aesId);P32(file.data()+24,bits);
 P32(file.data()+52,chunk);P64(file.data()+56,raw.size());P64(file.data()+64,begin);
 P32(file.data()+72,1);
 P32(file.data()+76,1);P64(file.data()+80,record);P64(file.data()+88,616);
 auto keyRecord=file.data()+record;
 Bytes salt(20,0x5b),iv(aesWrap?16:8,0),kek(24),aes(bits/8),hmac(20);
 std::fill_n(iv.begin(),8,0x7c);
 for(size_t i=0;i<aes.size();++i)aes[i]=uint8_t(0x8c+i);
 for(size_t i=0;i<hmac.size();++i)hmac[i]=uint8_t(0x34+i);
 TestAlgorithm sha(BCRYPT_SHA1_ALGORITHM,BCRYPT_ALG_HANDLE_HMAC_FLAG);
 Cng(BCryptDeriveKeyPBKDF2(sha.handle,const_cast<PUCHAR>(password.Data()),password.Size(),
     salt.data(),ULONG(salt.size()),1000,kek.data(),ULONG(kek.size()),0),"Derive fixture wrapping key");
 auto blob=aes;
 blob.insert(blob.end(),hmac.begin(),hmac.end());
 blob.insert(blob.end(),{'C','K','I','E',0});
 auto wrapped=EncryptBlock(aesWrap?BCRYPT_AES_ALGORITHM:BCRYPT_3DES_ALGORITHM,kek,iv,blob,true);
 P32(keyRecord,0x67);P32(keyRecord+8,1000);
 P32(keyRecord+12,uint32_t(salt.size()));memcpy(keyRecord+16,salt.data(),salt.size());
 P32(keyRecord+48,8);memcpy(keyRecord+52,iv.data(),8);
 P32(keyRecord+84,192);P32(keyRecord+88,aesWrap?aesId:17);P32(keyRecord+92,7);P32(keyRecord+96,6);
 P32(keyRecord+100,uint32_t(wrapped.size()));memcpy(keyRecord+104,wrapped.data(),wrapped.size());
 auto object=sha.Object();
 for(size_t offset=0;offset<raw.size();offset+=chunk){
  struct Hash {BCRYPT_HASH_HANDLE h=nullptr;~Hash(){if(h)BCryptDestroyHash(h);}} hash;
  Cng(BCryptCreateHash(sha.handle,&hash.h,object.data(),ULONG(object.size()),hmac.data(),ULONG(hmac.size()),0),
      "Initialize fixture IV synthesis");
  uint8_t block[4];P32(block,uint32_t(offset/chunk));
  Cng(BCryptHashData(hash.h,block,sizeof(block),0),"Hash fixture block number");
  Bytes digest(20);
  Cng(BCryptFinishHash(hash.h,digest.data(),ULONG(digest.size()),0),"Finish fixture IV");
  digest.resize(16);
  Bytes plain(chunk,0);
  const auto count=(std::min)(chunk,raw.size()-offset);
  memcpy(plain.data(),raw.data()+offset,count);
  auto cipherText=EncryptBlock(BCRYPT_AES_ALGORITHM,aes,std::move(digest),std::move(plain),false);
  file.insert(file.end(),cipherText.begin(),cipherText.end());
 }
 return file;
}
Bytes LegacyEnvelope(const Bytes& plain,const Dmg::Password& password)
{
    auto modern=Envelope(plain,128,false,password);
    Bytes file(modern.begin()+1536,modern.end());
    Bytes trailer(1276),salt(20,0x5b),kek(24),aes(16),hmac(20);
    for(size_t i=0;i<aes.size();++i)aes[i]=uint8_t(0x8c+i);
    for(size_t i=0;i<hmac.size();++i)hmac[i]=uint8_t(0x34+i);
    TestAlgorithm sha(BCRYPT_SHA1_ALGORITHM,BCRYPT_ALG_HANDLE_HMAC_FLAG);
    Cng(BCryptDeriveKeyPBKDF2(sha.handle,const_cast<PUCHAR>(password.Data()),password.Size(),
         salt.data(),ULONG(salt.size()),1000,kek.data(),ULONG(kek.size()),0),"Derive legacy DMG fixture key");
    const auto putKey=[&](size_t at,const Bytes& key){
        Bytes iv(8,0x92),data(4);
        data.insert(data.end(),key.begin(),key.end());
        const auto inner=EncryptBlock(BCRYPT_3DES_ALGORITHM,kek,iv,std::move(data),true);
        iv.insert(iv.end(),inner.begin(),inner.end());
        std::reverse(iv.begin(),iv.end());
        const auto wrapped=EncryptBlock(BCRYPT_3DES_ALGORITHM,kek,{0x4a,0xdd,0xa2,0x2c,0x79,0xe8,0x21,0x05},
                                        std::move(iv),true);
        P32(trailer.data()+at,uint32_t(wrapped.size()));
        memcpy(trailer.data()+at+4,wrapped.data(),wrapped.size());
    };
    P32(trailer.data()+16,512);P32(trailer.data()+20,17);P32(trailer.data()+24,7);
    P32(trailer.data()+28,6);P32(trailer.data()+32,192);P32(trailer.data()+36,8);
    P32(trailer.data()+40,0x67);P64(trailer.data()+44,1000);P32(trailer.data()+52,uint32_t(salt.size()));
    memcpy(trailer.data()+56,salt.data(),salt.size());
    P32(trailer.data()+88,16);P32(trailer.data()+92,5);P32(trailer.data()+96,0x80000001);
    P32(trailer.data()+100,128);
    putKey(136,aes);putKey(436,hmac);
    P64(trailer.data()+1256,plain.size());P32(trailer.data()+1264,1);
    memcpy(trailer.data()+1268,"cdsaencr",8);
    file.insert(file.end(),trailer.begin(),trailer.end());
    return file;
}
std::wstring EncryptedBundle(const std::string& path,const Bytes& raw,unsigned bits,bool aesWrap,
                             const Dmg::Password& password)
{
 const auto wide=VolumeCoord::Wide(path);
 Check(CreateDirectoryW(wide.c_str(),nullptr)&&CreateDirectoryW((wide+L"\\bands").c_str(),nullptr),
       "Create encrypted bundle fixture directory");
 constexpr size_t band=1024*1024;
 const auto plist=std::string("<plist><dict><key>bundle-backingstore-version</key><integer>1</integer>"
                  "<key>diskimage-bundle-type</key><string>com.apple.diskimage.sparsebundle</string>"
                  "<key>band-size</key><integer>1048576</integer><key>size</key><integer>")+
                  std::to_string(raw.size())+"</integer></dict></plist>";
 Save(path+"\\Info.plist",Bytes(plist.begin(),plist.end()));
 Save(path+"\\token",Envelope({},bits,aesWrap,password));
 for(size_t at=0;at<raw.size();at+=band){
  const auto first=raw.begin()+at,last=first+(std::min)(band,raw.size()-at);
  const auto nonzero=std::find_if(std::make_reverse_iterator(last),std::make_reverse_iterator(first),
                                 [](auto n){return n!=0;});
  if(nonzero.base()==first)continue; // Missing bands represent logical zeros.
  const auto packed=Envelope(Bytes(first,nonzero.base()),bits,aesWrap,password);
  char name[17]{};
  sprintf_s(name,"%llx",static_cast<unsigned long long>(at/band));
  Save(path+"\\bands\\"+name,Bytes(packed.begin()+1536,packed.end()));
 }
 return wide;
}

Bytes SparseImageFixture(const Bytes& raw)
{
 constexpr size_t header=4096,band=1024*1024;
 Bytes sparse(header);
 memcpy(sparse.data(),"sprs",4);
 P32(sparse.data()+4,3);P32(sparse.data()+8,band/512);
 P32(sparse.data()+16,uint32_t(raw.size()/512));P64(sparse.data()+28,raw.size()/512);
 size_t slot=0;
 for(size_t offset=0;offset<raw.size();offset+=band){
  const auto first=raw.begin()+offset,last=first+(std::min)(band,raw.size()-offset);
  if(std::all_of(first,last,[](uint8_t value){return value==0;}))continue;
  P32(sparse.data()+64+slot++*4,uint32_t(offset/band+1));
  sparse.insert(sparse.end(),first,last);
 }
 return sparse;
}

void VerifyRawDigest(Source& source,uint64_t length,const uint8_t (&expected)[32])
{
 Check(source.Size()==length,"Unexpected native encrypted fixture geometry");
 auto plain=Disk::Read(source,0,size_t(length));
 TestAlgorithm algorithm(BCRYPT_SHA256_ALGORITHM);
 auto memory=algorithm.Object();
 struct Hash {BCRYPT_HASH_HANDLE h=nullptr;~Hash(){if(h)BCryptDestroyHash(h);}} hash;
 Cng(BCryptCreateHash(algorithm.handle,&hash.h,memory.data(),ULONG(memory.size()),nullptr,0,0),"Load fixture SHA-256");
 Cng(BCryptHashData(hash.h,plain.data(),ULONG(plain.size()),0),"Hash native image plaintext");
 uint8_t digest[32];
 Cng(BCryptFinishHash(hash.h,digest,sizeof(digest),0),"Finish native image SHA-256");
 Check(!memcmp(digest,expected,sizeof(expected)),"Native encrypted image plaintext differs from hdiutil");
}

void VerifyCertificateFixture(const std::wstring& source,const std::wstring& output,
                              const Dmg::PrivateKey* key,const Dmg::Password* password=nullptr)
{
 Storage::Job job;
 const auto result=Dmg::Convert(source,output,Dmg::Output::RawImage,job,password,key);
 Check(!result.code,result.message.c_str());
 auto file=HfsPlus::OpenImage(output.c_str(),false);
 const uint8_t expected[]={
     0x2d,0x0e,0xae,0x24,0x7e,0xef,0x9f,0x48,0xfa,0x16,0x66,0x6e,0xe5,0x49,0x72,0x4f,
     0x54,0xef,0x65,0x38,0x93,0x3b,0x0e,0xb2,0x82,0x8c,0xc6,0xd3,0xe9,0x5a,0x8a,0x17};
 VerifyRawDigest(*file,196608,expected);
}

void NativeCertificates(const std::wstring& base,const std::wstring& root,FILE* report)
{
 const Dmg::PrivateKey pem(base+L"\\dmg-cert-test-key-2048.pem");
 const Dmg::PrivateKey wrong(base+L"\\dmg-cert-test-key-4096.pem");
 const auto only=base+L"\\dmg-cert-only-udzo-aes256.dmg";
 const auto raw=root+L"\\certificate-pem.raw";
 {
  Storage::Job job;
  bool retry=false;
  try{Dmg::Convert(only,raw,Dmg::Output::RawImage,job,nullptr,&wrong);}
  catch(const Dmg::PasswordRequired& prompt){retry=prompt.retry&&prompt.method==Dmg::UnlockMethod::PrivateKey;}
  Check(retry&&GetFileAttributesW(raw.c_str())==INVALID_FILE_ATTRIBUTES,
        "Wrong RSA key produced output or failed to request another key");
 }
 VerifyCertificateFixture(only,raw,&pem);
 const auto mixed=base+L"\\dmg-cert-udzo-aes128.dmg";
 const Dmg::Password password(L"ewfprobe-test-password");
 VerifyCertificateFixture(mixed,root+L"\\certificate-mixed-key.raw",&pem);
 VerifyCertificateFixture(mixed,root+L"\\certificate-mixed-password.raw",nullptr,&password);
 VerifyCertificateFixture(base+L"\\dmg-cert4096-sparse-aes256.sparseimage",
                          root+L"\\certificate-4096.raw",&wrong);
 const auto pfx=base+L"\\dmg-cert-test-ephemeral.pfx";
 const Dmg::Password pfxPassword(L"fixture-pfx-password");
 bool needsPassword=false;
 try{const Dmg::PrivateKey denied(pfx);}
 catch(const Dmg::KeyFilePasswordRequired&){needsPassword=true;}
 Check(needsPassword,"Password-protected PKCS#12 did not request its password");
 const Dmg::PrivateKey imported(pfx,&pfxPassword);
 VerifyCertificateFixture(only,root+L"\\certificate-pfx.raw",&imported);
 struct Reset {~Reset(){Dmg::UseCertificateStoreForTest(nullptr);}} reset;
 Dmg::UseCertificateStoreForTest(&imported);
 VerifyCertificateFixture(only,root+L"\\certificate-store.raw",nullptr);
 fprintf(report,"PASS: macOS hdiutil certificate DMGs and RSA-4096 sparseimage match the published raw SHA-256 by PEM, PFX, automatic certificate-store lookup and password where present.\n");
}

void CertificateRecordTest(const std::string& root,const Dmg::Password& password)
{
 struct Key {
  HCRYPTPROV provider=0;
  HCRYPTKEY key=0;
  ~Key(){if(key)CryptDestroyKey(key);if(provider)CryptReleaseContext(provider,0);}
 } identity;
 Check(CryptAcquireContextW(&identity.provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT|CRYPT_SILENT)&&
       CryptGenKey(identity.provider,AT_KEYEXCHANGE,(2048u<<16)|CRYPT_EXPORTABLE,&identity.key),
       "Create an ephemeral RSA test identity");
 const auto encode=[&](DWORD type,LPCSTR derType){
  DWORD size=0;
  Check(CryptExportKey(identity.key,0,type,0,nullptr,&size)!=FALSE,"Size the test RSA key");
  Bytes blob(size);
  Check(CryptExportKey(identity.key,0,type,0,blob.data(),&size)&&
        CryptEncodeObject(X509_ASN_ENCODING,derType,blob.data(),nullptr,&size),"Encode test RSA key");
  Bytes out(size);
  Check(CryptEncodeObject(X509_ASN_ENCODING,derType,blob.data(),out.data(),&size)!=FALSE,"Save test DER key");
  return out;
 };
 const auto der=Save(root+"\\generated-rsa.der",encode(PRIVATEKEYBLOB,PKCS_RSA_PRIVATE_KEY));
 const auto publicKey=encode(PUBLICKEYBLOB,RSA_CSP_PUBLICKEYBLOB);
 uint8_t id[20]{};
 DWORD idBytes=sizeof(id);
 Check(CryptHashCertificate(0,CALG_SHA1,0,publicKey.data(),DWORD(publicKey.size()),id,&idBytes)&&
       idBytes==sizeof(id),"Hash the test RSA public key");
 Bytes keys(256);
 for(unsigned i=0;i<16;++i)keys[i]=uint8_t(0x8c+i);
 for(unsigned i=0;i<20;++i)keys[16+i]=uint8_t(0x34+i);
 memcpy(keys.data()+36,"CKIE\0",5);
 DWORD keyBytes=41;
 Check(CryptEncrypt(identity.key,0,TRUE,0,keys.data(),&keyBytes,DWORD(keys.size()))&&keyBytes==keys.size(),
       "Wrap the test DMG payload keys with RSA");
 std::reverse(keys.begin(),keys.end());
 const Bytes expected(1024,'Q');
 auto image=Envelope(expected,128,true,password);
 P32(image.data()+76,2);
 constexpr size_t at=96;
 P64(image.data()+88,52+keys.size());
 P32(image.data()+at,20);memcpy(image.data()+at+4,id,sizeof(id));
 P32(image.data()+at+36,42);P32(image.data()+at+40,10);P32(image.data()+at+44,0);
 P32(image.data()+at+48,uint32_t(keys.size()));memcpy(image.data()+at+52,keys.data(),keys.size());

 const auto fixture=Save(root+"\\certificate-record.dmg",image);
 const Dmg::PrivateKey imported(der);
 auto source=HfsPlus::OpenImage(fixture.c_str(),false);
 bool needsKey=false;
 try{Dmg::Unlock(source,nullptr);}
 catch(const Dmg::PasswordRequired& prompt){needsKey=!prompt.retry&&prompt.method==Dmg::UnlockMethod::PrivateKey;}
 Check(needsKey,"Certificate record did not request a private key");
 Check(Dmg::Unlock(source,nullptr,&imported)&&!source->Writable()&&source->ProcessLocal()&&
       source->WorkerFiles().empty()&&Disk::Read(*source,0,expected.size())==expected,
       "DER private key did not unlock the generated certificate image");
 P32(image.data()+at+48,4096);
 const auto corrupt=Save(root+"\\certificate-overrun.dmg",image);
 Reject([&]{auto invalid=HfsPlus::OpenImage(corrupt.c_str(),false);Dmg::Unlock(invalid,nullptr,&imported);},
        "Invalid certificate extent was accepted");
}

void VerifyNativeBundle(const std::wstring& path)
{
 const Dmg::Password password(L"ewfprobe-test-password");
 const auto bundle=SparseDisk::OpenBundle(path,&password);
 Check(bool(bundle),"Cannot open native encrypted bundle");
 const uint8_t expected[]={0x34,0xda,0x28,0x88,0x83,0xb1,0x9c,0xd6,0x73,0x92,0x2b,0x26,0x74,0xd8,0xa2,0x5f,
     0xaf,0x1d,0x07,0xe7,0xdb,0xd4,0x33,0x52,0x40,0x71,0x8c,0x1c,0x70,0x44,0xea,0x3a};
 VerifyRawDigest(*bundle,2129920,expected);
}

void NeedsPassword(std::unique_ptr<Source>& file,const Dmg::Password* password,bool retry)
{
 try{Dmg::Unlock(file,password);}
 catch(const Dmg::PasswordRequired& required){
  Check(required.retry==retry&&!file->ProcessLocal(),"Wrong password state or source changed after a failed unlock");
  return;
 }
 throw HfsPlus::Error("Encrypted image did not request the password");
}

void VerifySparseConversion(const std::wstring& path,const std::wstring& output,const Dmg::Password& password)
{
 const Dmg::Password wrong(L"incorrect");
 Storage::Job denied;
 bool retry=false;
 try{Dmg::Convert(path,output,Dmg::Output::RawImage,denied,&wrong);}
 catch(const Dmg::PasswordRequired& required){retry=required.retry;}
 Check(retry&&GetFileAttributesW(output.c_str())==INVALID_FILE_ATTRIBUTES,
       "Wrong password left a raw output or did not request a retry");
 Storage::Job conversion;
 const auto done=Dmg::Convert(path,output,Dmg::Output::RawImage,conversion,&password);
 Check(!done.code,done.message.c_str());
}

void VerifyNativeSparseImage(const std::wstring& path,const std::wstring& output)
{
 const Dmg::Password password(L"ewfprobe-test-password");
 VerifySparseConversion(path,output,password);
 auto raw=HfsPlus::OpenImage(output.c_str(),false);
 const uint8_t expected[]={0x2d,0x0e,0xae,0x24,0x7e,0xef,0x9f,0x48,0xfa,0x16,0x66,0x6e,0xe5,0x49,0x72,0x4f,
     0x54,0xef,0x65,0x38,0x93,0x3b,0x0e,0xb2,0x82,0x8c,0xc6,0xd3,0xe9,0x5a,0x8a,0x17};
 VerifyRawDigest(*raw,196608,expected);
}

CString testHost;
class EncryptedDmgDoc:public CHFVExplorerDoc{
 DECLARE_DYNCREATE(EncryptedDmgDoc)
public:
 EncryptedDmgDoc():CHFVExplorerDoc(false){Mounts().AddWindows(testHost[0],DRIVE_FIXED,testHost);}
};
IMPLEMENT_DYNCREATE(EncryptedDmgDoc,CHFVExplorerDoc)
class EncryptedDmgFrame:public CMainFrame{
 DECLARE_DYNCREATE(EncryptedDmgFrame)
public:
 void LoadWindowPosition()override{}
 void SaveWindowPosition()override{}
 void ActivateFrame(int)override{}
};
IMPLEMENT_DYNCREATE(EncryptedDmgFrame,CMainFrame)

void BrowserPrompt(const CString& root,std::initializer_list<std::wstring> paths,
                   const std::shared_ptr<const Dmg::Password>& password)
{
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 app.m_startup_hds=app.m_startup_floppies=app.m_startup_cds=FALSE;
 app.m_dosicons=DI_NO_ICONS;
 app.m_mac_window_mode=FALSE;
 testHost=root;
 app.InstallBrowserTemplate(RUNTIME_CLASS(EncryptedDmgDoc),RUNTIME_CLASS(EncryptedDmgFrame));
 auto frame=app.CreateBrowser(FALSE);
 Check(frame!=nullptr,"Create the hidden image browser");
 struct Close {CMainFrame* frame;~Close(){if(frame->GetSafeHwnd())frame->DestroyWindow();}} close{frame};
 auto& session=frame->GetBrowserSession();
 Check(session.AwaitNavigationForTest()&&session.ListingAvailable(),"Open the initial Windows folder");
 auto& browser=session.MacBrowser();
 for(const auto& path:paths){
  const auto priorTitle=session.WindowTitle();
  unsigned prompts=0;
  browser.SetPasswordPromptForTest([&](bool retry)->std::shared_ptr<const Dmg::Password>{
   Check(!retry,"Initial encrypted image prompt was a retry");
   ++prompts;
   return {};
  });
  Check(browser.OpenImage(path)==1&&session.AwaitNavigationForTest()&&prompts==1&&
        session.ListingAvailable()&&session.WindowTitle()==priorTitle,"Cancelled password changed the browser listing");
  browser.SetPasswordPromptForTest([&](bool retry)->std::shared_ptr<const Dmg::Password>{
   ++prompts;
   Check(retry==(prompts==3),"Unexpected password retry state");
   if(!retry)return std::make_shared<Dmg::Password>(L"wrong");
   return password;
  });
  Check(browser.OpenImage(path)==1&&session.AwaitNavigationForTest()&&prompts==3&&
        browser.Active()&&session.ListingAvailable()&&!browser.Writable()&&
        session.WindowTitle().find(L"Encrypted fixture")!=std::wstring::npos,
        "Encrypted image did not mount after a wrong password followed by the right one");
  browser.SetPasswordPromptForTest({});
 }
}
}

int RunEncryptedDmgTest(const char* directory)
{
 if(!CreateDirectoryA(directory,nullptr))return 2;
 const std::string root=directory;
 FILE* report=fopen((root+"\\encrypted-dmg-report.txt").c_str(),"w");
 if(!report)return 2;
 int status=0;
 InitializeStorageUi();
 try{
  const auto password=std::make_shared<const Dmg::Password>(L"fixture \u03a9");
  const Dmg::Password wrong(L"incorrect");
  CertificateRecordTest(root,*password);
  fprintf(report,"PASS: generated certificate record, DER private key, bounds checks and read-only local decryption.\n");
  if(const auto fixtures=_wgetenv(L"HFV_CERT_DMG_FIXTURES"))
   NativeCertificates(fixtures,VolumeCoord::Wide(root),report);
  Bytes plain(2601);
  for(size_t i=0;i<plain.size();++i)plain[i]=uint8_t(i*113+i/512);
  for(const auto aesWrap:{false,true}){
   const auto path=Save(root+(aesWrap?"\\aes.dmg":"\\3des.dmg"),
                        Envelope(plain,aesWrap?256:128,aesWrap,*password));
   auto source=HfsPlus::OpenImage(path.c_str(),false);
   NeedsPassword(source,nullptr,false);
   NeedsPassword(source,&wrong,true);
   Check(Dmg::Unlock(source,password.get())&&source->ProcessLocal()&&source->Size()==plain.size()&&
         !source->Writable()&&source->WorkerFiles().empty(),"Unlock fixture in place without worker handles");
   Check(Disk::Read(*source,499,2099)==Bytes(plain.begin()+499,plain.begin()+2598)&&
         Disk::Read(*source,0,plain.size())==plain,"Encrypted DMG random or partial block reads differ");
   Reject([&]{source->Write(0,plain.data(),1);},"Unlocked DMG accepted a write");
   Reject([&]{Disk::Read(*source,plain.size()-5,10);},"Unlocked DMG allowed reading past plaintext size");
  }
  const auto old=Save(root+"\\version-1.dmg",LegacyEnvelope(plain,*password));
  auto legacy=HfsPlus::OpenImage(old.c_str(),false);
  NeedsPassword(legacy,nullptr,false);
  NeedsPassword(legacy,&wrong,true);
  Check(Dmg::Unlock(legacy,password.get())&&legacy->ProcessLocal()&&
        Disk::Read(*legacy,0,plain.size())==plain,"Read a legacy v1 encrypted DMG");
  fprintf(report,"PASS: v1 trailing encryption header, 3DES key unwrap, wrong password and partial payload block.\n");
  auto corrupt=Envelope(plain,128,true,*password);
  P64(corrupt.data()+80,corrupt.size());
  const auto badRecord=Save(root+"\\bad-record.dmg",corrupt);
  Reject([&]{auto file=HfsPlus::OpenImage(badRecord.c_str(),false);Dmg::Unlock(file,password.get());},
         "Out-of-bounds password record was accepted");
  fprintf(report,"PASS: Password-required and wrong-password responses, Unicode passwords, AES-128/AES-256, AES/3DES wrapped keys, partial blocks, bounds and read-only access.\n");
  fflush(report);

  wchar_t nativePath[32768]{};
  const auto length=GetEnvironmentVariableW(L"HFV_ENCRYPTED_DMG_FIXTURE",nativePath,ARRAYSIZE(nativePath));
  if(length&&length<ARRAYSIZE(nativePath)){
   auto source=HfsPlus::OpenImage(nativePath,false);
   const Dmg::Password publishedFixturePassword(L"hunter2");
   NeedsPassword(source,&wrong,true);
   Check(Dmg::Unlock(source,&publishedFixturePassword)&&source->Size()==65536,"Unlock genuine macOS encrypted fixture");
   const auto boot=Disk::Read(*source,0,512);
   Check(boot[510]==0x55&&boot[511]==0xaa&&!memcmp(boot.data()+54,"FAT12",5),"Unexpected authentic DMG plaintext");
   fprintf(report,"PASS: macOS hdiutil AES-128 encrypted fixture and wrong-password rejection.\n");
   fflush(report);
  }

  auto rawPath=VolumeCoord::Wide(root+"\\plus.img");
  const Bytes contents={'e','n','c','r','y','p','t','e','d'};
  Bytes raw;
  {
   auto image=CreateImage(rawPath.c_str(),8*1024*1024);
   Volume::Format(*image,u"Encrypted fixture",true);
   Volume volume(*image,true);
   volume.Put(2,u"Payload",contents.size(),1,[&](unsigned fork,uint64_t at,void* p,size_t n){
    if(fork)memset(p,0x4d,n);else memcpy(p,contents.data()+at,n);
   });
   raw=Disk::Read(*image,0,size_t(image->Size()));
  }
  const auto packed=Pack({{{Zlib,0,raw.size()/SectorBytes,ZlibBlock(raw)}}},raw.size()/SectorBytes);
  const auto path=Save(root+"\\compressed encrypted.dmg",Envelope(packed,256,true,*password));
  const auto bundle=EncryptedBundle(root+"\\encrypted.sparsebundle",raw,256,true,*password);
  const auto sparsePath=Save(root+"\\encrypted.sparseimage",Envelope(SparseImageFixture(raw),128,true,*password));
  BrowserPrompt(root.c_str(),{path,bundle,sparsePath},password);
  if(const auto nativeSparse=_wgetenv(L"HFV_ENCRYPTED_SPARSE_IMAGE_FIXTURE")){
   VerifyNativeSparseImage(nativeSparse,VolumeCoord::Wide(root+"\\native-sparseimage.raw"));
   fprintf(report,"PASS: macOS hdiutil AES-128 sparseimage raw conversion matches the published SHA-256.\n");
  }

  constexpr size_t band=1024*1024;
  Bytes bands(2*band+1024);
  std::fill_n(bands.begin(),513,'A');
  std::fill_n(bands.begin()+2*band,11,'C');
  for(const auto aesWrap:{false,true}){
   const auto sparse=EncryptedBundle(root+(aesWrap?"\\aes.sparsebundle":"\\3des.sparsebundle"),
                                     bands,aesWrap?256:128,aesWrap,*password);
   try{
    SparseDisk::OpenBundle(sparse,&wrong);
    throw Error("Wrong password unlocked an encrypted bundle");
   }catch(const Dmg::PasswordRequired& required){Check(required.retry,"Expected encrypted bundle password retry");}
   auto unlocked=SparseDisk::OpenBundle(sparse,password.get());
   Check(unlocked&&!unlocked->Writable()&&unlocked->ProcessLocal()&&unlocked->WorkerFiles().empty(),
         "Encrypted sparse bundle did not keep keys and plaintext local");
   Check(Disk::Read(*unlocked,0,3)==Bytes(3,'A')&&Disk::Read(*unlocked,2*band,3)==Bytes(3,'C')&&
         Disk::Read(*unlocked,0,3)==Bytes(3,'A')&&Disk::Read(*unlocked,0,bands.size())==bands,
         "Encrypted bundle changed bytes across short bands, holes, or repeated block numbers");
   Reject([&]{unlocked->Write(0,bands.data(),1);},"Encrypted bundle accepted a write");
  }
  const auto corruptBundle=EncryptedBundle(root+"\\truncated.sparsebundle",bands,128,true,*password);
  const auto corruptBand=corruptBundle+L"\\bands\\0";
  const auto oldBand=HfsPlus::OpenImage(corruptBand.c_str(),false)->Size();
  const auto handle=CreateFileW(corruptBand.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
  Check(handle!=INVALID_HANDLE_VALUE,"Open encrypted band for truncation fixture");
  LARGE_INTEGER end{};
  end.QuadPart=oldBand-1;
  const bool truncated=SetFilePointerEx(handle,end,nullptr,FILE_BEGIN)&&SetEndOfFile(handle);
  CloseHandle(handle);
  Check(truncated,"Truncate encrypted band fixture");
  Reject([&]{SparseDisk::OpenBundle(corruptBundle,password.get());},"Encrypted bundle accepted a partial ciphertext block");
  if(const auto nativeBundle=_wgetenv(L"HFV_ENCRYPTED_BUNDLE_FIXTURE")){
   VerifyNativeBundle(nativeBundle);
   fprintf(report,"PASS: macOS hdiutil AES-256 sparse bundle matches the published raw SHA-256.\n");
  }
  for(const auto& sparse:{bundle,sparsePath}){
   const auto converted=VolumeCoord::Wide(root+(sparse==bundle?"\\bundle.raw":"\\sparseimage.raw"));
   VerifySparseConversion(sparse,converted,*password);
   auto result=HfsPlus::OpenImage(converted.c_str(),false);
   Check(result->Size()==raw.size()&&Disk::Read(*result,0,raw.size())==raw,"Encrypted sparse raw conversion changed sectors");
  }
  auto invalidSparse=SparseImageFixture(raw);
  P32(invalidSparse.data()+64,0xffffffff);
  const auto damaged=Save(root+"\\malformed.sparseimage",Envelope(invalidSparse,128,true,*password));
  const auto staged=VolumeCoord::Wide(root+"\\malformed.raw");
  Storage::Job invalidJob;
  bool rejected=false;
  try{rejected=Dmg::Convert(damaged,staged,Dmg::Output::RawImage,invalidJob,password.get()).code!=0;}
  catch(const Error&){rejected=true;}
  Check(rejected&&GetFileAttributesW(staged.c_str())==INVALID_FILE_ATTRIBUTES,
        "Invalid decrypted sparse map published a raw image");
  fprintf(report,"PASS: password-encrypted sparseimage browsing, raw conversion and rejection of a damaged inner band map.\n");
  fprintf(report,"PASS: encrypted bundle password retry, per-band CBC, holes, short bands, ciphertext bounds and raw conversion.\n");
  const auto extracted=VolumeCoord::Wide(root+"\\decrypted.img");
  Storage::Job convert;
  Check(!Dmg::Convert(path,extracted,Dmg::Output::RawImage,convert,password.get()).code,
        "Convert encrypted DMG into a raw image");
  auto decrypted=HfsPlus::OpenImage(extracted.c_str(),false);
  Check(decrypted->Size()==raw.size()&&Disk::Read(*decrypted,0,raw.size())==raw,
        "Raw conversion changed encrypted image data");
  for(const auto& image:{path,bundle,sparsePath}){
   const auto volume=MacBrowser::OpenBrowserImage(image,{},password.get());
   MacBrowser::Entry entry;
   Check(volume&&volume->FindChild(2,u"Payload",entry)&&!volume->Writable()&&
         MacBrowser::ExecutionFor(*volume).processLocal,"Open HFS+ from each encrypted image format");
   const auto to=VolumeCoord::Wide(root+(image==path?"\\export-dmg":image==bundle?"\\export-bundle":"\\export-sparseimage"));
   Check(CreateDirectoryW(to.c_str(),nullptr)!=FALSE,"Create encrypted export directory");
   const auto source=FileOperations::Location::Mounted(volume,entry.id);
   FileOperations::Request request=FileOperations::OrdinaryCopy{{source},FileOperations::Location::Windows(to)};
   request.background=true;
   CopyProcess::enabled=true;
   CopyProcess::lastProcess=0;
   Check(!FileOperations::InspectPlanForTest(request,volume->ResourceKey()).temporaryWorker,
         "Encrypted image selected a temporary worker");
   const auto op=FileOperations::Submit(std::move(request));
   const auto start=GetTickCount();
   while(!op.Job()->done()){
    Check(GetTickCount()-start<15000,"Encrypted image export timed out");
    Sleep(5);
   }
   const auto copied=op.Job()->result();
   Check(!copied.code,copied.message.c_str());
   Check(!CopyProcess::lastProcess&&op.Snapshot().complete,"Encrypted copy crossed the subprocess boundary");
   const auto windows=MacBrowser::OpenWindows(to);
   Check(windows->FindChild(2,u"Payload",entry)&&entry.forks[0].size==contents.size()&&entry.forks[1].size==1,
         "Encrypted image export lost a fork");
   Bytes output(contents.size());windows->Read(entry.id,0,0,output.data(),output.size());
   uint8_t resource=0;windows->Read(entry.id,1,0,&resource,1);
   Check(output==contents&&resource==0x4d,"Encrypted image export changed file contents");
  }
  fprintf(report,"PASS: Cancel and wrong-password retry, and both-fork background export without transferring keys from any tested encrypted image format.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());status=1;}
 CopyProcess::enabled=false;
 ShutdownStorageUi();
 fclose(report);
 return status;
}

#endif
