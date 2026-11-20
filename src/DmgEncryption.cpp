// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DmgEncryption.h"
#include "DmgCertificate.h"
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
        throw HfsPlus::Error("Invalid or too long disk image password");
    }
    size_=unsigned(length);
}

Password::~Password(){SecureZeroMemory(utf8_.data(),utf8_.size());}
PasswordRequired::PasswordRequired(bool again,UnlockMethod requested)
    :HfsPlus::Error(again?"The supplied credential did not unlock this image.":
        requested==UnlockMethod::PrivateKey?"This disk image requires its matching RSA private key.":
        requested==UnlockMethod::Either?"This disk image requires a password or private key.":
        "This disk image requires a password."),retry(again),method(requested){}

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
    std::vector<CertificateRecord> certificates;
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

Layout Parse(Source& source,bool token=false)
{
    constexpr uint32_t fixed=76,row=20;
    const auto header=Disk::Read(source,0,fixed);
    const auto d=header.data();
    const auto bits=U32(d+24),blocks=U32(d+52),count=U32(d+72);
    const auto size=U64(d+56),offset=U64(d+64);
    Check(U32(d+8)==2&&U32(d+12)==16&&U32(d+16)==5&&U32(d+20)==Aes&&(bits==128||bits==256),
          "Unsupported encrypted DMG payload format");
    Check(blocks>=512&&blocks<=1024*1024&&blocks%16==0,"Invalid encrypted disk image block size");
    // A bundle token contains the key table with no inline disk payload.
    if(token)Check(!size&&offset==source.Size(),"Invalid encrypted sparse bundle token extent");
    else Check(size&&size<=INT64_MAX&&(size-1)/blocks<=UINT32_MAX&&
               Range(offset,(size+15)&~uint64_t(15),source.Size()),"Invalid encrypted DMG data extent");
    Check(count&&count<=64&&Range(fixed,uint64_t(count)*row,offset),"Invalid encrypted DMG key table");
    const auto table=Disk::Read(source,fixed,size_t(count)*row);
    const auto recordStart=fixed+count*row;
    Layout result{offset,size,blocks,bits/8,{},{}};
    uint64_t iterations=0;
    for(uint32_t i=0;i<count;++i){
        const auto entry=table.data()+i*row;
        const auto at=U64(entry+4),bytes=U64(entry+12);
        Check(bytes&&bytes<=65536&&at>=recordStart&&Range(at,bytes,offset),"Invalid encrypted DMG key record extent");
        if(U32(entry)==2&&!token){
            const auto record=Disk::Read(source,at,size_t(bytes));
            const auto key=record.data();
            constexpr size_t prefix=52;
            Check(record.size()>=prefix,"Truncated DMG certificate record");
            if(U32(key)!=20||U32(key+36)!=42||U32(key+40)!=10)continue;
            const auto wrapped=U32(key+48);
            Check(wrapped>=128&&wrapped<=1024&&Range(prefix,wrapped,record.size()),
                  "Invalid DMG certificate key length");
            CertificateRecord cert{{},Bytes(key+prefix,key+prefix+wrapped)};
            memcpy(cert.id.data(),key+4,cert.id.size());
            result.certificates.push_back(std::move(cert));
        }
        if(U32(entry)!=1)continue;
        auto record=ParseRecord(Disk::Read(source,at,size_t(bytes)));
        if(!record)continue;
        iterations+=record->iterations;
        Check(result.records.size()<8&&iterations<=20000000,"DMG password records exceed the supported work limit");
        result.records.push_back(std::move(*record));
    }
    Check(!result.records.empty()||!result.certificates.empty(),"This encrypted image has no supported unlock record");
    return result;
}

void Derive(const Password& password,Bytes& salt,uint32_t iterations,Sensitive& derived)
{
    Algorithm algorithm(BCRYPT_SHA1_ALGORITHM,BCRYPT_ALG_HANDLE_HMAC_FLAG);
    Crypto(BCryptDeriveKeyPBKDF2(algorithm.handle,const_cast<PUCHAR>(password.Data()),password.Size(),
           salt.data(),ULONG(salt.size()),iterations,derived.bytes.data(),
           ULONG(derived.bytes.size()),0),"Cannot derive the DMG password key");
}

bool UnwrapKey(Record& record,const Password& password,size_t keyBytes,Sensitive& plain)
{
    Sensitive derived(24);
    Derive(password,record.salt,record.iterations,derived);
    Cipher cipher(record.method==Aes?BCRYPT_AES_ALGORITHM:BCRYPT_3DES_ALGORITHM,derived.bytes.data(),24);
    ULONG written=0;
    return cipher.Decrypt(record.wrapped,plain.bytes,record.iv.data(),ULONG(record.iv.size()),true,written)&&
           written>=keyBytes+25&&!memcmp(plain.bytes.data()+keyBytes+20,"CKIE\0",5);
}

