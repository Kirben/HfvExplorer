// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DmgCertificate.h"
#include "DmgEncryption.h"
#include "HfsPlusDisk.h"
#include <ncrypt.h>
#include <wincrypt.h>
#include <mutex>
#pragma comment(lib,"ncrypt.lib")
#pragma comment(lib,"crypt32.lib")

namespace Dmg {
namespace {
using HfsPlus::Disk::Check;

struct Secret {
    HfsPlus::Bytes bytes;
    ~Secret(){if(!bytes.empty())SecureZeroMemory(bytes.data(),bytes.size());}
};

struct Store {
    HCERTSTORE handle=nullptr;
    ~Store(){if(handle)CertCloseStore(handle,0);}
};

bool ValidKeys(const uint8_t* data,DWORD written,DWORD keyBytes)
{
    return written==keyBytes+25&&!memcmp(data+keyBytes+20,"CKIE\0",5);
}

bool DecryptLegacy(HCRYPTKEY key,const CertificateRecord& record,uint8_t* plain,DWORD keyBytes)
{
    Secret buffer{record.wrapped};
    std::reverse(buffer.bytes.begin(),buffer.bytes.end());
    DWORD size=DWORD(buffer.bytes.size());
    if(!CryptDecrypt(key,0,TRUE,0,buffer.bytes.data(),&size)||!ValidKeys(buffer.bytes.data(),size,keyBytes))
        return false;
    memcpy(plain,buffer.bytes.data(),keyBytes+20);
    return true;
}

bool PublicKeyId(const uint8_t* der,DWORD size,std::array<uint8_t,20>& id)
{
    DWORD bytes=DWORD(id.size());
    return CryptHashCertificate(0,CALG_SHA1,0,der,size,id.data(),&bytes)&&bytes==id.size();
}

bool DecryptInstalled(PCCERT_CONTEXT certificate,const CertificateRecord& record,uint8_t* plain,DWORD keyBytes)
{
    const auto& pub=certificate->pCertInfo->SubjectPublicKeyInfo;
    std::array<uint8_t,20> id{};
    if(!pub.Algorithm.pszObjId||strcmp(pub.Algorithm.pszObjId,szOID_RSA_RSA)||pub.PublicKey.cUnusedBits||
       !PublicKeyId(pub.PublicKey.pbData,pub.PublicKey.cbData,id)||id!=record.id)return false;

    struct Acquired {
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE handle=0;
        DWORD spec=0;
        BOOL owned=FALSE;
        ~Acquired(){
            if(owned&&handle){
                if(spec==CERT_NCRYPT_KEY_SPEC)NCryptFreeObject(handle);
                else CryptReleaseContext(handle,0);
            }
        }
    } key;
    // Non-persisted PFX imports attach a live key to the certificate context.
    // The store owns this handle, and acquiring a provider by name can fail.
    CERT_KEY_CONTEXT context{};
    context.cbSize=sizeof(context);
    DWORD contextSize=sizeof(context);
    if(CertGetCertificateContextProperty(certificate,CERT_KEY_CONTEXT_PROP_ID,&context,&contextSize)&&context.hCryptProv){
        key.handle=context.hCryptProv;
        key.spec=context.dwKeySpec;
    }else if(!CryptAcquireCertificatePrivateKey(certificate,CRYPT_ACQUIRE_SILENT_FLAG|CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG,
                                                nullptr,&key.handle,&key.spec,&key.owned))return false;
    if(key.spec==CERT_NCRYPT_KEY_SPEC){
        Secret buffer{HfsPlus::Bytes(record.wrapped.size())};
        DWORD written=0;
        const auto status=NCryptDecrypt(key.handle,const_cast<BYTE*>(record.wrapped.data()),DWORD(record.wrapped.size()),
                                       nullptr,buffer.bytes.data(),DWORD(buffer.bytes.size()),&written,
                                       NCRYPT_PAD_PKCS1_FLAG|NCRYPT_SILENT_FLAG);
        if(status!=ERROR_SUCCESS||!ValidKeys(buffer.bytes.data(),written,keyBytes))return false;
        memcpy(plain,buffer.bytes.data(),keyBytes+20);
        return true;
    }
    struct Legacy {
        HCRYPTKEY key=0;
        ~Legacy(){if(key)CryptDestroyKey(key);}
    } capi;
    return CryptGetUserKey(key.handle,key.spec,&capi.key)&&DecryptLegacy(capi.key,record,plain,keyBytes);
}

bool TryStore(HCERTSTORE store,const std::vector<CertificateRecord>& records,uint8_t* plain,DWORD keyBytes)
{
    PCCERT_CONTEXT certificate=nullptr;
    while((certificate=CertEnumCertificatesInStore(store,certificate))!=nullptr){
        for(const auto& record:records){
            if(DecryptInstalled(certificate,record,plain,keyBytes)){
                CertFreeCertificateContext(certificate);
                return true;
            }
        }
    }
    return false;
}

bool Decode(LPCSTR type,const uint8_t* data,DWORD size,Secret& result)
{
    DWORD length=0;
    if(!CryptDecodeObjectEx(X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,type,data,size,0,nullptr,nullptr,&length))
        return false;
    result.bytes.resize(length);
    return CryptDecodeObjectEx(X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,type,data,size,0,nullptr,result.bytes.data(),&length)!=FALSE;
}

#ifdef HFV_TESTING
thread_local const PrivateKey* testStore=nullptr;
#endif
}

struct PrivateKey::State {
    Store store;
    HCRYPTPROV provider=0;
    HCRYPTKEY rsa=0;
    std::array<uint8_t,20> id{};
    mutable std::mutex mutex;
    ~State(){if(rsa)CryptDestroyKey(rsa);if(provider)CryptReleaseContext(provider,0);}
};

PrivateKey::PrivateKey(const std::wstring& path,const Password* password)
    :state_(std::make_unique<State>())
{
    const auto file=HfsPlus::OpenImage(path.c_str(),false);
    Check(file->Size()&&file->Size()<=4*1024*1024,"Invalid private key file size");
    Secret input{HfsPlus::Disk::Read(*file,0,size_t(file->Size()))};
    CRYPT_DATA_BLOB pfx{DWORD(input.bytes.size()),input.bytes.data()};
    if(PFXIsPFXBlob(&pfx)){
        struct Text {
            wchar_t value[4096]{};
            ~Text(){SecureZeroMemory(value,sizeof(value));}
        } text;
        if(password&&password->Size())
            Check(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(password->Data()),
                                      int(password->Size()),text.value,ARRAYSIZE(text.value)-1)>0,"Invalid PFX password");
        constexpr DWORD flags=PKCS12_NO_PERSIST_KEY|PKCS12_ALWAYS_CNG_KSP;
        state_->store.handle=PFXImportCertStore(&pfx,password?text.value:nullptr,flags);
        if(!state_->store.handle&&!password)state_->store.handle=PFXImportCertStore(&pfx,L"",flags);
        if(!state_->store.handle)throw KeyFilePasswordRequired();
        return;
    }

