// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "SparseDisk.h"
#include "HfsPlusDisk.h"
#include "DmgImage.h"
#include "MacBrowserModel.h"
#include "FileOperations.h"
#include "CopyProcess.h"
#include "StorageWorker.h"
#include "VolumeCoordinator.h"
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")

namespace {
using namespace HfsPlus;
using namespace HfsPlus::Disk;
constexpr size_t Header=4096;

template<class F> void Reject(F action,const char* wrong)
{
    try{action();}catch(const Error&){return;}
    throw Error(wrong);
}

std::unique_ptr<NewImage> Create(const std::wstring& name,const Bytes& bytes)
{
    auto out=CreateFormatOutput(name.c_str(),bytes.size());
    out->Write(0,bytes.data(),bytes.size());
    out->Keep();
    return out;
}

std::unique_ptr<Source> OpenSparse(const std::wstring& name,bool write=false)
{
    auto file=HfsPlus::OpenImage(name.c_str(),write);
    Check(SparseDisk::Unwrap(file),"Sparse image signature was missed");
    return file;
}

Bytes Head(uint32_t perBand,uint64_t sectors)
{
    Bytes h(Header);
    memcpy(h.data(),"sprs",4);P32(h.data()+4,3);P32(h.data()+8,perBand);
    P32(h.data()+16,uint32_t(sectors));P64(h.data()+28,sectors);
    return h;
}

void WriteHeader(NewImage& output,const Bytes& bytes)
{
    output.Write(0,bytes.data(),Header);
}

void VerifyNative(const wchar_t* name,const std::wstring& output)
{
    Storage::Job job;
    const auto result=Dmg::Convert(name,output,Dmg::Output::RawImage,job);
    Check(!result.code,result.message.c_str());
    const auto raw=HfsPlus::OpenImage(output.c_str(),false);
    Check(raw->Size()==3145728,"Native sparse fixture has incorrect logical size");
    const auto bytes=Disk::Read(*raw,0,size_t(raw->Size()));
    BCRYPT_ALG_HANDLE alg=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    Check(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"Cannot open SHA-256");
    uint8_t resultBytes[32];
    const bool valid=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0&&
        BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)>=0&&
        BCryptFinishHash(hash,resultBytes,sizeof(resultBytes),0)>=0;
    if(hash)BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg,0);
    Check(valid,"Cannot compute sparse fixture SHA-256");
    const uint8_t expected[]={
        0x77,0x0b,0xe7,0x32,0xaa,0xfc,0x49,0x62,0xc6,0x94,0x0a,0x20,0x76,0xc3,0x54,0x71,
        0x41,0x96,0x21,0xd9,0xd6,0xca,0x57,0xcd,0xa6,0x0c,0xbd,0x6e,0x82,0xa8,0xb3,0x6f};
    Check(!memcmp(resultBytes,expected,sizeof(expected)),"Native sparse fixture differs from its reference raw disk");
}

