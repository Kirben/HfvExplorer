// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DmgEncryption.h"
#include "HfsPlusDisk.h"
#include <bcrypt.h>
#include <mutex>
#include <optional>
#pragma comment(lib,"bcrypt.lib")

namespace Dmg {
Password::Password(std::wstring_view value)
{
    if(value.empty())return;
    const auto length=value.size()<=1023?WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,
        value.data(),int(value.size()),reinterpret_cast<char*>(utf8_.data()),int(utf8_.size()),nullptr,nullptr):0;
    if(length<=0){
        SecureZeroMemory(utf8_.data(),utf8_.size());
        throw HfsPlus::Error("Invalid or too long DMG password");
    }
    size_=unsigned(length);
}

Password::~Password(){SecureZeroMemory(utf8_.data(),utf8_.size());}
PasswordRequired::PasswordRequired(bool again)
    :HfsPlus::Error(again?"The DMG password did not unlock this image.":"This DMG requires a password."),retry(again){}

namespace {
using namespace HfsPlus;
using namespace HfsPlus::Disk;
constexpr uint32_t Aes=0x80000001,TripleDes=17,Pbkdf2=0x67;

void Crypto(NTSTATUS status,const char* message){Check(status>=0,message);}
struct Sensitive {
    Bytes bytes;
    explicit Sensitive(size_t size):bytes(size){}
    ~Sensitive(){if(!bytes.empty())SecureZeroMemory(bytes.data(),bytes.size());}
    Sensitive(const Sensitive&)=delete;
    Sensitive& operator=(const Sensitive&)=delete;
};
class Algorithm {
public:
    BCRYPT_ALG_HANDLE handle=nullptr;
    explicit Algorithm(const wchar_t* id,ULONG flags=0){
        Crypto(BCryptOpenAlgorithmProvider(&handle,id,nullptr,flags),"Cannot initialize Windows DMG cryptography");
    }
    ~Algorithm(){if(handle)BCryptCloseAlgorithmProvider(handle,0);}
    Algorithm(const Algorithm&)=delete;
    Algorithm& operator=(const Algorithm&)=delete;
    ULONG ObjectSize()const {
        ULONG bytes=0,actual=0;
        Crypto(BCryptGetProperty(handle,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&bytes),sizeof(bytes),&actual,0),
               "Cannot size the Windows cryptography object");
        Check(actual==sizeof(bytes)&&bytes,"Invalid Windows cryptography object size");
        return bytes;
    }
};

class Cipher {
    Algorithm algorithm_;
    Sensitive object_;
    BCRYPT_KEY_HANDLE key_=nullptr;
public:
    Cipher(const wchar_t* algorithm,const uint8_t* key,ULONG length)
        :algorithm_(algorithm),object_(algorithm_.ObjectSize()){
        Crypto(BCryptSetProperty(algorithm_.handle,BCRYPT_CHAINING_MODE,
               reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),sizeof(BCRYPT_CHAIN_MODE_CBC),0),
               "Cannot initialize CBC for a DMG");
        Crypto(BCryptGenerateSymmetricKey(algorithm_.handle,&key_,object_.bytes.data(),ULONG(object_.bytes.size()),
               const_cast<PUCHAR>(key),length,0),"Cannot initialize the DMG decryption key");
    }
    ~Cipher(){if(key_)BCryptDestroyKey(key_);}
    bool Decrypt(Bytes& ciphertext,Bytes& plaintext,uint8_t* iv,ULONG ivBytes,bool padded,ULONG& written){
        return BCryptDecrypt(key_,ciphertext.data(),ULONG(ciphertext.size()),nullptr,
               iv,ivBytes,plaintext.data(),ULONG(plaintext.size()),&written,padded?BCRYPT_BLOCK_PADDING:0)>=0;
    }
};
class ChunkIv {
    Algorithm sha_{BCRYPT_SHA1_ALGORITHM,BCRYPT_ALG_HANDLE_HMAC_FLAG};
    Sensitive object_{sha_.ObjectSize()},secret_{20};
public:
    explicit ChunkIv(const uint8_t* secret){memcpy(secret_.bytes.data(),secret,20);}
    void Make(uint32_t chunk,uint8_t* iv){
        struct Hash {
            BCRYPT_HASH_HANDLE handle=nullptr;
            ~Hash(){if(handle)BCryptDestroyHash(handle);}
        } hash;
        uint8_t word[4];
        P32(word,chunk);
        Crypto(BCryptCreateHash(sha_.handle,&hash.handle,object_.bytes.data(),ULONG(object_.bytes.size()),
               secret_.bytes.data(),ULONG(secret_.bytes.size()),0),"Cannot initialize the DMG chunk IV");
        Crypto(BCryptHashData(hash.handle,word,sizeof(word),0),"Cannot calculate the DMG chunk IV");
        Sensitive digest(20);
        Crypto(BCryptFinishHash(hash.handle,digest.bytes.data(),ULONG(digest.bytes.size()),0),
               "Cannot finish the DMG chunk IV");
        memcpy(iv,digest.bytes.data(),16);
    }
};