    Secret der;
    const uint8_t* bytes=input.bytes.data();
    DWORD length=DWORD(input.bytes.size());
    if(length>=11&&!memcmp(bytes,"-----BEGIN ",11)){
        DWORD decoded=0;
        const auto pem=reinterpret_cast<const char*>(bytes);
        Check(CryptStringToBinaryA(pem,length,CRYPT_STRING_BASE64HEADER,nullptr,&decoded,nullptr,nullptr),
              "Invalid PEM private key");
        der.bytes.resize(decoded);
        Check(CryptStringToBinaryA(pem,length,CRYPT_STRING_BASE64HEADER,der.bytes.data(),&decoded,nullptr,nullptr),
              "Invalid PEM private key");
        bytes=der.bytes.data();length=decoded;
    }
    Secret pkcs8, legacy;
    if(Decode(PKCS_PRIVATE_KEY_INFO,bytes,length,pkcs8)){
        const auto key=reinterpret_cast<const CRYPT_PRIVATE_KEY_INFO*>(pkcs8.bytes.data());
        Check(key->Algorithm.pszObjId&&!strcmp(key->Algorithm.pszObjId,szOID_RSA_RSA),"Private key is not RSA");
        bytes=key->PrivateKey.pbData;length=key->PrivateKey.cbData;
    }
    Check(Decode(PKCS_RSA_PRIVATE_KEY,bytes,length,legacy),
          "Choose an unencrypted RSA PEM/DER private key or a PFX/P12 file");
    Check(CryptAcquireContextW(&state_->provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT|CRYPT_SILENT)&&
          CryptImportKey(state_->provider,legacy.bytes.data(),DWORD(legacy.bytes.size()),0,0,&state_->rsa),
          "Windows could not import the RSA private key");
    // Apple's certificate identifier is SHA-1 of the PKCS#1 public key DER.
    DWORD size=0;
    Check(CryptExportKey(state_->rsa,0,PUBLICKEYBLOB,0,nullptr,&size)!=FALSE,"Cannot inspect RSA public key");
    HfsPlus::Bytes publicBlob(size);
    Check(CryptExportKey(state_->rsa,0,PUBLICKEYBLOB,0,publicBlob.data(),&size)&&
          CryptEncodeObject(X509_ASN_ENCODING,RSA_CSP_PUBLICKEYBLOB,publicBlob.data(),nullptr,&size),
          "Cannot encode RSA public key");
    HfsPlus::Bytes publicDer(size);
    Check(CryptEncodeObject(X509_ASN_ENCODING,RSA_CSP_PUBLICKEYBLOB,publicBlob.data(),publicDer.data(),&size)&&
          PublicKeyId(publicDer.data(),size,state_->id),"Cannot identify RSA private key");
}
PrivateKey::~PrivateKey()=default;

bool PrivateKey::Unwrap(const std::vector<CertificateRecord>& records,uint8_t* output,uint32_t keyBytes)const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    if(state_->store.handle)return TryStore(state_->store.handle,records,output,keyBytes);
    for(const auto& record:records)
        if(record.id==state_->id&&DecryptLegacy(state_->rsa,record,output,keyBytes))return true;
    return false;
}

bool FindCertificateKey(const std::vector<CertificateRecord>& records,uint8_t* output,uint32_t keyBytes)
{
#ifdef HFV_TESTING
    if(testStore)return testStore->Unwrap(records,output,keyBytes);
#endif
    for(DWORD location:{CERT_SYSTEM_STORE_CURRENT_USER,CERT_SYSTEM_STORE_LOCAL_MACHINE}){
        Store store;
        store.handle=CertOpenStore(CERT_STORE_PROV_SYSTEM_W,0,0,location|CERT_STORE_READONLY_FLAG|
                                    CERT_STORE_OPEN_EXISTING_FLAG,L"MY");
        if(store.handle&&TryStore(store.handle,records,output,keyBytes))return true;
    }
    return false;
}

#ifdef HFV_TESTING
void UseCertificateStoreForTest(const PrivateKey* store){testStore=store;}
#endif
}
