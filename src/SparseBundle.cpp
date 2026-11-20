// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "SparseDisk.h"
#include "AppleImagePlist.h"
#include "HfsPlusDisk.h"
#include <optional>

namespace SparseDisk {
namespace {
using namespace HfsPlus;
using namespace Disk;
constexpr uint64_t SectorSize=512;

struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};

BY_HANDLE_FILE_INFORMATION Info(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION value{};
    Check(GetFileInformationByHandle(handle,&value)!=FALSE,"Cannot inspect sparse bundle member");
    return value;
}

uint64_t FileSize(const BY_HANDLE_FILE_INFORMATION& value)
{
    return (uint64_t(value.nFileSizeHigh)<<32)|value.nFileSizeLow;
}

void Open(Handle& handle,const std::wstring& path,bool directory,bool optional=false)
{
    handle.value=CreateFileW(path.c_str(),directory?FILE_READ_ATTRIBUTES:GENERIC_READ,
                             directory?FILE_SHARE_READ|FILE_SHARE_WRITE:FILE_SHARE_READ,
                             nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle.value==INVALID_HANDLE_VALUE){
        Check(optional&&GetLastError()==ERROR_FILE_NOT_FOUND,"Cannot open sparse bundle member");
        return;
    }
    const auto attributes=Info(handle.value).dwFileAttributes;
    Check(!(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&bool(attributes&FILE_ATTRIBUTE_DIRECTORY)==directory,
          "A sparse bundle member has the wrong file type or is a reparse point");
}

void ReadAt(HANDLE handle,uint64_t offset,void* bytes,size_t count)
{
    LARGE_INTEGER at{};
    at.QuadPart=offset;
    Check(SetFilePointerEx(handle,at,nullptr,FILE_BEGIN)!=FALSE,"Cannot seek in sparse bundle band");
    auto out=static_cast<uint8_t*>(bytes);
    while(count){
        const DWORD take=DWORD((std::min)(count,size_t(1<<20)));
        DWORD done=0;
        Check(ReadFile(handle,out,take,&done,nullptr)&&done==take,"Cannot read sparse bundle member");
        count-=take;out+=take;
    }
}

uint64_t Decimal(std::wstring_view number)
{
    Check(!number.empty(),"Empty sparse bundle geometry value");
    uint64_t value=0;
    for(auto ch:number){
        Check(ch>=L'0'&&ch<=L'9'&&value<=(UINT64_MAX-(ch-L'0'))/10,"Invalid sparse bundle geometry value");
        value=value*10+(ch-L'0');
    }
    return value;
}

std::optional<uint64_t> BandNumber(std::wstring_view name)
{
    if(name.empty()||(name.size()>1&&name.front()==L'0')||name.size()>16)return {};
    uint64_t result=0;
    for(const auto digit:name){
        if(digit>=L'0'&&digit<=L'9')result=(result<<4)|(digit-L'0');
        else if(digit>=L'a'&&digit<=L'f')result=(result<<4)|(digit-L'a'+10);
        else return {};
    }
    return result;
}

enum class Field { Key, Type, Version, BandSize, Size, Other };
Field Key(std::wstring_view name)
{
    if(name==L"diskimage-bundle-type")return Field::Type;
    if(name==L"bundle-backingstore-version")return Field::Version;
    if(name==L"band-size")return Field::BandSize;
    if(name==L"size")return Field::Size;
    return Field::Other;
}

struct Geometry {uint64_t size,bandSize;};
Geometry ParseInfo(HANDLE info)
{
    const auto length=FileSize(Info(info));
    Check(length&&length<=1024*1024,"Invalid sparse bundle Info.plist size");
    Bytes xml(size_t(length),0);
    ReadAt(info,0,xml.data(),xml.size());
    auto reader=AppleImagePlist::Open(xml);

    std::wstring name,text;
    Field field=Field::Other;
    std::map<Field,std::wstring> values;
    bool plist=false,dict=false,keyPending=false;
    HRESULT status;
    XmlNodeType event;
    unsigned depth=0;
    while((status=reader->Read(&event))==S_OK){
        if(event==XmlNodeType_EndElement){
            Check(depth>0,"Invalid sparse bundle plist nesting");
            --depth;
        }
        if(event==XmlNodeType_Element){
            Check(depth<64,"Invalid sparse bundle plist structure");
            const wchar_t* local=nullptr;
            UINT chars=0;
            Check(SUCCEEDED(reader->GetLocalName(&local,&chars)),"Cannot read sparse bundle plist element");
            const std::wstring_view tag(local,chars);
            if(depth==0){
                Check(!plist&&tag==L"plist"&&!reader->IsEmptyElement(),"Invalid sparse bundle plist root");
                plist=true;
            }else if(depth==1){
                Check(!dict&&tag==L"dict"&&!reader->IsEmptyElement(),"Invalid sparse bundle plist dictionary");
                dict=true;
            }else if(depth==2){
                text.clear();
                if(!keyPending){
                    Check(tag==L"key"&&!reader->IsEmptyElement(),"Missing sparse bundle plist key");
                    field=Field::Key;
                }else{
                    Check(tag!=L"key","Missing sparse bundle plist value");
                    keyPending=false;
                    field=Key(name);
                    if(field!=Field::Other){
                        Check(!reader->IsEmptyElement()&&(field==Field::Type?tag==L"string":tag==L"integer"),
                              "Invalid sparse bundle plist value type");
                    }
                }
            }else Check(field==Field::Other,"Unexpected nested sparse bundle plist value");
            if(!reader->IsEmptyElement())++depth;
        }else if(event==XmlNodeType_EndElement&&depth==2){
            if(field==Field::Key){
                Check(!text.empty(),"Empty sparse bundle plist key");
                name=std::move(text);
                keyPending=true;
            }else if(field!=Field::Other){
                Check(values.emplace(field,std::move(text)).second,"Duplicate sparse bundle plist key");
            }
            field=Field::Other;
        }else if(depth==3&&field!=Field::Other&&
                 (event==XmlNodeType_Text||event==XmlNodeType_CDATA||event==XmlNodeType_Whitespace)){
            const wchar_t* contents=nullptr;
            UINT chars=0;
            Check(SUCCEEDED(reader->GetValue(&contents,&chars)),"Cannot read sparse bundle plist value");
            text.append(contents,chars);
        }
    }
    Check(status==S_FALSE&&!depth&&plist&&dict&&!keyPending&&values.size()==4,"Incomplete or malformed sparse bundle plist");
    Check(values[Field::Type]==L"com.apple.diskimage.sparsebundle"&&Decimal(values[Field::Version])==1,
          "Unsupported sparse bundle type or version");
    const auto size=Decimal(values[Field::Size]), band=Decimal(values[Field::BandSize]);
    Check(size&&size<=INT64_MAX&&size%SectorSize==0&&band&&band<=INT64_MAX&&band%SectorSize==0,
          "Invalid sparse bundle geometry");
    return {size,band};
}

struct Band {uint64_t bytes;FILETIME modified;};

class SparseBundle final : public Source {
    std::wstring path_, bandsPath_;
    Handle root_, folder_, plist_;
    FILETIME stamp_{};
    Geometry geometry_{};
    std::map<uint64_t,Band> bands_;
public:
    explicit SparseBundle(std::wstring path):path_(std::move(path)),bandsPath_(path_+L"\\bands"){
        Open(root_,path_,true);
        Open(folder_,bandsPath_,true);
        Open(plist_,path_+L"\\Info.plist",false);
        geometry_=ParseInfo(plist_.value);

        Handle token;
        Open(token,path_+L"\\token",false,true);
        Check(token.value==INVALID_HANDLE_VALUE||FileSize(Info(token.value))==0,
              "Encrypted sparse bundles are unsupported");

        stamp_=Info(folder_.value).ftLastWriteTime;
        const auto count=(geometry_.size-1)/geometry_.bandSize+1;
        WIN32_FIND_DATAW data{};
        const auto search=FindFirstFileW((bandsPath_+L"\\*").c_str(),&data);
        if(search!=INVALID_HANDLE_VALUE){
            struct Search {HANDLE value;~Search(){FindClose(value);}} close{search};
            do {
                const auto number=BandNumber(data.cFileName);
                if(!number||*number>=count)continue;
                Check(!(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),
                      "A sparse bundle band is a directory or reparse point");
                const auto bytes=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
                bands_.emplace(*number,Band{bytes,data.ftLastWriteTime});
            }while(FindNextFileW(search,&data));
            Check(GetLastError()==ERROR_NO_MORE_FILES,"Cannot enumerate sparse bundle bands");
        }else Check(GetLastError()==ERROR_FILE_NOT_FOUND,"Cannot enumerate sparse bundle bands");
        CheckMedia();
    }

    uint64_t Size()const override{return geometry_.size;}
    bool Writable()const override{return false;}
    // Retain the captured band metadata for file exports. Another process
    // opening the directory would acquire a different snapshot.
    bool ProcessLocal()const override{return true;}
    void CheckMedia()override{
        const auto info=Info(folder_.value);
        Check(CompareFileTime(&info.ftLastWriteTime,&stamp_)==0,"Sparse bundle bands changed; reopen the image");
    }
    void Read(uint64_t offset,void* bytes,size_t count)override{
        Check(Range(offset,count,Size()),"Read outside the sparse bundle");
        CheckMedia();
        auto out=static_cast<uint8_t*>(bytes);
        while(count){
            const auto number=offset/geometry_.bandSize,inside=offset%geometry_.bandSize;
            const auto band=bands_.lower_bound(number);
            size_t take;
            if(band!=bands_.end()&&band->first==number){
                take=size_t((std::min)(uint64_t(count),geometry_.bandSize-inside));
                wchar_t name[17]{};
                swprintf_s(name,L"%llx",static_cast<unsigned long long>(number));
                Handle file;
                Open(file,bandsPath_+L"\\"+name,false);
                const auto info=Info(file.value);
                Check(FileSize(info)==band->second.bytes&&CompareFileTime(&info.ftLastWriteTime,&band->second.modified)==0,
                      "A sparse bundle band changed; reopen the image");
                const size_t have=inside>=band->second.bytes?0:size_t((std::min)(uint64_t(take),band->second.bytes-inside));
                if(have)ReadAt(file.value,inside,out,have);
                memset(out+have,0,take-have);
            }else{
                const auto next=band==bands_.end()?Size():band->first*geometry_.bandSize;
                take=size_t((std::min)(uint64_t(count),next-offset));
                memset(out,0,take);
            }
            offset+=take;out+=take;count-=take;
        }
        CheckMedia();
    }
    void Write(uint64_t,const void*,size_t)override{throw Error("Sparse bundles are read-only");}
    void Flush()override{}
};
}

static bool BundleName(const std::wstring& path)
{
    const auto dot=path.find_last_of(L'.');
    return dot!=path.npos&&_wcsicmp(path.c_str()+dot,L".sparsebundle")==0;
}

std::wstring BundleFromPlist(const std::wstring& path)
{
    const auto slash=path.find_last_of(L"\\/");
    if(slash==path.npos||_wcsicmp(path.c_str()+slash+1,L"Info.plist")!=0)return path;
    const auto folder=path.substr(0,slash);
    return BundleName(folder)?folder:path;
}

std::unique_ptr<HfsPlus::Source> OpenBundle(const std::wstring& path)
{
    const auto last=path.find_last_not_of(L"\\/");
    if(last==path.npos)return {};
    const auto folder=path.substr(0,last+1);
    if(!BundleName(folder))return {};
    const auto attrs=GetFileAttributesW(folder.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES||!(attrs&FILE_ATTRIBUTE_DIRECTORY))return {};
    return std::make_unique<SparseBundle>(folder);
}
}
