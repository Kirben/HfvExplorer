// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsFormat.h"
#include "ApfsContainer.h"
#include "HfsPlusDisk.h"
#include "StorageWorker.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <string_view>
#pragma comment(lib,"bcrypt.lib")

namespace Apfs {
using HfsPlus::Bytes;
using HfsPlus::Source;
using HfsPlus::Disk::Check;

namespace {
// Apple File System Reference: the empty container has two object maps, a
// catalog, extent and snapshot trees, and one complete checkpoint. The space
// manager accounts for all reserved areas even when they contain only zeros.
constexpr uint64_t Block=4096,Chunk=8*Block,ChunksPerCib=(Block-40)/32,CibsPerCab=(Block-40)/8;
constexpr uint64_t Physical=0x40000000,Ephemeral=0x80000000,Tx=1;
constexpr uint64_t SpaceId=1024,ReaperId=1025,VolumeId=1026,CatalogId=1027,PoolQueueId=1028,MainQueueId=1029;
constexpr uint64_t DescBase=1,DescBlocks=8,DataBase=DescBase+DescBlocks,PoolBitmapVersions=16;

uint64_t DivideUp(uint64_t value,uint64_t unit){return value/unit+(value%unit!=0);}

void Put(uint8_t* at,uint64_t value,unsigned width)
{
    for(unsigned i=0;i<width;++i)at[i]=uint8_t(value>>(i*8));
}

void Set(Bytes& bytes,size_t at,uint64_t value,unsigned width){Put(bytes.data()+at,value,width);}

uint64_t MainQueueLimit(uint64_t blocks)
{
    uint64_t limit=512;
    if(blocks<0x40000)limit=1+(blocks-1)/4544;
    else if(blocks<0x100000)limit=116+(blocks-261281)/2272;
    return limit==2?3:limit;
}

struct Layout {
    uint64_t blocks,chunks,cibs,cabs,poolBlocks,poolBitmapBlocks,poolBitmapSlots,mainQueueLimit;
    uint64_t poolIndexOffset,poolNextOffset,devicesOffset,spaceBlocks,dataBlocks;
    uint64_t containerMap,volume,volumeMap,catalog,extents,snapshots,poolBitmap,pool,used;
    uint64_t mainBitmaps,firstCib,firstCab;
    explicit Layout(uint64_t size)
        :blocks(size/Block),chunks(DivideUp(blocks,Chunk)),cibs(DivideUp(chunks,ChunksPerCib)),
         cabs(cibs>CibsPerCab?DivideUp(cibs,CibsPerCab):0),poolBlocks(3*(chunks+cibs+cabs)),
         poolBitmapBlocks(DivideUp(poolBlocks,Chunk)),poolBitmapSlots(PoolBitmapVersions*poolBitmapBlocks),
         mainQueueLimit(MainQueueLimit(blocks)),poolIndexOffset(336+8*poolBitmapBlocks),
         poolNextOffset(poolIndexOffset+8*DivideUp(2*poolBitmapBlocks,8)),
         devicesOffset(poolNextOffset+2*poolBitmapSlots),
         spaceBlocks(DivideUp(devicesOffset+8*(cabs?cabs:cibs),Block)),
         dataBlocks((std::max)(uint64_t(64),4*(mainQueueLimit+spaceBlocks+8))),
         containerMap(DataBase+dataBlocks),volume(containerMap+2),volumeMap(volume+1),
         catalog(volumeMap+2),extents(catalog+1),snapshots(extents+1),
         poolBitmap(snapshots+1),pool(poolBitmap+poolBitmapSlots),used(pool+poolBlocks),
         mainBitmaps(DivideUp(used,Chunk)),firstCib(pool+mainBitmaps),firstCab(firstCib+cibs)
    {
        Check(used<blocks&&firstCab+cabs<=used&&poolBitmapSlots<UINT16_MAX&&
              3*(chunks+751)/1127-1<=UINT16_MAX&&cibs<=UINT32_MAX,
              "APFS image size exceeds the supported space-manager geometry");
    }
};

struct Record {Bytes key,value;};

Bytes Node(const std::vector<Record>& records,uint32_t flags,unsigned keyWidth=0,unsigned valueWidth=0)
{
    constexpr size_t Header=56,Footer=40,End=Block-Footer;
    const bool fixed=keyWidth!=0;
    const size_t tocSize=fixed?(Block-Header)/(4+keyWidth+valueWidth)*4:32*8;
    const size_t keyStart=Header+tocSize;
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
        values+=record.value.size();
        Set(node,toc,keys,2);
        if(fixed){
            Set(node,toc+2,values,2);
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

uint32_t DirectoryHash(std::string_view name)
{
    // APFS hashes normalized UTF-32LE. These two fixed system names are ASCII.
    uint32_t crc=UINT32_MAX;
    for(const auto c:name){
        crc^=uint8_t(c);
        for(unsigned bit=0;bit<32;++bit)crc=(crc>>1)^((crc&1)?0x82f63b78u:0);
    }
    return (crc&0x3fffff)<<10;
}

Record DirectoryEntry(std::string_view name,uint64_t inode,uint64_t now)
{
    Record entry{Bytes(13+name.size()),Bytes(18)};
    Set(entry.key,0,(uint64_t(9)<<60)|1,8);
    Set(entry.key,8,DirectoryHash(name)|(name.size()+1),4);
    std::copy(name.begin(),name.end(),entry.key.begin()+12);
    Set(entry.value,0,inode,8);
    Set(entry.value,8,now,8);
    Set(entry.value,16,4,2);
    return entry;
}

Record DirectoryInode(std::string_view name,uint64_t inode,uint64_t now)
{
    const size_t nameSpace=size_t(DivideUp(name.size()+1,8)*8);
    Record record{Bytes(8),Bytes(100+nameSpace)};
    Set(record.key,0,(uint64_t(3)<<60)|inode,8);
    Set(record.value,0,1,8);
    Set(record.value,8,inode,8);
    for(size_t field:{16,24,32,40})Set(record.value,field,now,8);
    Set(record.value,48,0x8000,8);
    Set(record.value,80,0040755,2);
    Set(record.value,92,1,2);
    Set(record.value,94,nameSpace,2);
    record.value[96]=4;
    record.value[97]=2;
    Set(record.value,98,name.size()+1,2);
    std::copy(name.begin(),name.end(),record.value.begin()+100);
    return record;
}

void RandomUuid(Bytes& bytes,size_t at)
{
    auto uuid=bytes.data()+at;
    Check(BCryptGenRandom(nullptr,uuid,16,BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Cannot generate an APFS UUID");
    uuid[6]=(uuid[6]&15)|0x40;
    uuid[8]=(uuid[8]&63)|0x80;
}

class Formatter {
    Source& out;
    Storage::Job& job;
    const Layout layout;
    const std::string label;
    const bool sensitive;
    uint64_t now;
    void Write(uint64_t block,const Bytes& bytes){
        if(job.cancelled())throw Storage::Cancelled();
        out.Write(block*Block,bytes.data(),bytes.size());
    }
    void WriteObject(uint64_t at,Bytes bytes,uint64_t oid,uint32_t type,uint32_t subtype=0){
        Set(bytes,8,oid,8);
        Set(bytes,16,Tx,8);
        Set(bytes,24,type,4);
        Set(bytes,28,subtype,4);
        Set(bytes,0,ObjectChecksum(bytes),8);
        Write(at,bytes);
    }
    void ObjectMap(uint64_t at,uint64_t oid,uint64_t physical,bool container){
        Bytes object(Block);
        Set(object,32,container?1:0,4);
        Set(object,40,Physical|2,4);
        Set(object,44,Physical|2,4);
        Set(object,48,at+1,8);
        WriteObject(at,std::move(object),at,Physical|11);
        Record mapping{Bytes(16),Bytes(16)};
        Set(mapping.key,0,oid,8);
        Set(mapping.key,8,Tx,8);
        Set(mapping.value,4,Block,4);
        Set(mapping.value,8,physical,8);
        WriteObject(at+1,Node({std::move(mapping)},0x10,16,16),at+1,Physical|2,11);
    }
    void Volume(){
        Bytes volume(Block);
        memcpy(volume.data()+32,"APSB",4);
        Set(volume,40,2,8); // Hard-link sibling map records.
        Set(volume,56,sensitive?8:1,8);
        Set(volume,88,5,8); // Volume object map and four tree roots.
        Set(volume,96,5,2);
        Set(volume,104,6,4);
        Set(volume,112,1,2);
        Set(volume,116,2,4);
        Set(volume,120,Physical|2,4);
        Set(volume,124,Physical|2,4);
        Set(volume,128,layout.volumeMap,8);
        Set(volume,136,CatalogId,8);
        Set(volume,144,layout.extents,8);
        Set(volume,152,layout.snapshots,8);
        Set(volume,176,16,8);
        RandomUuid(volume,240);
        Set(volume,264,1,8);
        memcpy(volume.data()+272,"HFVExplorer",11);
        Set(volume,304,now,8);
        Set(volume,312,Tx,8);
        std::copy(label.begin(),label.end(),volume.begin()+704);
        Set(volume,960,3,4);
        WriteObject(layout.volume,std::move(volume),VolumeId,13);

        std::vector<Record> catalog;
        if(DirectoryHash("private-dir")<DirectoryHash("root")){
            catalog.push_back(DirectoryEntry("private-dir",3,now));
            catalog.push_back(DirectoryEntry("root",2,now));
        }else{
            catalog.push_back(DirectoryEntry("root",2,now));
            catalog.push_back(DirectoryEntry("private-dir",3,now));
        }
        catalog.push_back(DirectoryInode("root",2,now));
        catalog.push_back(DirectoryInode("private-dir",3,now));
        WriteObject(layout.catalog,Node(catalog,0x40),CatalogId,2,14);
        WriteObject(layout.extents,Node({},0x50),layout.extents,Physical|2,15);
        WriteObject(layout.snapshots,Node({},0x50),layout.snapshots,Physical|2,16);
        ObjectMap(layout.volumeMap,CatalogId,layout.catalog,false);
    }
    void UsedRange(uint64_t address,uint64_t allocated){
        // These allocation bitmaps each mark a contiguous prefix in the new image.
        while(allocated){
            const auto count=(std::min)(allocated,Chunk);
            Bytes bitmap(Block);
            std::fill_n(bitmap.begin(),size_t(count/8),uint8_t(0xff));
            if(count%8)bitmap[size_t(count/8)]=uint8_t((1u<<(count%8))-1);
            Write(address++,bitmap);
            allocated-=count;
        }
    }
    void SpaceManager(){
        const auto& l=layout;
        Bytes space(size_t(l.spaceBlocks*Block));
        Set(space,32,Block,4);
        Set(space,36,Chunk,4);
        Set(space,40,ChunksPerCib,4);
        Set(space,44,CibsPerCab,4);
        Set(space,48,l.blocks,8);
        Set(space,56,l.chunks,8);
        Set(space,64,l.cibs,4);
        Set(space,68,l.cabs,4);
        Set(space,72,l.blocks-l.used,8);
        Set(space,80,l.devicesOffset,4);
        Set(space,128,l.devicesOffset+8*(l.cabs?l.cabs:l.cibs),4);
        Set(space,148,PoolBitmapVersions,4);
        Set(space,152,l.poolBlocks,8);
        Set(space,160,l.poolBitmapBlocks,4);
        Set(space,164,l.poolBitmapSlots,4);
        Set(space,168,l.poolBitmap,8);
        Set(space,176,l.pool,8);
        Set(space,208,PoolQueueId,8);
        const auto poolQueue=3*(l.chunks+751)/1127-1;
        Set(space,224,poolQueue==2?3:poolQueue,2);
        Set(space,248,MainQueueId,8);
        Set(space,264,l.mainQueueLimit,2);
        Set(space,320,l.poolBitmapBlocks,2);
        Set(space,322,l.poolBitmapSlots-1,2);
        Set(space,324,336,4);
        Set(space,328,l.poolIndexOffset,4);
        Set(space,332,l.poolNextOffset,4);
        for(uint64_t i=0;i<l.poolBitmapBlocks;++i){
            Set(space,336+8*i,Tx,8);
            Set(space,l.poolIndexOffset+2*i,i,2);
        }
        for(uint64_t i=0;i<l.poolBitmapSlots;++i)
            Set(space,l.poolNextOffset+2*i,i<l.poolBitmapBlocks||i+1==l.poolBitmapSlots?UINT16_MAX:i+1,2);

        for(uint64_t n=0;n<l.cibs;++n){
            Bytes info(Block);
            const auto first=n*ChunksPerCib;
            const auto count=(std::min)(l.chunks-first,ChunksPerCib);
            Set(info,32,n,4);Set(info,36,count,4);
            for(uint64_t i=0;i<count;++i){
                const auto chunk=first+i,start=chunk*Chunk,at=40+32*i;
                const auto blocks=(std::min)(l.blocks-start,Chunk);
                const auto used=start<l.used?(std::min)(l.used-start,blocks):0;
                Set(info,at,Tx,8);
                Set(info,at+8,start,8);
                Set(info,at+16,blocks,4);
                Set(info,at+20,blocks-used,4);
                if(used)Set(info,at+24,l.pool+chunk,8);
            }
            const auto at=l.firstCib+n;
            if(!l.cabs)Set(space,l.devicesOffset+n*8,at,8);
            WriteObject(at,std::move(info),at,Physical|7);
        }
        for(uint64_t n=0;n<l.cabs;++n){
            Bytes address(Block);
            const auto first=n*CibsPerCab,count=(std::min)(l.cibs-first,CibsPerCab);
            Set(address,32,n,4);Set(address,36,count,4);
            for(uint64_t i=0;i<count;++i)Set(address,40+8*i,l.firstCib+first+i,8);
            const auto at=l.firstCab+n;
            Set(space,l.devicesOffset+n*8,at,8);
            WriteObject(at,std::move(address),at,Physical|6);
        }
        UsedRange(l.pool,l.used);
        UsedRange(l.poolBitmap,l.mainBitmaps+l.cibs+l.cabs);
        WriteObject(DataBase+1,std::move(space),SpaceId,Ephemeral|5);
        WriteObject(DataBase+1+l.spaceBlocks,Node({},0x0c,16,8),PoolQueueId,Ephemeral|2,9);
        WriteObject(DataBase+2+l.spaceBlocks,Node({},0x0c,16,8),MainQueueId,Ephemeral|2,9);
    }
    void Checkpoint(){
        Bytes reaper(Block);
        Set(reaper,32,1,8);
        Set(reaper,64,1,4);
        Set(reaper,108,Block-112,4);
        WriteObject(DataBase,std::move(reaper),ReaperId,Ephemeral|17);

        Bytes mappings(Block);
        Set(mappings,32,1,4);
        Set(mappings,36,4,4);
        const auto add=[&](size_t index,uint64_t id,uint32_t type,uint32_t subtype,uint64_t at,uint64_t blocks){
            const auto pos=40+index*40;
            Set(mappings,pos,type,4);
            Set(mappings,pos+4,subtype,4);
            Set(mappings,pos+8,blocks*Block,4);
            Set(mappings,pos+24,id,8);
            Set(mappings,pos+32,at,8);
        };
        add(0,ReaperId,Ephemeral|17,0,DataBase,1);
        add(1,SpaceId,Ephemeral|5,0,DataBase+1,layout.spaceBlocks);
        add(2,PoolQueueId,Ephemeral|2,9,DataBase+1+layout.spaceBlocks,1);
        add(3,MainQueueId,Ephemeral|2,9,DataBase+2+layout.spaceBlocks,1);
        WriteObject(DescBase,std::move(mappings),DescBase,Physical|12);

        Bytes super(Block);
        memcpy(super.data()+32,"NXSB",4);
        Set(super,36,Block,4);
        Set(super,40,layout.blocks,8);
        Set(super,64,2,8);
        RandomUuid(super,72);
        Set(super,88,1124,8);
        Set(super,96,2,8);
        Set(super,104,DescBlocks,4);
        Set(super,108,layout.dataBlocks,4);
        Set(super,112,DescBase,8);
        Set(super,120,DataBase,8);
        Set(super,128,2,4);
        Set(super,132,layout.spaceBlocks+3,4);
        Set(super,140,2,4);
        Set(super,148,layout.spaceBlocks+3,4);
        Set(super,152,SpaceId,8);
        Set(super,160,layout.containerMap,8);
        Set(super,168,ReaperId,8);
        Set(super,180,(std::min)(uint64_t(100),DivideUp(out.Size(),512*1024*1024)),4);
        Set(super,184,VolumeId,8);
        const uint64_t minimum=out.Size()<128*1024*1024?layout.mainQueueLimit:8;
        Set(super,1312,(minimum<<32)|(uint64_t(4)<<16)|1,8);
        WriteObject(DescBase+1,super,1,Ephemeral|1);
        out.Flush();
        WriteObject(0,std::move(super),1,Ephemeral|1);
        out.Flush();
    }
public:
    Formatter(Source& image,Storage::Job& progress,std::string name,bool caseSensitive)
        :out(image),job(progress),layout(image.Size()),label(std::move(name)),sensitive(caseSensitive){
        FILETIME time;
        GetSystemTimeAsFileTime(&time);
        now=(((uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime)-116444736000000000ULL)*100;
    }
    void Run(){
        ObjectMap(layout.containerMap,VolumeId,layout.volume,true);
        Volume();
        SpaceManager();
        Checkpoint();
    }
};
}

std::string FormatLabel(const std::u16string& name)
{
    Check(!name.empty()&&name.find(u'\0')==name.npos&&name.find(u'/')==name.npos&&
          name.find(u':')==name.npos&&name.size()<=255,"Enter a valid APFS volume name");
    const auto data=reinterpret_cast<const wchar_t*>(name.data());
    const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,data,int(name.size()),nullptr,0,nullptr,nullptr);
    Check(size>0&&size<=255,"APFS volume names must fit in 255 UTF-8 bytes");
    std::string label(size,'\0');
    Check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,data,int(name.size()),label.data(),size,nullptr,nullptr)==size,
          "Cannot encode the APFS volume name");
    return label;
}

void Format(Source& out,const std::u16string& name,bool sensitive,Storage::Job& job)
{
    Check(out.Writable()&&out.Size()>=8*1024*1024&&out.Size()%Block==0&&out.Size()<=INT64_MAX,
          "APFS requires a new writable image of at least 8 MiB and a multiple of 4096 bytes");
    Formatter(out,job,FormatLabel(name),sensitive).Run();
}
}
