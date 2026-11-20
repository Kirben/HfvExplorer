// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsCrypto.h"
#include "ApfsTree.h"
#include "DmgEncryption.h"
#include "HfsPlusDisk.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#pragma comment(lib,"bcrypt.lib")

namespace Apfs {
namespace {
using namespace HfsPlus;
using Disk::Check;
using Disk::Range;
using Uuid=std::array<uint8_t,16>;
constexpr size_t Sector=512;
constexpr uint32_t ContainerKeybag=0x6b657973, VolumeKeybag=0x72656373;
constexpr uint16_t VolumeKey=2, UnlockRecord=3;

template<size_t Size> struct Secret {
    std::array<uint8_t,Size> bytes{};
    ~Secret(){SecureZeroMemory(bytes.data(),bytes.size());}
};
class Algorithm {
public:
    BCRYPT_ALG_HANDLE handle=nullptr;
    explicit Algorithm(const wchar_t* name,ULONG flags=0){
        Check(BCryptOpenAlgorithmProvider(&handle,name,nullptr,flags)>=0,"Cannot initialize APFS cryptography");
    }
    ~Algorithm(){BCryptCloseAlgorithmProvider(handle,0);}
    Algorithm(const Algorithm&)=delete;
    Algorithm& operator=(const Algorithm&)=delete;
};
class Aes {
    Algorithm provider{BCRYPT_AES_ALGORITHM};
    Bytes object;
    BCRYPT_KEY_HANDLE key=nullptr;
public:
    Aes(const uint8_t* bytes,ULONG size){
        auto ecb=std::wstring(BCRYPT_CHAIN_MODE_ECB);
        Check(BCryptSetProperty(provider.handle,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(ecb.data()),
              ULONG((ecb.size()+1)*sizeof(wchar_t)),0)>=0,"Cannot set APFS AES mode");
        ULONG length=0,written=0;
        Check(BCryptGetProperty(provider.handle,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),
              sizeof(length),&written,0)>=0&&written==sizeof(length)&&length,"Cannot size APFS AES key");
        object.resize(length);
        if(BCryptGenerateSymmetricKey(provider.handle,&key,object.data(),length,const_cast<PUCHAR>(bytes),size,0)<0){
            if(key)BCryptDestroyKey(key);
            SecureZeroMemory(object.data(),object.size());
            throw Error("Cannot initialize APFS AES key");
        }
    }
    ~Aes(){
        if(key)BCryptDestroyKey(key);
        SecureZeroMemory(object.data(),object.size());
    }
    void BlockTransform(uint8_t* data,ULONG size,bool encrypt)const{
        ULONG written=0;
        const auto operation=encrypt?BCryptEncrypt:BCryptDecrypt;
        Check(operation(key,data,size,nullptr,nullptr,0,data,size,&written,0)>=0&&written==size,
              "APFS AES operation failed");
    }
};

class Xts final:public VolumeCipher {
    Aes content,tweak;
    static void Advance(std::array<uint8_t,16>& bytes){
        unsigned carry=0;
        for(auto& byte:bytes){
            const unsigned next=(unsigned(byte)<<1)|carry;
            byte=uint8_t(next);
            carry=next>>8;
        }
        if(carry)bytes[0]^=0x87;
    }
public:
    Xts(const uint8_t* a,const uint8_t* b):content(a,16),tweak(b,16){}
    void Decrypt(uint64_t first,uint8_t* data,size_t size)const override{
        Check(size%Sector==0&&size/Sector<=UINT64_MAX-first,"Invalid APFS XTS data-unit range");
        for(size_t start=0;start<size;start+=Sector,++first){
            Secret<16> unit;
            for(size_t i=0;i<8;++i)unit.bytes[i]=uint8_t(first>>(8*i));
            tweak.BlockTransform(unit.bytes.data(),16,true);
            Secret<Sector> masks;
            for(size_t at=0;at<Sector;at+=16){
                memcpy(masks.bytes.data()+at,unit.bytes.data(),16);
                Advance(unit.bytes);
            }
            const auto sector=data+start;
            for(size_t i=0;i<Sector;++i)sector[i]^=masks.bytes[i];
            content.BlockTransform(sector,Sector,false);
            for(size_t i=0;i<Sector;++i)sector[i]^=masks.bytes[i];
        }
    }
};

struct Keybag {
    Bytes data;
    struct Entry {Uuid uuid;unsigned tag;size_t offset,size;};
    std::vector<Entry> entries;
    View Value(const Entry& entry)const{return {data.data()+entry.offset,entry.size};}
};
Keybag ReadKeybag(Source& source,const Container& container,uint64_t block,uint64_t count,
                  const Uuid& uuid,uint32_t type){
    Check(block&&count&&count<=64&&Range(block,count,container.blockCount),"Invalid APFS keybag location");
    Keybag bag;
    bag.data=Disk::Read(source,container.offset+block*container.blockSize,size_t(count*container.blockSize));
    Xts(uuid.data(),uuid.data()).Decrypt(block*(container.blockSize/Sector),bag.data.data(),bag.data.size());
    const auto p=bag.data.data();
    Check(Le32(p+24)==type&&Le64(p)==ObjectChecksum(bag.data)&&Le16(p+32)==2,"Invalid APFS keybag");
    const auto countEntries=Le16(p+34);
    const uint64_t end=32+uint64_t(Le32(p+36));
    Check(end>=48&&end<=bag.data.size(),"Invalid APFS keybag size");
    size_t at=48;
    for(unsigned i=0;i<countEntries;++i){
        Check(Range(at,24,end),"Truncated APFS keybag entry");
        Keybag::Entry entry{{},Le16(p+at+16),at+24,Le16(p+at+18)};
        memcpy(entry.uuid.data(),p+at,entry.uuid.size());
        const auto width=(24+entry.size+15)&~size_t(15);
        Check(Range(at,width,end),"APFS keybag entry extends beyond its bag");
        bag.entries.push_back(entry);
        at+=width;
    }
    return bag;
}

// Key records contain nested tag/length/value fields. Consume each bounded
// field even when it is unknown so malformed lengths cannot hide duplicates.
View Field(View blob,unsigned wanted){
    View result{};
    size_t at=0;
    while(at<blob.size){
        Check(Range(at,2,blob.size),"Truncated APFS key field");
        const auto tag=blob.data[at++];
        size_t size=blob.data[at++];
        if(size&0x80){
            const auto width=size&0x7f;
            Check(width&&width<=4&&Range(at,width,blob.size),"Invalid APFS key field length");
            size=0;
            for(size_t i=0;i<width;++i)size=(size<<8)|blob.data[at++];
        }
        Check(Range(at,size,blob.size),"APFS key field exceeds its record");
        if(tag==wanted){
            Check(!result.data,"Duplicate APFS key field");
            result={blob.data+at,size};
        }
        at+=size;
    }
    return result;
}
View KeyFields(View record){return Field(Field(record,0x30),0xa3);}
bool SupportedWrap(View fields){
    const auto flags=Field(fields,0x82);
    return flags.size>=4&&!(Le32(flags.data)&2); // The legacy CoreStorage wrapper uses different key sizes.
}

bool Unwrap(const uint8_t* key,View wrapped,Secret<32>& result){
    if(wrapped.size!=40)return false;
    Aes aes(key,32);
    Secret<16> work;
    memcpy(work.bytes.data(),wrapped.data,8);
    memcpy(result.bytes.data(),wrapped.data+8,result.bytes.size());
    for(unsigned pass=6;pass>0;--pass){
        for(unsigned word=4;word>0;--word){
            // RFC 3394 serializes the round counter as a big-endian word.
            work.bytes[7]^=uint8_t(4*(pass-1)+word);
            auto part=result.bytes.data()+8*(word-1);
            memcpy(work.bytes.data()+8,part,8);
            aes.BlockTransform(work.bytes.data(),16,false);
            memcpy(part,work.bytes.data()+8,8);
        }
    }
    uint8_t mismatch=0;
    for(size_t i=0;i<8;++i)mismatch|=work.bytes[i]^0xa6;
    return mismatch==0;
}
}
std::shared_ptr<const VolumeCipher> UnlockSoftwareVolume(
    Source& source,const Container& container,const std::array<uint8_t,16>& uuid,const Dmg::Password& password){
    const auto bag=ReadKeybag(source,container,container.keybagBlock,container.keybagBlocks,container.uuid,ContainerKeybag);
    View wrappedVek{},location{};
    for(const auto& e:bag.entries){
        if(e.uuid!=uuid)continue;
        if(e.tag==VolumeKey){
            Check(!wrappedVek.data,"Duplicate APFS volume key");
            wrappedVek=bag.Value(e);
        }else if(e.tag==UnlockRecord){
            Check(!location.data,"Duplicate APFS volume keybag");
            location=bag.Value(e);
        }
    }
    Check(wrappedVek.data&&location.size==16,"Missing APFS volume keybag");
    const auto vekFields=KeyFields(wrappedVek);
    Check(SupportedWrap(vekFields),"Unsupported APFS volume key wrapper");
    const auto encryptedKey=Field(vekFields,0x83);
    Check(encryptedKey.size==40,"Invalid APFS wrapped volume key");
    const auto unlock=ReadKeybag(source,container,Le64(location.data),Le64(location.data+8),uuid,VolumeKeybag);
    Algorithm sha(BCRYPT_SHA256_ALGORITHM,BCRYPT_ALG_HANDLE_HMAC_FLAG);
    // Keybag round counts are untrusted. Bound the work in one unlock attempt.
    uint64_t remaining=20000000;
    bool supported=false;
    for(const auto& e:unlock.entries){
        if(e.tag!=UnlockRecord)continue;
        const auto fields=KeyFields(unlock.Value(e));
        if(!SupportedWrap(fields))continue;
        const auto count=Field(fields,0x84),salt=Field(fields,0x85),wrapped=Field(fields,0x83);
        if(!count.size||count.size>4||salt.size!=16||wrapped.size!=40)continue;
        uint64_t rounds=0;
        for(size_t i=0;i<count.size;++i)rounds=(rounds<<8)|count.data[i];
        Check(rounds&&rounds<=remaining,"Invalid or excessive APFS password derivation work");
        remaining-=rounds;
        supported=true;
        Secret<32> derived,kek,vek;
        Check(BCryptDeriveKeyPBKDF2(sha.handle,const_cast<PUCHAR>(password.Data()),password.Size(),
              const_cast<PUCHAR>(salt.data),ULONG(salt.size),rounds,derived.bytes.data(),ULONG(derived.bytes.size()),0)>=0,
              "Cannot derive APFS password key");
        if(Unwrap(derived.bytes.data(),wrapped,kek)&&Unwrap(kek.bytes.data(),encryptedKey,vek))
            return std::make_shared<Xts>(vek.bytes.data(),vek.bytes.data()+16);
    }
    Check(supported,"No supported APFS password unlock record");
    return {};
}
}