// v1 uses two padded 3DES CBC layers. The reversed inner ciphertext
// begins with its own IV and the plaintext key follows a four-byte prefix.
bool LegacyKey(Cipher& cipher,Bytes wrapped,uint8_t* key,size_t keyBytes)
{
    uint8_t outerIv[]{0x4a,0xdd,0xa2,0x2c,0x79,0xe8,0x21,0x05};
    Sensitive inner(wrapped.size()),plain(wrapped.size());
    ULONG size=0;
    if(!cipher.Decrypt(wrapped,inner.bytes,outerIv,sizeof(outerIv),true,size)||size<16||size%8)
        return false;
    std::reverse(inner.bytes.begin(),inner.bytes.begin()+size);
    Bytes payload(inner.bytes.begin()+8,inner.bytes.begin()+size);
    uint8_t iv[8];
    memcpy(iv,inner.bytes.data(),sizeof(iv));
    if(!cipher.Decrypt(payload,plain.bytes,iv,sizeof(iv),true,size)||size!=keyBytes+4)
        return false;
    memcpy(key,plain.bytes.data()+4,keyBytes);
    return true;
}

Layout UnlockLegacy(Source& source,const Password* password,Sensitive& key)
{
    constexpr size_t trailerBytes=1276;
    Check(source.Size()>=trailerBytes,"Truncated version 1 encrypted DMG");
    const auto trailer=Disk::Read(source,source.Size()-trailerBytes,trailerBytes);
    const auto p=trailer.data();
    const auto bytes=U64(p+1256);
    const auto block=U32(p+16),bits=U32(p+100),saltBytes=U32(p+52);
    const auto iterations=U64(p+44);
    Check(U32(p+1264)==1&&U32(p+20)==TripleDes&&U32(p+24)==7&&U32(p+28)==6&&U32(p+32)==192&&
          U32(p+36)==8&&U32(p+40)==Pbkdf2&&U32(p+88)==16&&U32(p+92)==5&&U32(p+96)==Aes&&
          (bits==128||bits==256),"Unsupported version 1 encrypted DMG format");
    Check(bytes&&bytes<=INT64_MAX&&block>=512&&block<=1024*1024&&block%16==0&&
          (bytes-1)/block<=UINT32_MAX&&Range(0,(bytes+15)&~uint64_t(15),source.Size()-trailerBytes)&&
          saltBytes&&saltBytes<=32&&iterations&&iterations<=10000000,
          "Invalid version 1 encrypted DMG bounds");
    const auto wrapped=[&](size_t at){
        const auto size=U32(p+at);
        Check(size>=24&&size<=256&&size%8==0,"Invalid legacy DMG wrapped key length");
        return Bytes(p+at+4,p+at+4+size);
    };
    auto aes=wrapped(136),hmac=wrapped(436);
    if(!password)throw PasswordRequired(false);
    Bytes salt(p+56,p+56+saltBytes);
    Sensitive derived(24);
    Derive(*password,salt,uint32_t(iterations),derived);
    Cipher cipher(BCRYPT_3DES_ALGORITHM,derived.bytes.data(),24);
    const auto keyBytes=bits/8;
    if(!LegacyKey(cipher,std::move(aes),key.bytes.data(),keyBytes)||
       !LegacyKey(cipher,std::move(hmac),key.bytes.data()+keyBytes,20))
        throw PasswordRequired(true);
    return {0,bytes,block,keyBytes,{},{}};
}

