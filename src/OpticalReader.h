// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "OpticalVolume.h"
#include "HfsPlusDisk.h"

namespace Optical {
using HfsPlus::Bytes;
using HfsPlus::Disk::Check;
using HfsPlus::Disk::Range;
constexpr uint64_t BlockSize=2048;
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
inline uint32_t Le32(const uint8_t* p) { return Le16(p) | uint32_t(Le16(p+2))<<16; }
inline uint64_t Le64(const uint8_t* p) { return Le32(p) | uint64_t(Le32(p+4))<<32; }

// Convert a local civil time to the browser's canonical UTC seconds since 1904.
inline std::optional<uint64_t> CatalogDate(const SYSTEMTIME& time,int offsetMinutes)
{
    FILETIME fileTime{};
    if(!SystemTimeToFileTime(&time,&fileTime))return {};
    const auto ticks=(uint64_t(fileTime.dwHighDateTime)<<32)|fileTime.dwLowDateTime;
    const auto seconds=int64_t(ticks/10000000)-9561628800LL-offsetMinutes*60;
    if(seconds<0)return {};
    return uint64_t(seconds);
}

struct Extent {
    uint64_t block,bytes;
    unsigned partition=0;
    bool sparse=false;
};

struct Node {
    MacBrowser::Entry entry;
    uint64_t size=0;
    uint32_t descriptorBlock=0;
    std::vector<Extent> extents;
    Bytes embedded;
    std::string address,details;
};

class Reader {
protected:
    HfsPlus::Source& source_;
    void Read(uint64_t at,void* output,size_t bytes) {
        Check(Range(at,bytes,source_.Size()),"Optical read exceeds the data track");
        source_.Read(at,output,bytes);
    }
    virtual void ReadExtent(const Extent&,uint64_t offset,void*,size_t)=0;
public:
    explicit Reader(HfsPlus::Source& source):source_(source){}
    virtual ~Reader()=default;
    virtual const char* Kind()const=0;
    virtual Node Root()const=0;
    virtual std::vector<Node> List(const Node&)=0;
    void Contents(const Node&,uint64_t offset,void*,size_t);
};
std::unique_ptr<Reader> OpenUdf(HfsPlus::Source&);
}