struct Record {
    uint32_t iterations,method;
    Bytes salt,iv,wrapped;
};
struct Layout {
    uint64_t start,bytes;
    uint32_t block,keyBytes;
    std::vector<Record> records;
};

std::optional<Record> ParseRecord(const Bytes& data)
{
    constexpr size_t fixed=104;
    Check(data.size()>=fixed,"Truncated DMG password record");
    const auto d=data.data();
    const auto algorithm=U32(d+88),ivSize=U32(d+48),saltSize=U32(d+12),length=U32(d+100);
    if(U32(d)!=Pbkdf2||U32(d+84)!=192||(algorithm!=Aes&&algorithm!=TripleDes)||
       U32(d+92)!=7||U32(d+96)!=6)return {};
    const auto block=algorithm==Aes?16u:8u;
    Check(saltSize&&saltSize<=32&&(ivSize==8||(algorithm==Aes&&ivSize==16))&&
          length&&length<=256&&length%block==0&&Range(fixed,length,data.size()),
          "Invalid DMG password record");
    const auto iter=U32(d+8);
    Check(iter&&iter<=10000000,"DMG password derivation exceeds the supported work limit");
    Bytes iv(block);
    memcpy(iv.data(),d+52,ivSize);
    return Record{iter,algorithm,Bytes(d+16,d+16+saltSize),std::move(iv),Bytes(d+fixed,d+fixed+length)};
}

Layout Parse(Source& source)
{
    constexpr uint32_t fixed=76,row=20;
    const auto header=Disk::Read(source,0,fixed);
    const auto d=header.data();
    const auto bits=U32(d+24),blocks=U32(d+52),count=U32(d+72);
    const auto size=U64(d+56),offset=U64(d+64);
    Check(U32(d+8)==2&&U32(d+12)==16&&U32(d+16)==5&&U32(d+20)==Aes&&(bits==128||bits==256),
          "Unsupported encrypted DMG payload format");
    Check(size&&size<=INT64_MAX&&blocks>=512&&blocks<=1024*1024&&blocks%16==0&&
          (size-1)/blocks<=UINT32_MAX&&Range(offset,(size+15)&~uint64_t(15),source.Size()),
          "Invalid encrypted DMG data extent");
    Check(count&&count<=64&&Range(fixed,uint64_t(count)*row,offset),"Invalid encrypted DMG key table");
    const auto table=Disk::Read(source,fixed,size_t(count)*row);
    const auto recordStart=fixed+count*row;
    Layout result{offset,size,blocks,bits/8,{}};
    uint64_t iterations=0;
    for(uint32_t i=0;i<count;++i){
        const auto entry=table.data()+i*row;
        const auto at=U64(entry+4),bytes=U64(entry+12);
        Check(bytes&&bytes<=65536&&at>=recordStart&&Range(at,bytes,offset),"Invalid encrypted DMG key record extent");
        if(U32(entry)!=1)continue; // Certificate or other key types.
        auto record=ParseRecord(Disk::Read(source,at,size_t(bytes)));
        if(!record)continue;
        iterations+=record->iterations;
        Check(result.records.size()<8&&iterations<=20000000,"DMG password records exceed the supported work limit");
        result.records.push_back(std::move(*record));
    }
    Check(!result.records.empty(),"This encrypted DMG has no supported password record");
    return result;
}

