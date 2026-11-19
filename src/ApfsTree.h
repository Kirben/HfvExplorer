// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ApfsContainer.h"

namespace Apfs {
inline uint16_t Le16(const uint8_t* p){return uint16_t(p[0]|uint16_t(p[1])<<8);}
inline uint32_t Le32(const uint8_t* p){return Le16(p)|uint32_t(Le16(p+2))<<16;}
inline uint64_t Le64(const uint8_t* p){return Le32(p)|uint64_t(Le32(p+4))<<32;}

struct View {
    const uint8_t* data;
    size_t size;
};
struct Record {
    View key,value;
};
struct TreeLayout {
    uint32_t keySize=0,valueSize=0;
};

HfsPlus::Bytes ReadObject(HfsPlus::Source&,const Container&,uint64_t block,uint64_t oid,
                          uint64_t transaction,uint32_t type,uint32_t subtype=0,bool physical=false);

class Node {
public:
    Node(HfsPlus::Bytes,bool root,TreeLayout);
    unsigned Level()const{return level;}
    size_t Count()const{return locations.size();}
    TreeLayout Layout()const{return layout;}
    Record At(size_t i)const;
private:
    struct Location {size_t key,keySize,value,valueSize;};
    HfsPlus::Bytes bytes;
    TreeLayout layout;
    unsigned level;
    std::vector<Location> locations;
};

class ObjectMap {
public:
    ObjectMap(HfsPlus::Source&,const Container&,uint64_t block,uint64_t transaction);
    // Look up the last mapping at or before the selected transaction.
    std::optional<uint64_t> Lookup(uint64_t oid)const;
private:
    HfsPlus::Source& source;
    const Container& container;
    uint64_t tree,transaction;
};
}
