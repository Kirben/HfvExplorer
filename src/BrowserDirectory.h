// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include "ftypes.h"
#include <cstdio>
#include <cstring>

namespace BrowserDirectory {
// Immutable presentation values. Backend bindings retain the existing classic
// conversion/launcher routes without exposing catalog records to the view.
struct OwnedIcons {
 HICON small=nullptr,large=nullptr;
 ~OwnedIcons(){if(small)DestroyIcon(small);if(large&&large!=small)DestroyIcon(large);}
};
struct Row {
    MacBrowser::Entry entry;
    MacBrowser::Capabilities capabilities;
    int storageType = LIST_MAC, volume = -1;
    uint32_t id = 0, parent = 0;
    std::string path;
    uint64_t created = 0, modified = 0, directoryItems = 0;
    bool countKnown = false, initialized = false;
    POINT point{-1,-1};
    int finderMode = 0;
    HICON icon16 = nullptr, icon32 = nullptr;
    std::shared_ptr<OwnedIcons> ownedIcons;
};
struct Snapshot {
    std::string resourceKey;uint64_t revision=0;
    std::wstring folderPath;std::string folderIdentity;
    std::vector<Row> rows;
    MacBrowser::Capabilities capabilities;
    bool capabilitiesKnown=false;
    int finderMode = 0, originX = 0, originY = 0;
};
#ifdef HFV_TESTING
extern std::function<void(const char*)> testObserveRead;
#endif
inline void ObserveRead(const char* path){
#ifdef HFV_TESTING
 if(testObserveRead)testObserveRead(path);
#else
 (void)path;
#endif
}
inline uint64_t Ticks(FILETIME t) { return uint64_t(t.dwHighDateTime)<<32 | t.dwLowDateTime; }
inline FILETIME FileTime(uint64_t t) { return {DWORD(t), DWORD(t>>32)}; }
inline uint64_t MacTime(uint32_t t) { return (uint64_t(t)+9561628800ULL)*10000000ULL; }
inline std::u16string FromAnsi(const char* text) {
    int n=MultiByteToWideChar(CP_ACP,0,text,-1,nullptr,0);std::wstring out(n,0);
    if(n)MultiByteToWideChar(CP_ACP,0,text,-1,&out[0],n);
    if(n)out.resize(n-1);return MacBrowser::U16(out);
}
inline uint64_t Bytes(const MacBrowser::Entry& e) {
    return UINT64_MAX-e.forks[0].size<e.forks[1].size?UINT64_MAX:e.forks[0].size+e.forks[1].size;
}
inline unsigned Label(const MacBrowser::Entry& e) { return ((unsigned(e.finder[8])<<8|e.finder[9])>>1)&7; }
inline Row FromMac(const MacBrowser::Entry& e,int volume,MacBrowser::Capabilities caps) {
    Row row;row.entry=e;row.volume=volume;row.id=e.id;row.parent=e.parent;row.capabilities=caps;
    if(e.protectedFile||e.compressed||e.hardLink||!e.readError.empty())row.capabilities.create=row.capabilities.rename=row.capabilities.remove=false;
    if(e.datesValid){row.created=MacTime(e.created);row.modified=MacTime(e.modified);}return row;
}
// All formats sort typed metadata, never formatted sizes or date strings.
inline int Compare(const Row& a,const Row& b,int column,bool ascending) {
    if(a.entry.directory!=b.entry.directory)return a.entry.directory?-1:1;
    auto number=[](uint64_t x,uint64_t y){return x<y?-1:x>y?1:0;};int result=0;
    if(column==1)result=number(a.entry.directory?a.directoryItems:Bytes(a.entry),b.entry.directory?b.directoryItems:Bytes(b.entry));
    else if(column==2)result=number(a.created,b.created);
    else if(column==3)result=number(a.modified,b.modified);
    else if(column==4||column==5)result=memcmp(a.entry.finder.data()+(column==4?0:4),b.entry.finder.data()+(column==4?0:4),4);
    else if(column==6)result=number(Label(a.entry),Label(b.entry));
    auto compareName=[&](BOOL ignore){return CompareStringOrdinal(reinterpret_cast<const wchar_t*>(a.entry.name.data()),int(a.entry.name.size()),reinterpret_cast<const wchar_t*>(b.entry.name.data()),int(b.entry.name.size()),ignore)-CSTR_EQUAL;};
    if(!result)result=compareName(TRUE);if(!result)result=compareName(FALSE);
    if(!result)result=number(a.entry.version,b.entry.version);if(!result)result=number(a.id,b.id);
    return ascending?result:-result;
}
inline std::wstring DateText(uint64_t ticks) {
    if(!ticks)return {};auto time=FileTime(ticks);FILETIME local{};SYSTEMTIME s{};
    if(!FileTimeToLocalFileTime(&time,&local)||!FileTimeToSystemTime(&local,&s))return {};wchar_t text[40];
    swprintf_s(text,L"%04u-%02u-%02u %02u:%02u:%02u",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond);return text;
}
}