bool UnwrapKey(Record& record,const Password& password,size_t keyBytes,Sensitive& plain)
{
    Algorithm algorithm(BCRYPT_SHA1_ALGORITHM,BCRYPT_ALG_HANDLE_HMAC_FLAG);
    Sensitive derived(24);
    Crypto(BCryptDeriveKeyPBKDF2(algorithm.handle,const_cast<PUCHAR>(password.Data()),password.Size(),
           record.salt.data(),ULONG(record.salt.size()),record.iterations,derived.bytes.data(),
           ULONG(derived.bytes.size()),0),"Cannot derive the DMG password key");
    Cipher cipher(record.method==Aes?BCRYPT_AES_ALGORITHM:BCRYPT_3DES_ALGORITHM,derived.bytes.data(),24);
    ULONG written=0;
    return cipher.Decrypt(record.wrapped,plain.bytes,record.iv.data(),ULONG(record.iv.size()),true,written)&&
           written>=keyBytes+25&&!memcmp(plain.bytes.data()+keyBytes+20,"CKIE\0",5);
}

class Image final:public Source {
    std::unique_ptr<Source> file_;
    const uint64_t start_,size_;
    const uint32_t block_;
    Cipher cipher_;
    ChunkIv iv_;
    std::mutex lock_;
    Sensitive plaintext_;
    Bytes encrypted_;
    uint64_t cached_=UINT64_MAX;
public:
    Image(std::unique_ptr<Source> file,const Layout& layout,const Sensitive& key)
        :file_(std::move(file)),start_(layout.start),size_(layout.bytes),block_(layout.block),
        cipher_(BCRYPT_AES_ALGORITHM,key.bytes.data(),layout.keyBytes),
        iv_(key.bytes.data()+layout.keyBytes),plaintext_(block_),encrypted_(block_){}
    uint64_t Size()const override{return size_;}
    bool Writable()const override{return false;}
    bool ProcessLocal()const override{return true;}
    void CheckMedia()override{file_->CheckMedia();}
    std::shared_ptr<Source> OpenJournal(const Bytes& info)override{return file_->OpenJournal(info);}
    void Flush()override{}
    void Write(uint64_t,const void*,size_t)override{throw Error("Encrypted DMG images are read-only");}
    void Read(uint64_t at,void* output,size_t bytes)override{
        Check(Range(at,bytes,size_)&&(!bytes||output),"Read exceeds the encrypted DMG payload");
        std::lock_guard<std::mutex> lock(lock_);
        auto dest=static_cast<uint8_t*>(output);
        while(bytes){
            const uint64_t block=at/block_,position=at%block_;
            if(cached_!=block){
                cached_=UINT64_MAX;
                const auto offset=block*block_;
                const auto available=(std::min)(uint64_t(block_),size_-offset);
                encrypted_.resize(size_t((available+15)&~uint64_t(15)));
                file_->Read(start_+offset,encrypted_.data(),encrypted_.size());
                uint8_t iv[16]{};
                iv_.Make(uint32_t(block),iv);
                ULONG written=0;
                Check(cipher_.Decrypt(encrypted_,plaintext_.bytes,iv,sizeof(iv),false,written)&&
                      written==encrypted_.size(),"Cannot decrypt DMG data block");
                cached_=block;
            }
            const auto count=size_t((std::min)(uint64_t(bytes),block_-position));
            memcpy(dest,plaintext_.bytes.data()+position,count);
            at+=count;dest+=count;bytes-=count;
        }
    }
};
}

bool Unlock(std::unique_ptr<HfsPlus::Source>& source,const Password* password)
{
    if(source->Size()<8)return false;
    const auto magic=Disk::Read(*source,0,8);
    if(memcmp(magic.data(),"encrcdsa",8)){
        const auto tail=Disk::Read(*source,source->Size()-8,8);
        Check(memcmp(tail.data(),"cdsaencr",8)!=0,"Legacy version 1 encrypted DMG images are unsupported");
        return false;
    }
    auto layout=Parse(*source);
    if(!password)throw PasswordRequired(false);
    for(auto& record:layout.records){
        Sensitive key(record.wrapped.size());
        if(UnwrapKey(record,*password,layout.keyBytes,key)){
            source=std::make_unique<Image>(std::move(source),layout,key);
            return true;
        }
    }
    throw PasswordRequired(true);
}
}
