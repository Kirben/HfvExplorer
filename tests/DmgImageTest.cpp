// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "DmgImage.h"
#include "DmgEncryption.h"
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
void NeedsPassword(std::unique_ptr<Source>& file,const Dmg::Password* password,bool retry)
{
 try{Dmg::Unlock(file,password);}
 catch(const Dmg::PasswordRequired& required){
  Check(required.retry==retry&&!file->ProcessLocal(),"Wrong password state or source changed after a failed unlock");
  return;
 }
 throw HfsPlus::Error("Encrypted image did not request the password");
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

void BrowserPrompt(const CString& root,const std::wstring& path,
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
 unsigned prompts=0;
 browser.SetPasswordPromptForTest([&](bool retry)->std::shared_ptr<const Dmg::Password>{
  Check(!retry,"Initial encrypted DMG prompt was a retry");
  ++prompts;
  return {};
 });
 Check(browser.OpenImage(path)==1&&session.AwaitNavigationForTest()&&prompts==1&&
       session.ListingAvailable()&&browser.TargetFolder().IsWindows(),"Cancelled password changed the browser listing");
 browser.SetPasswordPromptForTest([&](bool retry)->std::shared_ptr<const Dmg::Password>{
  ++prompts;
  Check(retry==(prompts==3),"Unexpected password retry state");
  if(!retry)return std::make_shared<Dmg::Password>(L"wrong");
  return password;
 });
 Check(browser.OpenImage(path)==1&&session.AwaitNavigationForTest()&&prompts==3&&
       browser.Active()&&session.ListingAvailable()&&!browser.Writable()&&
       session.WindowTitle().find(L"Encrypted fixture")!=std::wstring::npos,
       "Encrypted DMG did not mount after a wrong password followed by the right one");
 browser.SetPasswordPromptForTest({});
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
  BrowserPrompt(root.c_str(),path,password);
  const auto volume=MacBrowser::OpenBrowserImage(path,{},password.get());
  MacBrowser::Entry entry;
  Check(volume&&volume->FindChild(2,u"Payload",entry)&&!volume->Writable()&&
        MacBrowser::ExecutionFor(*volume).processLocal,"Open encrypted and compressed HFS+ in the browser");
  const auto to=VolumeCoord::Wide(root+"\\export");
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
   Check(GetTickCount()-start<15000,"Encrypted DMG export timed out");
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
  fprintf(report,"PASS: Cancel and wrong-password retry in the normal image-opening path, layered encryption and compression, and both-fork background export without transferring keys.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());status=1;}
 CopyProcess::enabled=false;
 ShutdownStorageUi();
 fclose(report);
 return status;
}

#endif
