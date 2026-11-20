// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsUnicode.h"
#include "HfsPlusDisk.h"
#include <algorithm>
#include <windows.h>

namespace Apfs {
namespace {
using HfsPlus::Disk::Check;
struct CodeRange { char32_t first,last; };
struct Combining { char32_t point; unsigned value; };
struct Mapping { char32_t point; char32_t values[3]; };
#include "ApfsUnicodeData.inc"

template<class T,size_t N>
const T* Lookup(const T (&table)[N],char32_t point)
{
    const auto found=std::lower_bound(table,table+N,point,
                                     [](const auto& row,char32_t key){return row.point<key;});
    return found!=table+N&&found->point==point?found:nullptr;
}

bool Assigned(char32_t point)
{
    const auto found=std::lower_bound(std::begin(assigned),std::end(assigned),point,
                                     [](const auto& row,char32_t key){return row.last<key;});
    return found!=std::end(assigned)&&found->first<=point;
}

unsigned CombiningClass(char32_t point)
{
    const auto row=Lookup(combining,point);
    return row?row->value:0;
}

template<class Append>
void Decompose(char32_t point,const Append& append)
{
    constexpr char32_t Hangul=0xac00;
    if(point>=Hangul&&point<Hangul+11172){
        const auto syllable=point-Hangul;
        append(0x1100+syllable/(21*28));
        append(0x1161+(syllable/28)%21);
        if(syllable%28)append(0x11a7+syllable%28);
    }else if(const auto row=Lookup(decomposition,point)){
        for(const auto item:row->values)if(item)Decompose(item,append);
    }else append(point);
}

std::string Utf8(std::u16string_view name)
{
    if(name.empty())return {};
    const int size=int(name.size());
    const auto* chars=reinterpret_cast<const wchar_t*>(name.data());
    const int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,chars,size,nullptr,0,nullptr,nullptr);
    Check(length>0,"Invalid UTF-16 in APFS entry name");
    std::string result(length,'\0');
    Check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,chars,size,result.data(),length,nullptr,nullptr)==length,
          "Cannot encode APFS entry name");
    return result;
}

char32_t NextScalar(std::u16string_view name,size_t& index)
{
    const char32_t lead=name[index++];
    if(lead<0xd800||lead>0xdfff)return lead;
    Check(lead<=0xdbff&&index<name.size()&&name[index]>=0xdc00&&name[index]<=0xdfff,
          "Invalid UTF-16 in APFS entry name");
    return 0x10000+((lead-0xd800)<<10)+name[index++]-0xdc00;
}
}

std::string EncodeCatalogName(std::u16string_view name)
{
    Check(!name.empty()&&name.size()<=255&&name!=u"."&&name!=u"..","Invalid APFS entry name");
    for(size_t index=0;index<name.size();){
        const auto point=NextScalar(name,index);
        Check(point>=32&&point!=127&&point!=U'/'&&point!=U':'&&Assigned(point),"Invalid APFS entry name");
    }
    auto result=Utf8(name);
    Check(result.size()<=255,"APFS entry names must fit in 255 UTF-8 bytes");
    return result;
}

std::u32string NormalizedName(std::u16string_view name,bool fold)
{
    std::u32string result;
    const auto append=[&](char32_t point){
        const auto rank=CombiningClass(point);
        auto position=result.end();
        while(position!=result.begin()&&CombiningClass(position[-1])>rank&&rank)
            --position;
        result.insert(position,point);
    };
    const auto foldPoint=[&](char32_t point){
        const auto row=fold?Lookup(caseFold,point):nullptr;
        if(row){
            for(const auto item:row->values)if(item)append(item);
        }else append(point);
    };
    for(size_t index=0;index<name.size();)Decompose(NextScalar(name,index),foldPoint);
    return result;
}

std::u32string NormalizedName(std::string_view name,bool fold)
{
    if(name.empty())return {};
    const int size=int(name.size());
    std::wstring wide(size,L'\0');
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),size,wide.data(),size);
    Check(length>0,"Invalid UTF-8 in APFS entry name");
    return NormalizedName(std::u16string_view(reinterpret_cast<const char16_t*>(wide.data()),length),fold);
}
}
