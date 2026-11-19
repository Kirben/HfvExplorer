// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "SparseDisk.h"
#include "HfsPlusDisk.h"
#include "DmgImage.h"
#include "MacBrowserModel.h"
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
#endif
