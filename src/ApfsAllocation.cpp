// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsAllocation.h"
#include "ApfsTree.h"
#include "ApfsWriteDisk.h"
#include <algorithm>
#include <set>
#include <tuple>

namespace Apfs {
namespace {
using HfsPlus::Source;
using HfsPlus::Disk::Range;
using WriterDisk::Block;
using WriterDisk::Chunk;
using WriterDisk::ChunksPerCib;
using WriterDisk::Bytes;
using WriterDisk::Check;
using WriterDisk::DivideUp;
using WriterDisk::EncodeLeaf;
using WriterDisk::Set;
using OwnedRecord=WriterDisk::Record;
using Writes=std::vector<CheckpointBlockWrite>;
constexpr uint64_t Physical=0x40000000,Ephemeral=0x80000000;
constexpr size_t MainFreeQueue=240,PoolFreeQueue=200;

Bytes ReadBlock(Source& source,uint64_t block)
{
    return HfsPlus::Disk::Read(source,block*Block,Block);
}

bool Bit(const Bytes& map,uint64_t index)
{
    return (map[index/8]&(1u<<(index%8)))!=0;
}
void SetBit(Bytes& map,uint64_t index)
{
    map[index/8]|=uint8_t(1u<<(index%8));
}
template<class ReleaseBlock>
void Reclaim(const Bytes& space,const Bytes& queue,size_t field,uint64_t base,uint64_t limit,
             uint64_t committed,ReleaseBlock releaseBlock)
{
    Check(queue.size()==Block&&Le32(queue.data()+24)==(Ephemeral|2)&&Le32(queue.data()+28)==9&&
          Le16(queue.data()+32)==7&&Le16(queue.data()+34)==0&&(Le32(queue.data()+Block-40)&~2u)==0x0c,
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
        for(uint64_t n=0;n<count;++n)releaseBlock(block+n);
        reclaimed+=count;
    }
    Check(reclaimed==Le64(space.data()+field)&&oldest==Le64(space.data()+field+16),
          "APFS free queue counts disagree");
}

void Retire(Bytes& space,Bytes& queue,size_t field,std::vector<DataRun> ranges,uint64_t transaction)
{
    std::sort(ranges.begin(),ranges.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    std::vector<OwnedRecord> records;
    uint64_t retired=0;
    for(size_t i=0;i<ranges.size();){
        const auto first=ranges[i].block;
        uint64_t end=first;
        do{
            end+=ranges[i++].count;
            Check(i==ranges.size()||ranges[i].block>=end,"Overlapping APFS retired block ranges");
        }while(i<ranges.size()&&ranges[i].block==end);
        const auto count=end-first;
        OwnedRecord record{Bytes(16),Bytes(count==1?0:8)};
        Set(record.key,0,transaction,8);
        Set(record.key,8,first,8);
        if(count!=1)Set(record.value,0,count,8);
        records.push_back(std::move(record));
        retired+=count;
    }
    auto updated=EncodeLeaf(records,Le32(queue.data()+Block-40),16,8);
    std::copy_n(queue.begin(),size_t(32),updated.begin());
    queue=std::move(updated);
    Set(space,field,retired,8);
    Set(space,field+16,transaction,8);
}

struct ChunkBitmap {
    uint64_t size,available,oldBlock;
    Bytes bitmap;
    bool dirty=false;
};
struct ChunkInfoBlock {
    uint64_t address;
    Bytes bytes;
    bool dirty=false;
};

struct AllocationBitmaps {
    std::vector<ChunkBitmap> bitmaps;
    std::vector<ChunkInfoBlock> cibs;
    std::set<uint64_t> currentPoolBlocks;
    uint64_t available,pool,poolSize,currentBitmapBase;
    uint32_t cibOffset,bitmapSlots;
};

AllocationBitmaps ReadAllocationBitmaps(Source& source,const Container& container,const Bytes& space)
{
    const auto chunks=DivideUp(container.blockCount,Chunk);
    const auto cibCount=DivideUp(chunks,ChunksPerCib);
    Check(space.size()==Block&&Le32(space.data()+24)==(Ephemeral|5)&&Le32(space.data()+32)==Block&&
          Le32(space.data()+36)==Chunk&&Le32(space.data()+40)==ChunksPerCib&&Le64(space.data()+48)==container.blockCount&&
          Le64(space.data()+56)==chunks&&Le32(space.data()+64)==cibCount&&!Le32(space.data()+68)&&
          !Le64(space.data()+96)&&Le32(space.data()+160)==1,
          "Unsupported APFS space-manager layout");
    const auto cibOffset=Le32(space.data()+80);
    Check(cibOffset>=336&&Range(cibOffset,cibCount*8,space.size()),"APFS chunk address is missing");
    const auto pool=Le64(space.data()+176),poolSize=Le64(space.data()+152);
    const auto currentBitmapBase=Le64(space.data()+168);
    const auto bitmapSlots=Le32(space.data()+164);
    const auto mainLimit=(std::min)(container.blockCount,Chunk);
    Check(pool&&poolSize&&bitmapSlots>2&&bitmapSlots<UINT16_MAX&&
          currentBitmapBase&&Range(currentBitmapBase,bitmapSlots,mainLimit)&&currentBitmapBase+bitmapSlots<=pool&&
          Range(pool,poolSize,mainLimit),
          "Invalid APFS internal pool geometry");
    std::vector<ChunkBitmap> bitmaps;
    std::vector<ChunkInfoBlock> cibs;
    std::set<uint64_t> currentPoolBlocks;
    const auto trackPoolBlock=[&](uint64_t block){
        Check(block>=pool&&Range(block-pool,1,poolSize)&&currentPoolBlocks.insert(block).second,
              "Invalid or duplicate APFS chunk metadata block");
    };
    uint64_t available=0;
    for(uint64_t n=0;n<cibCount;++n){
        const auto address=Le64(space.data()+cibOffset+n*8);
        trackPoolBlock(address);
        auto cib=ReadObject(source,container,address,address,container.transaction,7,0,true);
        const auto first=n*ChunksPerCib;
        const auto count=(std::min)(ChunksPerCib,chunks-first);
        Check(Le32(cib.data()+32)==n&&Le32(cib.data()+36)==count,"Invalid APFS chunk-info block");
        uint64_t latestChunk=0;
        for(uint64_t i=0;i<count;++i){
            const auto record=cib.data()+40+i*32;
            const auto start=(first+i)*Chunk;
            const auto size=(std::min)(Chunk,container.blockCount-start);
            const auto free=Le32(record+20);
            const auto oldBlock=Le64(record+24);
            const auto xid=Le64(record);
            Check(xid&&xid<=Le64(cib.data()+16)&&Le64(record+8)==start&&Le32(record+16)==size&&free<=size,
                  "Invalid APFS chunk record");
            latestChunk=(std::max)(latestChunk,xid);
            if(oldBlock)trackPoolBlock(oldBlock);
            else Check(free==size,"APFS chunk is missing its allocation bitmap");
            auto bitmap=oldBlock?ReadBlock(source,oldBlock):Bytes(Block);
            if(oldBlock){
                uint64_t used=0;
                for(uint64_t bit=0;bit<Chunk;++bit){
                    if(!Bit(bitmap,bit))continue;
                    Check(bit<size,"APFS bitmap marks blocks beyond the container");
                    ++used;
                }
                Check(size-used==free,"APFS chunk bitmap and free count disagree");
            }
            available+=free;
            bitmaps.push_back({size,free,oldBlock,std::move(bitmap)});
        }
        Check(latestChunk==Le64(cib.data()+16),"APFS chunk-info block has a mismatched transaction ID");
        cibs.push_back({address,std::move(cib)});
    }
    Check(available==Le64(space.data()+72),"APFS space manager and chunk counts disagree");

    return {std::move(bitmaps),std::move(cibs),std::move(currentPoolBlocks),
            available,pool,poolSize,currentBitmapBase,cibOffset,bitmapSlots};
}


}

void StageObject(Writes& writes,uint64_t block,Bytes bytes,uint64_t oid,uint64_t tx,uint32_t type,uint32_t subtype)
{
    Set(bytes,8,oid,8);
    Set(bytes,16,tx,8);
    Set(bytes,24,type,4);
    Set(bytes,28,subtype,4);
    Set(bytes,0,ObjectChecksum(bytes),8);
    writes.push_back({block,std::move(bytes)});
}

std::vector<uint64_t> AllocateWriteBlocks(Writes& writes,Source& source,const Container& container,
                              Bytes& space,Bytes& superblock,const CheckpointObjectLookup& ephemeral,
                              const std::vector<uint64_t>& retire,const std::vector<uint64_t>& extentNodes,uint64_t snapshots,
                              const std::vector<FileRun>& files,std::vector<DataRun> retiredData,
                              uint64_t dataBlocks,std::vector<DataRun>& dataRuns,
                              uint64_t transaction,const std::function<size_t()>& requiredMetadata,
                              const std::vector<DataRun>& protectedRuns)
{
    auto [bitmaps,cibs,currentPoolBlocks,available,pool,poolSize,currentBitmapBase,cibOffset,bitmapSlots]=
        ReadAllocationBitmaps(source,container,space);


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
    Reclaim(space,poolQueue,PoolFreeQueue,pool,poolSize,container.transaction,[&](uint64_t block){
        const auto bit=block-pool;
        Check(Bit(poolMap,bit)&&!currentPoolBlocks.count(block),"APFS retired range overlaps free or live pool blocks");
        poolMap[bit/8]&=uint8_t(~(1u<<(bit%8)));
    });
    std::vector<uint64_t> internal;
    for(uint64_t i=0;i<poolSize;++i){
        const bool active=currentPoolBlocks.count(pool+i)!=0;
        Check(Bit(poolMap,i)==active,"APFS internal pool allocation is inconsistent");
        if(!active)internal.push_back(pool+i);
    }

    const auto allocated=[&](uint64_t block){return Bit(bitmaps[block/Chunk].bitmap,block%Chunk);};
    const auto verify=[&](uint64_t start,uint64_t count){
        Check(Range(start,count,container.blockCount),"APFS referenced block lies outside the container");
        for(uint64_t n=0;n<count;++n)Check(allocated(start+n),"APFS referenced block is marked free");
    };
    verify(0,1);
    verify(Le64(superblock.data()+112),Le32(superblock.data()+104));
    verify(Le64(superblock.data()+120),Le32(superblock.data()+108));
    verify(currentBitmapBase,bitmapSlots);
    verify(pool,poolSize);
    for(const auto block:retire){
        Check(block<pool||!Range(block-pool,1,poolSize),"APFS volume object overlaps the internal pool");
        verify(block,1);
    }
    for(const auto block:extentNodes)verify(block,1);
    verify(snapshots,1);

    const auto within=[](uint64_t block,uint64_t start,uint64_t count){
        return block>=start&&block-start<count;
    };
    const auto isMetadata=[&](uint64_t block){
        return !block||block==snapshots||
               std::find(extentNodes.begin(),extentNodes.end(),block)!=extentNodes.end()||
               within(block,Le64(superblock.data()+112),Le32(superblock.data()+104))||
               within(block,Le64(superblock.data()+120),Le32(superblock.data()+108))||
               within(block,currentBitmapBase,bitmapSlots)||within(block,pool,poolSize)||
               std::find(retire.begin(),retire.end(),block)!=retire.end();
    };
    for(const auto& run:files){
        verify(run.block,run.count);
        for(uint64_t i=0;i<run.count;++i)
            Check(!isMetadata(run.block+i),"APFS file extent overlaps live metadata");
    }
    const auto protectedBlock=[&](uint64_t block){
        const auto next=std::upper_bound(protectedRuns.begin(),protectedRuns.end(),block,
            [](uint64_t address,const DataRun& run){return address<run.block;});
        return next!=protectedRuns.begin()&&within(block,std::prev(next)->block,std::prev(next)->count);
    };
    for(const auto& run:protectedRuns){
        verify(run.block,run.count);
        for(uint64_t i=0;i<run.count;++i)
            Check(!isMetadata(run.block+i),"APFS sibling volume overlaps selected metadata");
    }
    for(const auto& run:files)
        for(uint64_t i=0;i<run.count;++i)
            Check(!protectedBlock(run.block+i),"APFS sibling volume overlaps selected data");
    Reclaim(space,mainQueue,MainFreeQueue,0,container.blockCount,container.transaction,[&](uint64_t block){
        const bool live=isMetadata(block)||protectedBlock(block)||std::any_of(files.begin(),files.end(),[&](const auto& run){
            return within(block,run.block,run.count);
        });
        Check(!live&&allocated(block),"APFS retired range overlaps free or live blocks");
        auto& chunk=bitmaps[block/Chunk];
        const auto bit=block%Chunk;
        chunk.bitmap[bit/8]&=uint8_t(~(1u<<(bit%8)));
        chunk.dirty=true;
        ++chunk.available;
        ++available;
    });
    Check(dataBlocks<=available,"Insufficient APFS space for file data");

    const auto reserve=[&](uint64_t block,uint64_t count){
        auto& chunk=bitmaps[block/Chunk];
        for(uint64_t i=0;i<count;++i)SetBit(chunk.bitmap,block%Chunk+i);
        chunk.dirty=true;
        chunk.available-=count;
        available-=count;
    };
    std::vector<DataRun> freeRuns;
    for(uint64_t chunk=0;chunk<bitmaps.size()&&dataBlocks;++chunk){
        const auto& bitmap=bitmaps[chunk];
        if(bitmap.available==bitmap.size){
            freeRuns.push_back({chunk*Chunk,bitmap.size});
            continue;
        }
        for(uint64_t i=0;i<bitmap.size;){
            if(Bit(bitmap.bitmap,i)){
                ++i;
                continue;
            }
            const auto start=i;
            do{++i;}while(i<bitmap.size&&!Bit(bitmap.bitmap,i));
            freeRuns.push_back({chunk*Chunk+start,i-start});
        }
    }

    // Prefer the longest runs to keep catalog growth small on fragmented images.
    if(dataBlocks){
        // Prefer higher addresses on ties to leave low blocks available for metadata.
        std::sort(freeRuns.begin(),freeRuns.end(),[](const auto& a,const auto& b){
            return std::tie(a.count,a.block)>std::tie(b.count,b.block);
        });
        auto remaining=dataBlocks;
        for(const auto& run:freeRuns){
            const auto count=(std::min)(run.count,remaining);
            dataRuns.push_back({run.block,count});
            reserve(run.block,count);
            remaining-=count;
            if(!remaining)break;
        }
        std::sort(dataRuns.begin(),dataRuns.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    }
    const auto metadataCount=requiredMetadata();
    Check(metadataCount<=available,"Insufficient APFS space for the catalog transaction");
    std::vector<uint64_t> result;
    for(uint64_t i=0;i<container.blockCount&&result.size()<metadataCount;++i){
        if(allocated(i))continue;
        result.push_back(i);
        reserve(i,1);
    }
    Check(result.size()==metadataCount,"APFS metadata allocation did not cover the transaction");

    // Only replace dirty bitmaps and their owning chunk-info blocks.
    std::vector<DataRun> retiredPool;
    size_t nextInternal=0;
    const auto allocateInternal=[&]{
        Check(nextInternal<internal.size(),"APFS internal pool is full");
        const auto block=internal[nextInternal++];
        SetBit(poolMap,block-pool);
        return block;
    };
    for(size_t i=0;i<bitmaps.size();++i){
        auto& chunk=bitmaps[i];
        if(!chunk.dirty)continue;
        uint64_t newBlock=0;
        if(chunk.available!=chunk.size){
            newBlock=allocateInternal();
            writes.push_back({newBlock,std::move(chunk.bitmap)});
        }
        if(chunk.oldBlock)retiredPool.push_back({chunk.oldBlock,1});
        auto& cib=cibs[i/ChunksPerCib];
        const auto at=40+32*(i%ChunksPerCib);
        Set(cib.bytes,at,transaction,8);
        Set(cib.bytes,at+20,chunk.available,4);
        Set(cib.bytes,at+24,newBlock,8);
        cib.dirty=true;
    }
    for(size_t i=0;i<cibs.size();++i){
        auto& cib=cibs[i];
        if(!cib.dirty)continue;
        const auto block=allocateInternal();
        retiredPool.push_back({cib.address,1});
        StageObject(writes,block,std::move(cib.bytes),block,transaction,Physical|7);
        Set(space,cibOffset+8*i,block,8);
    }
    // Deleted file blocks remain allocated until the new catalog is durable.
    for(const auto block:retire)retiredData.push_back({block,1});
    Retire(space,mainQueue,MainFreeQueue,std::move(retiredData),transaction);
    Retire(space,poolQueue,PoolFreeQueue,std::move(retiredPool),transaction);
    writes.push_back({currentBitmapBase+head,std::move(poolMap)});

    Set(space,72,available,8);
    Set(space,xidOffset,transaction,8);
    Set(space,bitmapOffset,head,2);
    Set(space,320,nextHead,2);
    Set(space,322,oldSlot,2);
    Set(space,nextOffset+head*2,UINT16_MAX,2);
    Set(space,nextOffset+tail*2,oldSlot,2);
    return result;
}

uint64_t VerifyAllocationBitmaps(Source& source,const Container& container,const Bytes& space)
{
    return ReadAllocationBitmaps(source,container,space).available*container.blockSize;
}

}
