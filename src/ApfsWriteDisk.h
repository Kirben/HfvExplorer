// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlusDisk.h"
#include <algorithm>
#include <string_view>

namespace Apfs::WriterDisk {
using HfsPlus::Bytes;
using HfsPlus::Disk::Check;
inline constexpr uint64_t Block=4096;
inline uint64_t DivideUp(uint64_t value,uint64_t unit){return value/unit+(value%unit!=0);}

inline void Put(uint8_t* at,uint64_t value,unsigned width)
{
    for(unsigned i=0;i<width;++i)at[i]=uint8_t(value>>(i*8));
}

inline void Set(Bytes& bytes,size_t at,uint64_t value,unsigned width){Put(bytes.data()+at,value,width);}

struct Record {Bytes key,value;};

inline Bytes EncodeLeaf(const std::vector<Record>& records,uint32_t flags,unsigned keyWidth=0,unsigned valueWidth=0)
{
    constexpr size_t Header=56,Footer=40,End=Block-Footer;
    const bool fixed=keyWidth!=0;
    const size_t tocSize=fixed?(Block-Header)/(4+keyWidth+valueWidth)*4:32*8;
    const size_t keyStart=Header+tocSize;
    Check(records.size()<=tocSize/(fixed?4:8),"APFS leaf record table is full");
    Bytes node(Block);
    Set(node,32,fixed?7:3,2);
    Set(node,36,records.size(),4);
    Set(node,42,tocSize,2);
    Set(node,48,UINT16_MAX,2);
    Set(node,52,UINT16_MAX,2);
    size_t keys=0,values=0,longestKey=keyWidth,longestValue=valueWidth;
    for(size_t i=0;i<records.size();++i){
        const auto& record=records[i];
        const size_t toc=Header+i*(fixed?4:8);
        const bool ghost=record.value.empty()&&fixed&&(flags&4);
        Check(!fixed||(record.key.size()==keyWidth&&(ghost||record.value.size()==valueWidth)),
              "Invalid fixed-width APFS leaf record");
        Check(keys+values+record.key.size()+record.value.size()<=End-keyStart,"APFS leaf is full");
        values+=record.value.size();
        Set(node,toc,keys,2);
        if(fixed){
            Set(node,toc+2,ghost?UINT16_MAX:values,2);
        }else{
            Set(node,toc+2,record.key.size(),2);
            Set(node,toc+4,values,2);
            Set(node,toc+6,record.value.size(),2);
        }
        std::copy(record.key.begin(),record.key.end(),node.begin()+keyStart+keys);
        std::copy(record.value.begin(),record.value.end(),node.begin()+End-values);
        keys+=record.key.size();
        longestKey=(std::max)(longestKey,record.key.size());
        longestValue=(std::max)(longestValue,record.value.size());
    }
    Set(node,44,keys,2);
    Set(node,46,End-keyStart-keys-values,2);
    Set(node,End,flags,4);
    Set(node,End+4,Block,4);
    Set(node,End+8,keyWidth,4);
    Set(node,End+12,valueWidth,4);
    Set(node,End+16,longestKey,4);
    Set(node,End+20,longestValue,4);
    Set(node,End+24,records.size(),8);
    Set(node,End+32,1,8);
    return node;
}

inline uint32_t DirectoryHash(std::string_view name,bool fold=false)
{
    // APFS hashes normalized UTF-32LE. These encoders accept ASCII names.
    uint32_t crc=UINT32_MAX;
    for(const auto c:name){
        crc^=uint8_t(fold&&c>='A'&&c<='Z'?c+32:c);
        for(unsigned bit=0;bit<32;++bit)crc=(crc>>1)^((crc&1)?0x82f63b78u:0);
    }
    return (crc&0x3fffff)<<10;
}

inline Record DirectoryEntry(std::string_view name,uint64_t inode,uint64_t now,uint64_t parent=1,bool fold=false,uint16_t type=4)
{
    Record entry{Bytes(13+name.size()),Bytes(18)};
    Set(entry.key,0,(uint64_t(9)<<60)|parent,8);
    Set(entry.key,8,DirectoryHash(name,fold)|(name.size()+1),4);
    std::copy(name.begin(),name.end(),entry.key.begin()+12);
    Set(entry.value,0,inode,8);
    Set(entry.value,8,now,8);
    Set(entry.value,16,type,2);
    return entry;
}

inline Record Inode(std::string_view name,uint64_t inode,uint64_t now,uint64_t parent,
                    uint16_t mode,uint32_t linksOrChildren,uint64_t dataSize=0)
{
    const size_t nameSpace=size_t(DivideUp(name.size()+1,8)*8);
    const size_t dataField=dataSize?4:0;
    Record record{Bytes(8),Bytes(100+dataField+nameSpace+(dataSize?40:0))};
    Set(record.key,0,(uint64_t(3)<<60)|inode,8);
    Set(record.value,0,parent,8);
    Set(record.value,8,inode,8);
    for(size_t field:{16,24,32,40})Set(record.value,field,now,8);
    Set(record.value,48,0x8000,8);
    Set(record.value,56,linksOrChildren,4);
    Set(record.value,80,mode,2);
    Set(record.value,92,dataSize?2:1,2);
    Set(record.value,94,nameSpace+(dataSize?40:0),2);
    record.value[96]=4;
    record.value[97]=2;
    Set(record.value,98,name.size()+1,2);
    std::copy(name.begin(),name.end(),record.value.begin()+100+dataField);
    if(dataSize){
        record.value[100]=8;
        record.value[101]=0x20; // System field.
        Set(record.value,102,40,2);
        const size_t stream=104+nameSpace;
        Set(record.value,stream,dataSize,8);
        Set(record.value,stream+8,DivideUp(dataSize,Block)*Block,8);
    }
    return record;
}

}