void SparseImageCases(const std::wstring& root,FILE* report)
{
    const auto sample=root+L"chained.sparseimage";
    constexpr size_t next=Header+2*1024;
    auto head=Head(2,7);
    P64(head.data()+20,next);
    P32(head.data()+64,4); // Last band, one sector long.
    P32(head.data()+68,1); // Out of logical order.
    Bytes image(next+Header+1024);
    memcpy(image.data(),head.data(),Header);
    std::fill_n(image.data()+Header,512,'D');
    std::fill_n(image.data()+Header+1024,1024,'A');
    auto continuation=image.data()+next;
    memcpy(continuation,"sprs",4);
    P32(continuation+56,3);
    std::fill_n(continuation+Header,1024,'C');
    Create(sample,image).reset();
    {
        auto sparse=OpenSparse(sample,true);
        Bytes expected(3584);
        std::fill_n(expected.data(),1024,'A');
        std::fill_n(expected.data()+2048,1024,'C');
        std::fill_n(expected.data()+3072,512,'D');
        Check(!sparse->Writable()&&Disk::Read(*sparse,0,expected.size())==expected,
              "Sparse image bands or holes decoded incorrectly");
        Reject([&]{sparse->Write(0,expected.data(),1);},"Sparse image accepted a write");
        Reject([&]{Disk::Read(*sparse,expected.size()-1,2);},"Sparse image accepted a read past its end");
    }
    const auto bad=root+L"malformed.sparseimage";
    const auto rejects=[&](Bytes bytes,const char* wrong){
        if(GetFileAttributesW(bad.c_str())!=INVALID_FILE_ATTRIBUTES)
            Check(DeleteFileW(bad.c_str())!=FALSE,"Cannot replace malformed test fixture");
        Create(bad,bytes).reset();
        Reject([&]{OpenSparse(bad);},wrong);
    };
    auto altered=image;P32(altered.data()+next+56,1);
    rejects(std::move(altered),"Sparse image accepted a duplicate band");
    altered=image;P64(altered.data()+next+12,next);
    rejects(std::move(altered),"Sparse image accepted a cyclic header chain");
    altered=image;P64(altered.data()+20,Header);
    rejects(std::move(altered),"Sparse image accepted a header overlapping a band");
    altered=image;P32(altered.data()+64,5);
    rejects(std::move(altered),"Sparse image accepted an out-of-range logical band");
    altered=image;altered.pop_back();
    rejects(std::move(altered),"Sparse image accepted a truncated stored band");
    altered=image;P32(altered.data()+next+4,1);
    rejects(std::move(altered),"Sparse image accepted an invalid continuation sequence");

    const auto huge=root+L"large.sparseimage";
    auto large=Head(2048,(1ULL<<32)+512);
    P32(large.data()+64,2097153);
    Bytes last(256*1024,'Z');
    large.insert(large.end(),last.begin(),last.end());
    Create(huge,large).reset();
    auto sparse=OpenSparse(huge);
    const auto end=sparse->Size();
    const auto tail=Disk::Read(*sparse,end-last.size()-1,last.size()+1);
    Check(end==((1ULL<<32)+512)*512&&tail.front()==0&&
          std::equal(last.begin(),last.end(),tail.begin()+1),"Sparse image lost a 64-bit logical offset");
    fprintf(report,"PASS: sparse image hole and band reads, continuation headers, >2 TiB addressing and malformed-input rejection.\n");

    const auto rawPath=root+L"volume.raw";
    const auto disk=CreateImage(rawPath.c_str(),8*1024*1024);
    Volume::Format(*disk,u"Sparse Fixture",false);
    {
        Volume volume(*disk,true);
        volume.Put(2,u"Both forks",4,4,[](unsigned fork,uint64_t at,void* out,size_t bytes){
            memcpy(out,(fork?"RSRC":"DATA")+size_t(at),bytes);
        });
    }
    const auto diskBytes=Disk::Read(*disk,0,size_t(disk->Size()));
    auto mountedBytes=Head(2048,diskBytes.size()/512);
    for(unsigned i=0;i<8;++i)P32(mountedBytes.data()+64+i*4,i+1);
    mountedBytes.insert(mountedBytes.end(),diskBytes.begin(),diskBytes.end());
    const auto mount=root+L"mount.sparseimage";
    Create(mount,mountedBytes).reset();
    auto volume=MacBrowser::OpenBrowserImage(mount);
    MacBrowser::Entry entry;
    Check(volume&&!volume->Writable()&&volume->FindChild(2,u"Both forks",entry),
          "Sparse image did not open read-only through the browser");
    for(unsigned i=0;i<2;++i){
        char bytes[4];
        volume->Read(entry.id,i,0,bytes,sizeof(bytes));
        Check(!memcmp(bytes,i?"RSRC":"DATA",4),"Sparse image changed a file fork");
    }
    Storage::Job job;
    const auto extracted=root+L"extracted.raw";
    const auto result=Dmg::Convert(mount,extracted,Dmg::Output::RawImage,job);
    Check(!result.code,result.message.c_str());
    auto out=HfsPlus::OpenImage(extracted.c_str(),false);
    Check(Disk::Read(*out,0,diskBytes.size())==diskBytes,"Sparse image raw conversion changed sectors");
    fprintf(report,"PASS: sparse image browser, both forks and byte-exact raw conversion.\n");

    if(const auto fixture=_wgetenv(L"HFV_NATIVE_SPARSE_IMAGE")){
        VerifyNative(fixture,root+L"native-sparse.raw");
        fprintf(report,"PASS: original hdiutil sparse image matches the published raw SHA-256.\n");
    }
}

void Mkdir(const std::wstring& path)
{
    Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"Cannot create sparse bundle fixture directory");
}

