// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsCatalogWrite.h"
#include "ApfsTree.h"
#include "ApfsWriteDisk.h"
#include <windows.h>

namespace Apfs {
namespace {
using HfsPlus::Source;
using HfsPlus::Disk::Range;
using WriterDisk::Block;
using WriterDisk::Bytes;
using WriterDisk::Check;
using WriterDisk::DivideUp;
using WriterDisk::EncodeLeaf;
using WriterDisk::Set;
using OwnedRecord=WriterDisk::Record;
using Writes=std::vector<CheckpointBlockWrite>;
constexpr uint64_t Physical=0x40000000,Ephemeral=0x80000000,Chunk=Block*8;
constexpr uint64_t IdMask=(uint64_t(1)<<60)-1;
constexpr size_t MainFreeQueue=240,PoolFreeQueue=200;

std::string DirectoryName(const std::u16string& name)
{
    Check(!name.empty()&&name.size()<=255&&name!=u"."&&name!=u"..","Invalid APFS directory name");
    std::string result;
    for(const auto c:name){
        Check(c>=32&&c<127&&c!='/'&&c!=':',"APFS catalog updates currently require an ASCII directory name");
        result+=char(c);
    }
    return result;
}

Bytes ReadBlock(Source& source,uint64_t block)
{
    return HfsPlus::Disk::Read(source,block*Block,Block);
}

void StageObject(Writes& writes,uint64_t block,Bytes bytes,uint64_t oid,uint64_t tx,uint32_t type,uint32_t subtype=0)
{
    Set(bytes,8,oid,8);
    Set(bytes,16,tx,8);
    Set(bytes,24,type,4);
    Set(bytes,28,subtype,4);
    Set(bytes,0,ObjectChecksum(bytes),8);
    writes.push_back({block,std::move(bytes)});
}

bool Bit(const Bytes& map,uint64_t index)
{
    return (map[index/8]&(1u<<(index%8)))!=0;
}
void SetBit(Bytes& map,uint64_t index)
{
    map[index/8]|=uint8_t(1u<<(index%8));
}
void VerifyAllocated(const Bytes& map,uint64_t start,uint64_t size,uint64_t limit)
{
    Check(Range(start,size,limit),"APFS referenced block lies outside the writable allocation chunk");
    for(uint64_t n=0;n<size;++n)Check(Bit(map,start+n),"APFS referenced block is marked free");
}

struct MapLeaf {
    uint64_t headerBlock,rootBlock,physical;
    Bytes header;
};
MapLeaf ReadSingleMapping(Source& source,const Container& container,uint64_t block,uint64_t wanted)
{
    auto header=ReadObject(source,container,block,block,container.transaction,11,0,true);
    const auto root=Le64(header.data()+48);
    Check(Le32(header.data()+40)==(Physical|2)&&!Le32(header.data()+36)&&!Le64(header.data()+56),
          "APFS object map contains unsupported snapshot references");
    const Node node(ReadObject(source,container,root,root,container.transaction,2,11,true),true,{});
    Check(node.Level()==0&&node.Count()==1,"APFS writes require a single-leaf object map");
    const auto record=node.At(0);
    Check(record.key.size==16&&record.value.size==16&&Le64(record.key.data)==wanted&&
          Le64(record.key.data+8)&&Le64(record.key.data+8)<=container.transaction&&
          Le32(record.value.data)==0&&Le32(record.value.data+4)==Block,
          "Invalid APFS writable object mapping");
    return {block,root,Le64(record.value.data+8),std::move(header)};
}

void StageMap(Writes& writes,MapLeaf map,uint64_t headerBlock,uint64_t rootBlock,
              uint64_t oid,uint64_t physical,uint64_t transaction)
{
    Set(map.header,48,rootBlock,8);
    StageObject(writes,headerBlock,std::move(map.header),headerBlock,transaction,Physical|11);
    OwnedRecord mapping{Bytes(16),Bytes(16)};
    Set(mapping.key,0,oid,8);
    Set(mapping.key,8,transaction,8);
    Set(mapping.value,4,Block,4);
    Set(mapping.value,8,physical,8);
    StageObject(writes,rootBlock,EncodeLeaf({std::move(mapping)},0x10,16,16),rootBlock,transaction,Physical|2,11);
}

template<class Protected>
uint64_t Reclaim(const Bytes& space,const Bytes& queue,size_t field,Bytes& bitmap,
                 uint64_t base,uint64_t limit,uint64_t committed,Protected protectedBlock)
{
    Check(queue.size()==Block&&Le32(queue.data()+24)==(Ephemeral|2)&&Le32(queue.data()+28)==9&&
          Le16(queue.data()+32)==7&&Le16(queue.data()+34)==0&&Le32(queue.data()+Block-40)==0x0c,
          "Unsupported APFS free queue");
    const Node tree(queue,true,{},true);
    Check(tree.Layout().keySize==16&&tree.Layout().valueSize==8,"Invalid APFS free queue record layout");
    uint64_t reclaimed=0,oldest=0;
    std::pair<uint64_t,uint64_t> previous{};
    for(size_t i=0;i<tree.Count();++i){
        const auto record=tree.At(i);
        const auto xid=Le64(record.key.data),block=Le64(record.key.data+8);
        const auto count=record.value.size?Le64(record.value.data):1;
        const auto key=std::make_pair(xid,block);
        Check(xid&&xid<=committed&&key>previous&&count&&block>=base&&Range(block-base,count,limit),
              "Invalid APFS retired block range");
        if(!oldest)oldest=xid;
        previous=key;
        // The selected checkpoint no longer references these blocks. Its
        // allocation metadata remains intact until publication succeeds.
        for(uint64_t n=0;n<count;++n){
            const auto bit=block-base+n;
            Check(Bit(bitmap,bit)&&!protectedBlock(block+n),"APFS retired range overlaps free or live blocks");
            bitmap[bit/8]&=uint8_t(~(1u<<(bit%8)));
        }
        reclaimed+=count;
    }
    Check(reclaimed==Le64(space.data()+field)&&oldest==Le64(space.data()+field+16),
          "APFS free queue counts disagree");
    return reclaimed;
}

void Retire(Bytes& space,Bytes& queue,size_t field,std::vector<uint64_t> blocks,uint64_t transaction)
{
    std::sort(blocks.begin(),blocks.end());
    Check(std::adjacent_find(blocks.begin(),blocks.end())==blocks.end(),"Duplicate APFS retired block");
    std::vector<OwnedRecord> runs;
    for(size_t i=0;i<blocks.size();){
        size_t end=i+1;
        while(end<blocks.size()&&blocks[end]==blocks[end-1]+1)++end;
        OwnedRecord record{Bytes(16),Bytes(end==i+1?0:8)};
        Set(record.key,0,transaction,8);
        Set(record.key,8,blocks[i],8);
        if(!record.value.empty())Set(record.value,0,end-i,8);
        runs.push_back(std::move(record));
        i=end;
    }
    auto updated=EncodeLeaf(runs,0x0c,16,8);
    std::copy_n(queue.begin(),size_t(32),updated.begin());
    queue=std::move(updated);
    Set(space,field,blocks.size(),8);
    Set(space,field+16,transaction,8);
}

std::vector<uint64_t> Allocate(Writes& writes,Source& source,const Container& container,
                              Bytes& space,Bytes& superblock,const CheckpointObjectLookup& ephemeral,
                              const std::vector<uint64_t>& retire,uint64_t extents,uint64_t snapshots,
                              uint64_t transaction)
{
    const auto chunks=DivideUp(container.blockCount,Chunk);
    Check(space.size()==Block&&Le32(space.data()+24)==(Ephemeral|5)&&Le32(space.data()+32)==Block&&
          Le32(space.data()+36)==Chunk&&Le64(space.data()+48)==container.blockCount&&
          Le64(space.data()+56)==chunks&&Le32(space.data()+64)==1&&!Le32(space.data()+68)&&
          !Le64(space.data()+96)&&Le32(space.data()+160)==1,
          "Unsupported APFS space-manager layout");
    const auto cibOffset=Le32(space.data()+80);
    Check(cibOffset>=336&&Range(cibOffset,8,space.size()),"APFS chunk address is missing");
    const auto oldCib=Le64(space.data()+cibOffset),pool=Le64(space.data()+176),poolSize=Le64(space.data()+152);
    const auto currentBitmapBase=Le64(space.data()+168);
    const auto bitmapSlots=Le32(space.data()+164);
    const auto mainLimit=(std::min)(container.blockCount,Chunk);
    Check(pool&&poolSize&&bitmapSlots>2&&bitmapSlots<UINT16_MAX&&
          currentBitmapBase&&Range(currentBitmapBase,bitmapSlots,mainLimit)&&currentBitmapBase+bitmapSlots<=pool&&
          Range(pool,poolSize,mainLimit)&&oldCib>=pool&&Range(oldCib-pool,1,poolSize),
          "Invalid APFS internal pool geometry");
    auto cib=ReadObject(source,container,oldCib,oldCib,container.transaction,7,0,true);
    Check(Le32(cib.data()+32)==0&&Le32(cib.data()+36)==chunks&&chunks<=(Block-40)/32&&
          Le64(cib.data()+48)==0&&Le32(cib.data()+56)==mainLimit,
          "Invalid APFS chunk information");
    const auto oldMainBitmap=Le64(cib.data()+64);
    Check(oldMainBitmap!=oldCib&&oldMainBitmap>=pool&&Range(oldMainBitmap-pool,1,poolSize),
          "Invalid APFS allocation bitmap location");
    for(uint64_t i=1;i<chunks;++i){
        const auto record=cib.data()+40+i*32;
        const auto blocks=(std::min)(Chunk,container.blockCount-i*Chunk);
        Check(Le64(record+8)==i*Chunk&&Le32(record+16)==blocks&&Le32(record+20)==blocks&&!Le64(record+24),
              "APFS catalog writes currently require all later allocation chunks to be empty");
    }

    const auto xidOffset=Le32(space.data()+324),bitmapOffset=Le32(space.data()+328),nextOffset=Le32(space.data()+332);
    Check(xidOffset>=336&&Range(xidOffset,8,space.size())&&bitmapOffset>=xidOffset+8&&
          Range(bitmapOffset,2,space.size())&&nextOffset>=bitmapOffset+2&&
          Range(nextOffset,bitmapSlots*2,space.size())&&cibOffset>=nextOffset+bitmapSlots*2,
          "Invalid APFS internal bitmap offsets");
    const auto oldSlot=Le16(space.data()+bitmapOffset),head=Le16(space.data()+320),tail=Le16(space.data()+322);
    Check(oldSlot<bitmapSlots&&head<bitmapSlots&&tail<bitmapSlots&&oldSlot!=head&&oldSlot!=tail&&head!=tail&&
          Le16(space.data()+nextOffset+oldSlot*2)==UINT16_MAX&&
          Le16(space.data()+nextOffset+tail*2)==UINT16_MAX,"Invalid APFS internal bitmap free list");
    const auto nextHead=Le16(space.data()+nextOffset+head*2);
    Check(nextHead<bitmapSlots&&nextHead!=oldSlot&&nextHead!=head,"Invalid APFS internal bitmap successor");
    // Reject corrupt chains before relying on any slot reported as available.
    size_t count=0;
    for(auto slot=head;slot!=UINT16_MAX;slot=Le16(space.data()+nextOffset+slot*2)){
        Check(slot<bitmapSlots&&slot!=oldSlot&&++count<bitmapSlots,"Invalid APFS internal bitmap free list");
        if(Le16(space.data()+nextOffset+slot*2)==UINT16_MAX)
            Check(slot==tail&&count==bitmapSlots-1,"Incomplete APFS internal bitmap free list");
    }

    auto& mainQueue=ephemeral(Le64(space.data()+MainFreeQueue+8));
    auto& poolQueue=ephemeral(Le64(space.data()+PoolFreeQueue+8));
    auto poolMap=ReadBlock(source,currentBitmapBase+oldSlot);
    Reclaim(space,poolQueue,PoolFreeQueue,poolMap,pool,poolSize,container.transaction,[&](uint64_t block){
        return block==oldCib||block==oldMainBitmap;
    });
    std::vector<uint64_t> internal;
    size_t used=0;
    for(uint64_t i=0;i<poolSize;++i){
        if(Bit(poolMap,i))++used;
        else if(internal.size()<2)internal.push_back(pool+i);
    }
    Check(used==2&&internal.size()==2&&Bit(poolMap,oldCib-pool)&&Bit(poolMap,oldMainBitmap-pool),
          "APFS internal pool allocation is inconsistent");
    auto mainMap=ReadBlock(source,oldMainBitmap);
    VerifyAllocated(mainMap,0,1,mainLimit);
    VerifyAllocated(mainMap,Le64(superblock.data()+112),Le32(superblock.data()+104),mainLimit);
    VerifyAllocated(mainMap,Le64(superblock.data()+120),Le32(superblock.data()+108),mainLimit);
    VerifyAllocated(mainMap,currentBitmapBase,bitmapSlots,mainLimit);
    VerifyAllocated(mainMap,pool,poolSize,mainLimit);
    for(const auto block:retire){
        Check(block<pool||!Range(block-pool,1,poolSize),"APFS volume object overlaps the internal pool");
        VerifyAllocated(mainMap,block,1,mainLimit);
    }
    VerifyAllocated(mainMap,extents,1,mainLimit);
    VerifyAllocated(mainMap,snapshots,1,mainLimit);

    const auto within=[](uint64_t block,uint64_t start,uint64_t count){
        return block>=start&&block-start<count;
    };
    const auto reclaimed=Reclaim(space,mainQueue,MainFreeQueue,mainMap,0,mainLimit,container.transaction,[&](uint64_t block){
        return !block||block==extents||block==snapshots||
               within(block,Le64(superblock.data()+112),Le32(superblock.data()+104))||
               within(block,Le64(superblock.data()+120),Le32(superblock.data()+108))||
               within(block,currentBitmapBase,bitmapSlots)||within(block,pool,poolSize)||
               std::find(retire.begin(),retire.end(),block)!=retire.end();
    });
    std::vector<uint64_t> result;
    uint64_t available=0;
    for(uint64_t i=0;i<mainLimit;++i){
        if(Bit(mainMap,i))continue;
        ++available;
        if(result.size()<retire.size())result.push_back(i);
    }
    Check(available>=reclaimed&&available-reclaimed==Le32(cib.data()+60)&&
          available-reclaimed+(container.blockCount-mainLimit)==Le64(space.data()+72)&&
          result.size()==retire.size(),"APFS allocation counts disagree or the active chunk is full");

    Retire(space,mainQueue,MainFreeQueue,retire,transaction);
    Retire(space,poolQueue,PoolFreeQueue,{oldCib,oldMainBitmap},transaction);

    for(const auto block:result)SetBit(mainMap,block);
    for(const auto block:internal)SetBit(poolMap,block-pool);
    writes.push_back({internal[0],std::move(mainMap)});
    writes.push_back({currentBitmapBase+head,std::move(poolMap)});
    Set(cib,40,transaction,8);
    Set(cib,60,available-result.size(),4);
    Set(cib,64,internal[0],8);
    StageObject(writes,internal[1],std::move(cib),internal[1],transaction,Physical|7);
    Set(space,cibOffset,internal[1],8);
    Set(space,72,available+(container.blockCount-mainLimit)-result.size(),8);
    Set(space,xidOffset,transaction,8);
    Set(space,bitmapOffset,head,2);
    Set(space,320,nextHead,2);
    Set(space,322,oldSlot,2);
    Set(space,nextOffset+head*2,UINT16_MAX,2);
    Set(space,nextOffset+tail*2,oldSlot,2);
    return result;
}

Bytes AddDirectory(Bytes catalog,const std::string& name,uint64_t id,uint64_t time,bool fold,
                   uint64_t directoryCount)
{
    const Node tree(std::move(catalog),true,{});
    Check(tree.Level()==0&&directoryCount<=tree.Count()&&tree.Count()==4+2*directoryCount,
          "APFS catalog writes currently require a single leaf containing only root folders");
    // The record keys sort first by inode ID, then by record type.
    const bool privateFirst=WriterDisk::DirectoryHash("private-dir")<WriterDisk::DirectoryHash("root");
    const uint64_t reserved[]={privateFirst?3u:2u,privateFirst?2u:3u};
    const auto added=WriterDisk::DirectoryEntry(name,id,time,2,fold);
    const uint32_t newHash=Le32(added.key.data()+8)>>10;
    const size_t privateIndex=3+size_t(directoryCount);
    size_t insertAt=3;
    uint32_t previousHash=0;
    uint64_t previousInode=1;
    std::vector<uint64_t> children;
    std::vector<OwnedRecord> records;
    for(size_t i=0;i<tree.Count();++i){
        const auto view=tree.At(i);
        if(i<2){
            const auto expected=WriterDisk::DirectoryEntry(reserved[i]==2?"root":"private-dir",reserved[i],0);
            Check(view.key.size==expected.key.size()&&!memcmp(view.key.data,expected.key.data(),view.key.size)&&
                  view.value.size==18&&Le64(view.value.data)==reserved[i]&&Le16(view.value.data+16)==4,
                  "Invalid APFS reserved directory entry");
        }else if(i>2&&i<privateIndex){
            Check(view.key.size>=14&&Le64(view.key.data)==((uint64_t(9)<<60)|2)&&
                  view.value.size==18&&Le16(view.value.data+16)==4,"Invalid APFS root directory entry");
            const auto key=Le32(view.key.data+8);
            Check((key&1023u)==view.key.size-12&&view.key.data[view.key.size-1]==0,
                  "Invalid APFS root directory name");
            const auto begin=view.key.data+12;
            const auto childName=DirectoryName(std::u16string(begin,view.key.data+view.key.size-1));
            const auto hash=key>>10;
            Check((WriterDisk::DirectoryHash(childName,fold)>>10)==hash&&(i==3||hash>previousHash),
                  "Unsupported APFS root directory key order");
            Check(hash!=newHash,"The APFS directory name already exists or has a conflicting hash");
            if(hash<newHash)++insertAt;
            previousHash=hash;
            const auto child=Le64(view.value.data);
            Check(child>=16&&child<id,"Invalid APFS root directory entry ID");
            children.push_back(child);
        }else{
            const bool userDirectory=i>privateIndex;
            Check(view.key.size==8&&view.value.size>=92,"Invalid APFS directory inode size");
            const auto key=Le64(view.key.data);
            const auto inode=key&IdMask;
            const bool validId=userDirectory?inode>=16:inode==(i==2?2u:3u);
            Check((key>>60)==3&&validId&&inode>previousInode&&inode<id&&
                  Le64(view.value.data)==(userDirectory?2:1)&&Le64(view.value.data+8)==inode&&
                  Le32(view.value.data+56)==(i==2?directoryCount:0)&&
                  (Le16(view.value.data+80)&0170000)==0040000,"Invalid APFS directory inode");
            if(userDirectory){
                if(i==privateIndex+1)std::sort(children.begin(),children.end());
                Check(inode==children[i-privateIndex-1],"APFS directory entry and inode disagree");
            }
            previousInode=inode;
        }
        records.push_back({Bytes(view.key.data,view.key.data+view.key.size),
                           Bytes(view.value.data,view.value.data+view.value.size)});
    }
    Set(records[2].value,24,time,8);
    Set(records[2].value,32,time,8);
    Set(records[2].value,56,directoryCount+1,4);
    records.insert(records.begin()+insertAt,added);
    records.push_back(WriterDisk::DirectoryInode(name,id,time,2));
    return EncodeLeaf(records,0x40);
}
}

std::vector<CheckpointBlockWrite> PrepareRootDirectory(
    Source& source,const Container& current,uint64_t transaction,Bytes& superblock,
    const CheckpointObjectLookup& ephemeral,const std::u16string& name)
{
    Check(current.blockSize==Block,"APFS catalog writes currently require 4096-byte blocks");
    const auto ascii=DirectoryName(name);
    const auto oid=current.volumes.front();
    const auto containerMap=ReadSingleMapping(source,current,current.objectMap,oid);
    auto volume=ReadObject(source,current,containerMap.physical,oid,current.transaction,13);
    const auto flags=Le64(volume.data()+56);
    const auto directoryCount=Le64(volume.data()+192);
    const auto catalogOid=Le64(volume.data()+136);
    const auto extents=Le64(volume.data()+144),snapshots=Le64(volume.data()+152);
    Check(!memcmp(volume.data()+32,"APSB",4)&&!memcmp(volume.data()+272,"HFVExplorer",12)&&
          (flags==1||flags==8)&&Le64(volume.data()+264)==1&&Le32(volume.data()+116)==2&&
          !Le64(volume.data()+184)&&!Le64(volume.data()+200)&&
          !Le64(volume.data()+208)&&!Le64(volume.data()+216),
          "APFS catalog writes currently require an unencrypted volume formatted by HFVExplorer");
    const Node extentTree(ReadObject(source,current,extents,extents,current.transaction,2,15,true),true,{});
    const Node snapshotTree(ReadObject(source,current,snapshots,snapshots,current.transaction,2,16,true),true,{});
    Check(!extentTree.Level()&&!extentTree.Count()&&!snapshotTree.Level()&&!snapshotTree.Count(),
          "APFS catalog writes currently require empty extent and snapshot trees");
    const auto volumeMap=ReadSingleMapping(source,current,Le64(volume.data()+128),catalogOid);
    const auto nextId=Le64(volume.data()+176);
    Check(nextId>=16&&nextId<IdMask,"Invalid APFS next inode ID");
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    const auto time=(((uint64_t(fileTime.dwHighDateTime)<<32)|fileTime.dwLowDateTime)-116444736000000000ULL)*100;
    auto catalog=AddDirectory(ReadObject(source,current,volumeMap.physical,catalogOid,current.transaction,2,14),
                              ascii,nextId,time,flags==1,directoryCount);
    auto& space=ephemeral(Le64(superblock.data()+152));
    Writes writes;
    const auto allocated=Allocate(writes,source,current,space,superblock,ephemeral,
        {containerMap.headerBlock,containerMap.rootBlock,containerMap.physical,
         volumeMap.headerBlock,volumeMap.rootBlock,volumeMap.physical},extents,snapshots,transaction);
    StageObject(writes,allocated[0],std::move(catalog),catalogOid,transaction,2,14);
    StageMap(writes,volumeMap,allocated[1],allocated[2],catalogOid,allocated[0],transaction);
    Set(volume,128,allocated[1],8);
    Set(volume,176,nextId+1,8);
    Set(volume,192,directoryCount+1,8);
    Set(volume,256,time,8);
    // Replacing the catalog and its object map retires three volume blocks.
    Check(Le64(volume.data()+224)<=UINT64_MAX-3&&Le64(volume.data()+232)<=UINT64_MAX-3,
          "APFS volume allocation history overflow");
    Set(volume,224,Le64(volume.data()+224)+3,8);
    Set(volume,232,Le64(volume.data()+232)+3,8);
    StageObject(writes,allocated[3],std::move(volume),oid,transaction,13);
    StageMap(writes,containerMap,allocated[4],allocated[5],oid,allocated[3],transaction);
    Set(superblock,160,allocated[4],8);
    return writes;
}
}
