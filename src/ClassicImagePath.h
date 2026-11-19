// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#define HFV_CLASSIC_IMAGE_PREFIX "|hfv-image|"

#ifdef __cplusplus
#include <windows.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ClassicImagePath {
// A pipe cannot occur in a Windows filename or a device pseudo-path.
inline constexpr char Prefix[]=HFV_CLASSIC_IMAGE_PREFIX;

inline bool IsEncoded(const char* path)
{
    return path&&strncmp(path,Prefix,sizeof(Prefix)-1)==0;
}

inline std::string Encode(const std::wstring& path)
{
    const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.c_str(),-1,nullptr,0,nullptr,nullptr);
    if(size<=1)throw std::runtime_error("Invalid classic HFS image path");
    std::string result=Prefix;
    const auto start=result.size();
    result.resize(start+size);
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.c_str(),-1,result.data()+start,size,nullptr,nullptr))
        throw std::runtime_error("Invalid classic HFS image path");
    result.pop_back();
    return result;
}

inline std::wstring Decode(const char* path)
{
    const auto text=path+sizeof(Prefix)-1;
    const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,nullptr,0);
    if(size<=1)throw std::runtime_error("Invalid classic HFS image path");
    std::wstring result(size,0);
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,result.data(),size))
        throw std::runtime_error("Invalid classic HFS image path");
    result.pop_back();
    return result;
}
}
#endif