void Plist(const std::wstring& bundle,uint64_t size,uint64_t band,const std::string& extra={})
{
    const auto xml=std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>diskimage-bundle-type</key><string>com.apple.diskimage.sparsebundle</string>
<key>bundle-backingstore-version</key><integer>1</integer>
<key>band-size</key><integer>)")+std::to_string(band)+"</integer>\n<key>size</key><integer>"+
                   std::to_string(size)+"</integer>"+extra+"</dict></plist>";
    Create(bundle+L"\\Info.plist",Bytes(xml.begin(),xml.end()));
}

std::wstring BundleFixture(const std::wstring& path,uint64_t size,uint64_t band,const std::string& extra={})
{
    Mkdir(path);
    Mkdir(path+L"\\bands");
    Plist(path,size,band,extra);
    return path;
}

void SparseBundleCases(const std::wstring& root,FILE* report)
{
    const auto basic=BundleFixture(root+L"holes.sparsebundle",11*1024+512,1024);
    auto path=basic+L"\\bands\\0";
    Create(path,Bytes(512,'A'));
    Create(basic+L"\\bands\\a",Bytes(1024,'B'));
    Create(basic+L"\\bands\\b",Bytes(512,'C'));
    Create(basic+L"\\bands\\c",Bytes(512,'X')); // Beyond the logical disk.
    auto disk=SparseDisk::OpenBundle(basic+L"\\");
    Check(disk&&!disk->Writable()&&SparseDisk::BundleFromPlist(basic+L"\\Info.plist")==basic,
          "Sparse bundle did not open read-only");
    Check(disk->Size()==11*1024+512,"Sparse bundle logical length changed");
    Bytes expected(size_t(disk->Size()),0);
    std::fill_n(expected.data(),512,'A');
    std::fill_n(expected.data()+10*1024,1024,'B');
    std::fill_n(expected.data()+11*1024,512,'C');
    Check(Disk::Read(*disk,0,expected.size())==expected,"Sparse bundle stored or zero-filled ranges differ");
    Reject([&]{disk->Write(0,expected.data(),1);},"Sparse bundle accepted a write");
    Reject([&]{Disk::Read(*disk,disk->Size()-1,2);},"Sparse bundle accepted a read past its end");
    {
        auto change=HfsPlus::OpenImage(path.c_str(),true);
        const char byte='x';
        change->Write(0,&byte,1);
        change->Flush();
    }
    const auto attrs=GetFileAttributesW(path.c_str());
    Check(attrs!=INVALID_FILE_ATTRIBUTES,"Cannot inspect changed sparse band fixture");
    const auto file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,
                               OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"Cannot change fixture timestamp");
    FILETIME modified{0x30554000,0x01d70001};
    const bool changed=SetFileTime(file,nullptr,nullptr,&modified)!=FALSE;
    CloseHandle(file);
    Check(changed,"Cannot set fixture timestamp");
    Reject([&]{Disk::Read(*disk,0,1);},"Sparse bundle accepted a changed band");

    const auto huge=BundleFixture(root+L"large.sparsebundle",(1ULL<<44)+512,1<<20);
    Create(huge+L"\\bands\\1000000",Bytes(512,'Z'));
    auto hugeDisk=SparseDisk::OpenBundle(huge);
    const auto last=Disk::Read(*hugeDisk,hugeDisk->Size()-513,513);
    Check(last.front()==0&&std::all_of(last.begin()+1,last.end(),[](auto b){return b=='Z';}),
          "Sparse bundle lost a high logical band number");

    const auto duplicate=BundleFixture(root+L"duplicate.sparsebundle",4096,1024,"<key>size</key><integer>2048</integer>");
    Reject([&]{SparseDisk::OpenBundle(duplicate);},"Sparse bundle accepted duplicate geometry fields");
    const auto unsafe=BundleFixture(root+L"unsafe.sparsebundle",4096,1024);
    Mkdir(unsafe+L"\\bands\\0");
    Reject([&]{SparseDisk::OpenBundle(unsafe);},"Sparse bundle accepted a directory as band data");
    const auto encrypted=BundleFixture(root+L"encrypted.sparsebundle",4096,1024);
    Create(encrypted+L"\\token",Bytes{'e','n','c','r','c','d','s','a'});
    Reject([&]{SparseDisk::OpenBundle(encrypted);},"Sparse bundle treated encrypted data as plain");
    const auto incomplete=root+L"incomplete.sparsebundle";
    Mkdir(incomplete);
    Plist(incomplete,4096,1024);
    Reject([&]{SparseDisk::OpenBundle(incomplete);},"Sparse bundle accepted a missing bands directory");
    fprintf(report,"PASS: sparse bundle missing and short bands, hexadecimal names, >16 TiB offsets, changed bands and invalid layouts.\n");

    const auto rawPath=root+L"volume.raw";
    const auto source=CreateImage(rawPath.c_str(),8*1024*1024);
    Volume::Format(*source,u"Bundle Fixture",false);
    {
        Volume volume(*source,true);
        volume.Put(2,u"Both forks",4,4,[](unsigned fork,uint64_t at,void* out,size_t bytes){
            memcpy(out,(fork?"RSRC":"DATA")+size_t(at),bytes);
        });
    }
    const auto raw=Disk::Read(*source,0,size_t(source->Size()));
    const auto mount=BundleFixture(root+L"image-\u03b2.sparsebundle",raw.size(),1024*1024);
    for(size_t at=0;at<raw.size();at+=1<<20){
        const auto first=raw.begin()+at,last=first+(1<<20);
        if(std::any_of(first,last,[](uint8_t byte){return byte!=0;}))
            Create(mount+L"\\bands\\"+std::to_wstring(at>>20),Bytes(first,last));
    }
    auto mounted=MacBrowser::OpenBrowserImage(mount);
    MacBrowser::Entry entry;
    Check(mounted&&!mounted->Writable()&&mounted->FindChild(2,u"Both forks",entry)&&
          MacBrowser::OpenBrowserImage(mount)==mounted,"Sparse bundle did not open in the shared browser");
    const auto destination=root+L"export";
    Mkdir(destination);
    CopyProcess::enabled=true;
    FileOperations::Request request=FileOperations::OrdinaryCopy{
        {FileOperations::Location::Mounted(mounted,entry.id)},FileOperations::Location::Windows(destination)};
    request.background=true;
    auto operation=FileOperations::Submit(std::move(request));
    const auto start=GetTickCount64();
    while(!operation.Job()->done()){
        Check(GetTickCount64()-start<15000,"Sparse bundle background copy timed out");
        Sleep(5);
    }
    const auto exported=operation.Job()->result();
    Check(!exported.code,exported.message.c_str());
    auto windows=MacBrowser::OpenWindows(destination);
    auto copied=windows->List().at(0);
    for(unsigned fork=0;fork<2;++fork){
        char bytes[4];
        Check(copied.forks[fork].size==sizeof(bytes),"Sparse bundle copy lost a fork");
        windows->Read(copied.id,fork,0,bytes,sizeof(bytes));
        Check(!memcmp(bytes,fork?"RSRC":"DATA",4),"Sparse bundle copy changed a fork");
    }
    Storage::Job job;
    const auto extracted=root+L"extracted.raw";
    const auto result=Dmg::Convert(mount,extracted,Dmg::Output::RawImage,job);
    Check(!result.code,result.message.c_str());
    auto out=HfsPlus::OpenImage(extracted.c_str(),false);
    Check(Disk::Read(*out,0,raw.size())==raw,"Sparse bundle raw conversion changed sectors");
    fprintf(report,"PASS: shared sparse bundle browser, background both-fork copy and byte-exact raw conversion.\n");

    if(const auto fixture=_wgetenv(L"HFV_NATIVE_SPARSE_BUNDLE")){
        VerifyNative(fixture,root+L"native-sparse.raw");
        fprintf(report,"PASS: original hdiutil sparse bundle matches the published raw SHA-256.\n");
    }
}
}

int RunSparseDiskTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const auto root=VolumeCoord::Wide(std::string(directory)+"\\");
    FILE* report=_wfopen((root+L"sparse-image-report.txt").c_str(),L"w");
    if(!report)return 2;
    int status=0;
    try{SparseImageCases(root,report);}
    catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());status=1;}
    fclose(report);
    return status;
}

int RunSparseBundleTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const auto root=VolumeCoord::Wide(std::string(directory)+"\\");
    FILE* report=_wfopen((root+L"sparse-bundle-report.txt").c_str(),L"w");
    if(!report)return 2;
    int status=0;
    try{SparseBundleCases(root,report);}
    catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());status=1;}
    CopyProcess::enabled=false;
    fclose(report);
    return status;
}
#endif
