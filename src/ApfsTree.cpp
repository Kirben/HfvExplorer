// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsTree.h"
#include "ApfsCrypto.h"
#include "HfsPlusDisk.h"
#include <algorithm>

namespace Apfs {
using namespace HfsPlus;
using Disk::Check;
using Disk::Range;

Bytes ReadObject(Source& source,const Container& container,uint64_t block,uint64_t oid,
                 uint64_t transaction,uint32_t type,uint32_t subtype,bool physical,const VolumeCipher* cipher)
{
    Check(block&&block<container.blockCount,"APFS object lies outside its container");
    auto bytes=Disk::Read(source,container.offset+block*container.blockSize,container.blockSize);
    if(cipher)cipher->Decrypt(block*(container.blockSize/512),bytes.data(),bytes.size());
    const auto p=bytes.data();
    Check(Le64(p)==ObjectChecksum(bytes),"APFS object checksum mismatch");
    const uint32_t storage=physical?0x40000000:0;
    Check(Le64(p+8)==oid&&Le64(p+16)&&Le64(p+16)<=transaction&&
          Le32(p+24)==(storage|type|(cipher?0x10000000:0))&&Le32(p+28)==subtype,"Invalid APFS object header");
    return bytes;
}

Node::Node(Bytes data,bool root,TreeLayout sizes,bool allowGhosts):bytes(std::move(data)),layout(sizes)
{
    constexpr size_t header=56,footer=40;
    const auto p=bytes.data();
    const auto flags=Le16(p+32);
    level=Le16(p+34);
    const auto count=Le32(p+36);
    Check((flags&~7)==0&&bool(flags&1)==root&&bool(flags&2)==(level==0)&&
          (level==0||count>0),"Invalid or unsupported APFS B-tree node");
    const size_t valuesEnd=bytes.size()-(root?footer:0);
    if(root){
        const auto info=p+valuesEnd;
        Check(Le32(info+4)==bytes.size()&&!(Le32(info)&0x180),"Unsupported APFS B-tree layout");
        layout={Le32(info+8),Le32(info+12)};
    }
    const bool fixed=(flags&4)!=0;
    const size_t table=header+Le16(p+40),tableSize=Le16(p+42),stride=fixed?4:8;
    Check(Range(table,tableSize,valuesEnd)&&count<=tableSize/stride&&
          (!fixed||(layout.keySize&&layout.valueSize)),"Invalid APFS B-tree table");
    const size_t keys=table+tableSize;
    const size_t space=keys+Le16(p+44),spaceSize=Le16(p+46);
    Check(Range(space,spaceSize,valuesEnd),"Invalid APFS B-tree free-space bounds");
    locations.reserve(count);
    std::vector<std::pair<size_t,size_t>> occupied;
    occupied.reserve(size_t(count)*2);
    for(uint32_t i=0;i<count;++i){
        const auto entry=p+table+i*stride;
        const size_t key=keys+Le16(entry),keySize=fixed?layout.keySize:Le16(entry+2);
        const size_t back=Le16(entry+(fixed?2:4));
        const bool ghost=allowGhosts&&fixed&&!level&&back==UINT16_MAX;
        const size_t valueSize=ghost?0:fixed?(level?8:layout.valueSize):Le16(entry+6);
        Check(keySize&&Range(key,keySize,space)&&
              (ghost||(valueSize&&back>=valueSize&&back<=valuesEnd-(space+spaceSize))),
              "APFS B-tree record extends outside its data area");
        const auto value=ghost?0:valuesEnd-back;
        locations.push_back({key,keySize,value,valueSize});
        occupied.emplace_back(key,key+keySize);
        if(!ghost)occupied.emplace_back(value,value+valueSize);
    }
    std::sort(occupied.begin(),occupied.end());
    for(size_t i=1;i<occupied.size();++i)
        Check(occupied[i-1].second<=occupied[i].first,"Overlapping APFS B-tree records");
}
Record Node::At(size_t i)const
{
    const auto& loc=locations.at(i);
    return {{bytes.data()+loc.key,loc.keySize},{bytes.data()+loc.value,loc.valueSize}};
}

ObjectMapKey ReadObjectMapKey(View key)
{
    Check(key.size==16,"Invalid APFS object-map key size");
    const ObjectMapKey decoded{Le64(key.data),Le64(key.data+8)};
    Check(decoded.first&&decoded.second,"Invalid APFS object-map key");
    return decoded;
}

ObjectMap::ObjectMap(Source& s,const Container& c,uint64_t block,uint64_t tx,uint64_t treeTx)
    :source(s),container(c),transaction(tx),treeTransaction(treeTx?treeTx:tx)
{
    const auto object=ReadObject(source,container,block,block,treeTransaction,11,0,true);
    Check(Le32(object.data()+40)==0x40000002,"Unsupported APFS object-map tree type");
    tree=Le64(object.data()+48);
    changingEncryption=(Le32(object.data()+32)&0x0e)!=0;
}

std::optional<uint64_t> ObjectMap::Lookup(uint64_t oid)const
{
    const auto mapping=Find(oid);
    if(!mapping)return {};
    Check(!mapping->encrypted,"Unexpected encrypted APFS object mapping");
    return mapping->block;
}

std::optional<ObjectMap::Mapping> ObjectMap::Find(uint64_t oid)const
{
    using Key=ObjectMapKey;
    const Key sought{oid,transaction};
    uint64_t block=tree;
    bool root=true;
    unsigned level=0;
    TreeLayout layout;
    std::optional<Key> lower,upper;
    while(true){
        const Node node(ReadObject(source,container,block,block,treeTransaction,root?2:3,11,true),root,layout);
        Check(root||node.Level()+1==level,"Invalid APFS object-map level");
        level=node.Level();
        layout=node.Layout();
        Check(layout.keySize==16&&layout.valueSize==16,"Invalid APFS object-map layout");
        std::optional<Key> previous;
        std::optional<Record> found;
        Key match{};
        auto nextUpper=upper;
        for(size_t i=0;i<node.Count();++i){
            const auto record=node.At(i);
            Check(record.value.size==(level?8:16),"Invalid APFS object-map record");
            const auto key=ReadObjectMapKey(record.key);
            Check((!previous||*previous<key)&&(!lower||*lower<=key)&&(!upper||key<*upper),
                  "APFS object-map key lies outside its ordered range");
            if(key<=sought){found=record;match=key;}
            else if(found&&previous&&*previous==match)nextUpper=key;
            previous=key;
        }
        if(!found)return {};
        const auto value=found->value.data;
        if(level){block=Le64(value);root=false;lower=match;upper=nextUpper;continue;}
        if(match.first!=oid)return {};
        const auto flags=Le32(value);
        if(flags&1)return {};
        Check(!(flags&~6u)&&Le32(value+4)==container.blockSize,
              "Unsupported APFS object-map value");
        const auto physical=Le64(value+8);
        Check(physical&&physical<container.blockCount,"APFS object mapping lies outside the container");
        return Mapping{physical,bool(flags&4)};
    }
}
}