class Payload final:public EncryptedPayload {
    const uint32_t block_;
    Cipher cipher_;
    ChunkIv iv_;
    std::mutex lock_;
    Sensitive plaintext_;
    Bytes encrypted_;
    uint64_t stream_=UINT64_MAX,cached_=UINT64_MAX;
public:
    Payload(const Layout& layout,const Sensitive& key)
        :block_(layout.block),cipher_(BCRYPT_AES_ALGORITHM,key.bytes.data(),layout.keyBytes),
         iv_(key.bytes.data()+layout.keyBytes),plaintext_(block_),encrypted_(block_){}
    uint32_t BlockSize()const override{return block_;}
    void Read(uint64_t stream,uint64_t size,uint64_t at,void* output,size_t bytes,
              const CiphertextReader& read)override{
        Check(Range(at,bytes,size)&&(!bytes||output)&&(!size||(size-1)/block_<=UINT32_MAX),
              "Read exceeds the encrypted disk image payload");
        std::lock_guard<std::mutex> lock(lock_);
        auto dest=static_cast<uint8_t*>(output);
        while(bytes){
            const uint64_t block=at/block_,position=at%block_;
            if(stream_!=stream||cached_!=block){
                cached_=UINT64_MAX;
                const auto offset=block*block_;
                const auto available=(std::min)(uint64_t(block_),size-offset);
                encrypted_.resize(size_t((available+15)&~uint64_t(15)));
                read(offset,encrypted_.data(),encrypted_.size());
                uint8_t iv[16]{};
                iv_.Make(uint32_t(block),iv);
                ULONG written=0;
                Check(cipher_.Decrypt(encrypted_,plaintext_.bytes,iv,sizeof(iv),false,written)&&
                      written==encrypted_.size(),"Cannot decrypt disk image block");
                stream_=stream;
                cached_=block;
            }
            const auto count=size_t((std::min)(uint64_t(bytes),block_-position));
            memcpy(dest,plaintext_.bytes.data()+position,count);
            at+=count;dest+=count;bytes-=count;
        }
    }
};

std::unique_ptr<EncryptedPayload> UnlockPayload(Layout& layout,const Password* password,const PrivateKey* privateKey=nullptr)
{
    const bool certificates=!layout.certificates.empty(), passwords=!layout.records.empty();
    if(certificates&&!password){
        Sensitive key(layout.keyBytes+20);
        const auto unlocked=privateKey?privateKey->Unwrap(layout.certificates,key.bytes.data(),layout.keyBytes):
                                        FindCertificateKey(layout.certificates,key.bytes.data(),layout.keyBytes);
        if(unlocked)return std::make_unique<Payload>(layout,key);
    }
    if(password){
        for(auto& record:layout.records){
            Sensitive key(record.wrapped.size());
            if(UnwrapKey(record,*password,layout.keyBytes,key))
                return std::make_unique<Payload>(layout,key);
        }
    }
    const auto method=certificates?(passwords?UnlockMethod::Either:UnlockMethod::PrivateKey):UnlockMethod::Password;
    throw PasswordRequired(password||privateKey,method);
}

class Image final:public Source {
    std::unique_ptr<Source> file_;
    const uint64_t start_,size_;
    std::unique_ptr<EncryptedPayload> payload_;
public:
    Image(std::unique_ptr<Source> file,const Layout& layout,std::unique_ptr<EncryptedPayload> payload)
        :file_(std::move(file)),start_(layout.start),size_(layout.bytes),payload_(std::move(payload)){}
    uint64_t Size()const override{return size_;}
    bool Writable()const override{return false;}
    bool ProcessLocal()const override{return true;}
    void CheckMedia()override{file_->CheckMedia();}
    std::shared_ptr<Source> OpenJournal(const Bytes& info)override{return file_->OpenJournal(info);}
    void Flush()override{}
    void Write(uint64_t,const void*,size_t)override{throw Error("Encrypted DMG images are read-only");}
    void Read(uint64_t at,void* output,size_t bytes)override{
        payload_->Read(0,size_,at,output,bytes,[this](uint64_t offset,void* cipher,size_t count){
            file_->Read(start_+offset,cipher,count);
        });
    }
};
}

bool Unlock(std::unique_ptr<HfsPlus::Source>& source,const Password* password,const PrivateKey* key)
{
    if(source->Size()<8)return false;
    const auto magic=Disk::Read(*source,0,8);
    if(memcmp(magic.data(),"encrcdsa",8)){
        const auto tail=Disk::Read(*source,source->Size()-8,8);
        if(memcmp(tail.data(),"cdsaencr",8))return false;
        Sensitive key(32+20);
        auto layout=UnlockLegacy(*source,password,key);
        source=std::make_unique<Image>(std::move(source),layout,std::make_unique<Payload>(layout,key));
        return true;
    }
    auto layout=Parse(*source);
    auto payload=UnlockPayload(layout,password,key);
    source=std::make_unique<Image>(std::move(source),layout,std::move(payload));
    return true;
}

std::unique_ptr<EncryptedPayload> UnlockBundleToken(HfsPlus::Source& token,const Password* password)
{
    Check(token.Size()>=76&&!memcmp(Disk::Read(token,0,8).data(),"encrcdsa",8),
          "Invalid or unsupported sparse bundle encryption token");
    auto layout=Parse(token,true);
    return UnlockPayload(layout,password);
}
}
