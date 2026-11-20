// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsCatalogWrite.h"
#include "ApfsTree.h"
#include "ApfsWriteDisk.h"
#include <windows.h>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>

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
struct FileRun {
    uint64_t owner,block,count;
    uint64_t stream=0;
};
constexpr uint64_t Physical=0x40000000,Ephemeral=0x80000000;
constexpr uint64_t IdMask=(uint64_t(1)<<60)-1;
constexpr size_t MainFreeQueue=240,PoolFreeQueue=200;

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
struct Mapping {
    uint64_t oid,block;
};
struct WritableMap {
    uint64_t headerBlock,rootBlock;
    Bytes header;
    std::vector<Mapping> mappings;
    std::vector<uint64_t> leaves;
};
WritableMap ReadWritableMap(Source& source,const Container& container,uint64_t block)
{
    auto header=ReadObject(source,container,block,block,container.transaction,11,0,true);
    const auto root=Le64(header.data()+48);
    Check(Le32(header.data()+40)==(Physical|2)&&!Le32(header.data()+36)&&!Le64(header.data()+56),
          "APFS object map contains unsupported snapshot references");
    const Node node(ReadObject(source,container,root,root,container.transaction,2,11,true),true,{});
    Check(node.Level()<=1&&node.Layout().keySize==16&&node.Layout().valueSize==16,
          "APFS writes require a supported object map");
    WritableMap map{block,root,std::move(header),{},{}};
    std::set<uint64_t> physicalBlocks{block,root};
    uint64_t previous=0;
    const auto append=[&](const Node& leaf){
        Check(!leaf.Level()&&leaf.Count(),"Invalid APFS object map leaf");
        for(size_t i=0;i<leaf.Count();++i){
            const auto record=leaf.At(i);
            const auto [oid,xid]=ReadObjectMapKey(record.key);
            Check(record.value.size==16,"Invalid APFS writable object-map record size");
            const auto physical=Le64(record.value.data+8);
            Check(oid>previous&&xid<=container.transaction&&Le32(record.value.data)==0&&
                  Le32(record.value.data+4)==Block&&physical&&physical<container.blockCount&&
                  physicalBlocks.insert(physical).second,"Invalid APFS writable object mapping");
            map.mappings.push_back({oid,physical});
            previous=oid;
        }
    };
    if(!node.Level())append(node);
    else{
        Check(node.Count()>0,"Empty APFS object map index");
        for(size_t i=0;i<node.Count();++i){
            const auto separator=node.At(i);
            Check(separator.key.size==16&&separator.value.size==8,"Invalid APFS object map separator");
            const auto child=Le64(separator.value.data);
            Check(physicalBlocks.insert(child).second,"Repeated APFS object map leaf");
            const Node leaf(ReadObject(source,container,child,child,container.transaction,3,11,true),false,node.Layout());
            Check(leaf.Count()&&!memcmp(leaf.At(0).key.data,separator.key.data,16),
                  "APFS object map separator does not match its leaf");
            map.leaves.push_back(child);
            append(leaf);
        }
    }
    return map;
}

constexpr size_t MapLeafCapacity=(Block-WriterDisk::NodeHeader-WriterDisk::TreeFooter)/(4+16+16);
size_t MapLeaves(size_t mappings)
{
    return mappings<=MapLeafCapacity?0:size_t(DivideUp(mappings,MapLeafCapacity));
}

void StageMap(Writes& writes,WritableMap map,uint64_t headerBlock,uint64_t rootBlock,
              const std::vector<Mapping>& mappings,uint64_t transaction,
              const std::vector<uint64_t>& allocation,size_t firstLeaf)
{
    Set(map.header,48,rootBlock,8);
    StageObject(writes,headerBlock,std::move(map.header),headerBlock,transaction,Physical|11);
    std::vector<OwnedRecord> records;
    for(const auto& item:mappings){
        OwnedRecord mapping{Bytes(16),Bytes(16)};
        Set(mapping.key,0,item.oid,8);
        Set(mapping.key,8,transaction,8);
        Set(mapping.value,4,Block,4);
        Set(mapping.value,8,item.block,8);
        records.push_back(std::move(mapping));
    }
    const auto count=MapLeaves(records.size());
    if(!count){
        StageObject(writes,rootBlock,EncodeLeaf(records,0x10,16,16),rootBlock,transaction,Physical|2,11);
        return;
    }
    std::vector<OwnedRecord> index;
    for(size_t i=0;i<count;++i){
        const auto begin=i*MapLeafCapacity,end=(std::min)(begin+MapLeafCapacity,records.size());
        const auto child=allocation[firstLeaf+i];
        OwnedRecord separator{records[begin].key,Bytes(8)};
        Set(separator.value,0,child,8);
        index.push_back(std::move(separator));
        const std::vector<OwnedRecord> leaf(records.begin()+begin,records.begin()+end);
        StageObject(writes,child,WriterDisk::EncodeNode(leaf,0x10,false,0,16,16),child,transaction,Physical|3,11);
    }
    auto node=WriterDisk::EncodeNode(index,0x10,true,1,16,8);
    constexpr auto footer=Block-WriterDisk::TreeFooter;
    Set(node,footer+12,16,4);
    Set(node,footer+20,16,4);
    Set(node,footer+24,records.size(),8);
    Set(node,footer+32,count+1,8);
    StageObject(writes,rootBlock,std::move(node),rootBlock,transaction,Physical|2,11);
}

struct CatalogRecords {
    std::vector<OwnedRecord> records;
    std::vector<uint64_t> blocks;
};
CatalogRecords ReadWritableCatalog(Source& source,const Container& container,const WritableMap& map,uint64_t oid)
{
    const auto find=[&](uint64_t wanted){
        const auto item=std::lower_bound(map.mappings.begin(),map.mappings.end(),wanted,
            [](const auto& entry,uint64_t id){return entry.oid<id;});
        Check(item!=map.mappings.end()&&item->oid==wanted,"Missing APFS catalog object mapping");
        return item->block;
    };
    CatalogRecords catalog;
    const auto append=[&](const Node& leaf){
        Check(!leaf.Level()&&leaf.Count(),"Invalid APFS writable catalog leaf");
        for(size_t i=0;i<leaf.Count();++i){
            const auto record=leaf.At(i);
            catalog.records.push_back({Bytes(record.key.data,record.key.data+record.key.size),
                                       Bytes(record.value.data,record.value.data+record.value.size)});
        }
    };
    const auto rootBlock=find(oid);
    const Node root(ReadObject(source,container,rootBlock,oid,container.transaction,2,14),true,{});
    Check(root.Level()<=2&&!root.Layout().keySize&&!root.Layout().valueSize,
          "APFS catalog writes currently support at most three tree levels");
    catalog.blocks.push_back(rootBlock);
    std::set<uint64_t> seen{oid};
    const auto visit=[&](const auto& visit,const Node& node)->void{
        if(!node.Level()){
            append(node);
            return;
        }
        Check(node.Count()>0,"Empty APFS catalog index");
        for(size_t i=0;i<node.Count();++i){
            const auto record=node.At(i);
            Check(record.value.size==8,"Invalid APFS catalog index record");
            const auto child=Le64(record.value.data);
            Check(seen.insert(child).second,"Repeated APFS catalog child");
            const auto block=find(child);
            const Node next(ReadObject(source,container,block,child,container.transaction,3,14),false,{});
            Check(next.Level()+1==node.Level()&&next.Count(),"Invalid APFS catalog child level or count");
            const auto first=next.At(0).key;
            Check(first.size==record.key.size&&!memcmp(first.data,record.key.data,first.size),
                  "APFS catalog separator does not match its first child key");
            catalog.blocks.push_back(block);
            visit(visit,next);
        }
    };
    visit(visit,root);
    Check(catalog.blocks.size()==map.mappings.size(),"APFS volume map has unrelated live objects");
    return catalog;
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


template<class RequiredMetadata>
std::vector<uint64_t> Allocate(Writes& writes,Source& source,const Container& container,
                              Bytes& space,Bytes& superblock,const CheckpointObjectLookup& ephemeral,
                              const std::vector<uint64_t>& retire,const std::vector<uint64_t>& extentNodes,uint64_t snapshots,
                              const std::vector<FileRun>& files,std::vector<DataRun> retiredData,
                              uint64_t dataBlocks,std::vector<DataRun>& dataRuns,
                              uint64_t transaction,RequiredMetadata requiredMetadata)
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
    Reclaim(space,mainQueue,MainFreeQueue,0,container.blockCount,container.transaction,[&](uint64_t block){
        const bool live=isMetadata(block)||std::any_of(files.begin(),files.end(),[&](const auto& run){
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

OwnedRecord StreamReference(uint64_t inode)
{
    OwnedRecord record{Bytes(8),Bytes(4)};
    Set(record.key,0,(uint64_t(6)<<60)|inode,8);
    Set(record.value,0,1,4);
    return record;
}

OwnedRecord FileExtent(const FileRun& run,uint64_t logicalBlock)
{
    OwnedRecord record{Bytes(16),Bytes(24)};
    Set(record.key,0,(uint64_t(8)<<60)|(run.stream?run.stream:run.owner),8);
    Set(record.key,8,logicalBlock*Block,8);
    Set(record.value,0,run.count*Block,8);
    Set(record.value,8,run.block,8);
    return record;
}

OwnedRecord ExtentReference(const FileRun& run)
{
    OwnedRecord record{Bytes(8),Bytes(20)};
    Set(record.key,0,(uint64_t(2)<<60)|run.block,8);
    Set(record.value,0,(uint64_t(1)<<60)|run.count,8);
    Set(record.value,8,run.owner,8);
    Set(record.value,16,1,4);
    return record;
}

bool Matches(View view,const Bytes& expected)
{
    return view.size==expected.size()&&!memcmp(view.data,expected.data(),view.size);
}

CatalogRecords ReadExtentTree(Source& source,const Container& container,uint64_t block)
{
    CatalogRecords tree;
    tree.blocks.push_back(block);
    const Node root(ReadObject(source,container,block,block,container.transaction,2,15,true),true,{});
    Check(root.Level()<=1&&!root.Layout().keySize&&!root.Layout().valueSize,
          "Unsupported APFS extent reference tree layout");
    uint64_t previousEnd=0;
    const auto append=[&](const Node& leaf){
        Check(!leaf.Level(),"Invalid APFS extent reference leaf level");
        for(size_t i=0;i<leaf.Count();++i){
            const auto record=leaf.At(i);
            Check(record.key.size==8&&record.value.size==20,"Invalid APFS extent reference record size");
            const auto key=Le64(record.key.data),length=Le64(record.value.data);
            const auto start=key&IdMask,count=length&IdMask;
            Check(key>>60==2&&length>>60==1&&start&&count&&start>=previousEnd&&
                  Range(start,count,container.blockCount)&&Le32(record.value.data+16)>0,
                  "Invalid or overlapping APFS physical extent range");
            previousEnd=start+count;
            tree.records.push_back({Bytes(record.key.data,record.key.data+8),Bytes(record.value.data,record.value.data+20)});
        }
    };
    if(!root.Level())append(root);
    else{
        Check(root.Count()>0,"Empty APFS extent reference index");
        std::set<uint64_t> seen{block};
        for(size_t i=0;i<root.Count();++i){
            const auto separator=root.At(i);
            Check(separator.key.size==8&&separator.value.size==8,"Invalid APFS extent reference index key");
            const auto child=Le64(separator.value.data);
            Check(seen.insert(child).second,"Repeated APFS extent reference leaf");
            const Node leaf(ReadObject(source,container,child,child,container.transaction,3,15,true),false,{});
            Check(leaf.Count()>0,"Empty APFS extent reference leaf");
            const auto first=leaf.At(0).key;
            Check(first.size==8&&!memcmp(first.data,separator.key.data,8),"Invalid APFS extent reference separator");
            append(leaf);
            tree.blocks.push_back(child);
        }
    }
    return tree;
}

void ValidateExtentReferences(const CatalogRecords& tree,std::vector<FileRun>& files)
{
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    Check(tree.records.size()==files.size(),"APFS extent reference count disagrees with the catalog");
    uint64_t end=0;
    for(size_t i=0;i<files.size();++i){
        const auto& run=files[i];
        const auto& record=tree.records[i];
        const auto expected=ExtentReference(run);
        Check(run.block>=end&&record.key==expected.key&&record.value==expected.value,
              "Invalid, shared or overlapping APFS extent reference");
        end=run.block+run.count;
    }
}

constexpr auto ResourceAttribute="com.apple.ResourceFork";
constexpr auto CompressionAttribute="com.apple.decmpfs";
constexpr auto SymlinkAttribute="com.apple.fs.symlink";
constexpr uint64_t HasUncompressedSize=0x40000;
constexpr uint32_t BsdCompressed=0x20;
void SetCompressionFlags(Bytes& inode,uint64_t size,bool compressed)
{
    const auto flags=Le64(inode.data()+48)&~HasUncompressedSize;
    Set(inode,48,flags|(compressed?HasUncompressedSize:0),8);
    const auto bsd=Le32(inode.data()+68)&~BsdCompressed;
    Set(inode,68,bsd|(compressed?BsdCompressed:0),4);
    Set(inode,84,compressed?size:0,8);
}
constexpr uint64_t HasResourceFork=0x4000,NoResourceFork=0x8000;
void SetResourceFlag(Bytes& inode,bool present)
{
    const auto flags=Le64(inode.data()+48)&~(HasResourceFork|NoResourceFork);
    Set(inode,48,flags|(present?HasResourceFork:NoResourceFork),8);
}
constexpr uint64_t HasSecurityAttribute=0x40;
constexpr auto SecurityAttribute=u"com.apple.system.Security";
void SetSecurityFlag(Bytes& inode,const HfsPlus::Attributes& attributes)
{
    const auto flags=Le64(inode.data()+48)&~HasSecurityAttribute;
    Set(inode,48,flags|(attributes.count(SecurityAttribute)?HasSecurityAttribute:0),8);
}
bool OrdinaryAttribute(std::string_view name)
{
    return name!="com.apple.FinderInfo"&&name!="com.apple.ResourceFork"&&name!="com.apple.decmpfs"&&
           name!="com.apple.fs.symlink";
}

OwnedRecord EmbeddedAttribute(uint64_t inode,std::string_view name,size_t size)
{
    OwnedRecord record{Bytes(11+name.size()),Bytes(4+size)};
    Set(record.key,0,(uint64_t(4)<<60)|inode,8);
    Set(record.key,8,name.size()+1,2);
    std::copy(name.begin(),name.end(),record.key.begin()+10);
    Set(record.value,0,2,2);
    Set(record.value,2,size,2);
    return record;
}

void SortAttributes(std::vector<OwnedRecord>& records)
{
    std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){
        return std::lexicographical_compare(a.key.begin()+10,a.key.end(),b.key.begin()+10,b.key.end());
    });
}
using AttributeStreams=std::map<std::u16string,uint64_t>;
class AttributeInput final:public Source {
    const HfsPlus::AttributeData& value;
public:
    explicit AttributeInput(const HfsPlus::AttributeData& data):value(data){}
    uint64_t Size()const override{return value.size();}
    bool Writable()const override{return false;}
    void Read(uint64_t offset,void* data,size_t size)override{value.Read(offset,data,size);}
    void Write(uint64_t,const void*,size_t)override{throw HfsPlus::Error("APFS attribute source is read-only");}
    void Flush()override{}
};
OwnedRecord StreamAttribute(uint64_t inode,std::string_view name,uint64_t id,uint64_t size)
{
    auto record=EmbeddedAttribute(inode,name,48);
    Set(record.value,0,1,2);
    Set(record.value,4,id,8);
    Set(record.value,12,size,8);
    Set(record.value,20,DivideUp(size,Block)*Block,8);
    return record;
}
struct ResourceWrite {
    Source* input;
    uint64_t size,id;
};
std::vector<OwnedRecord> EncodeAttributes(uint64_t inode,const HfsPlus::Attributes* attributes,
                                          const ResourceWrite* resource,const AttributeStreams& streams)
{
    std::vector<OwnedRecord> records;
    if(attributes){
        ValidateWritableAttributes(*attributes);
        for(const auto& [wide,value]:*attributes){
            const auto name=EncodeAttributeName(wide);
            const auto stream=streams.find(wide);
            if(stream!=streams.end())records.push_back(StreamAttribute(inode,name,stream->second,value.size()));
            else{
                Check(value.size()<EmbeddedAttributeLimit,"Missing APFS attribute stream allocation");
                auto record=EmbeddedAttribute(inode,name,size_t(value.size()));
                if(value.size())value.Read(0,record.value.data()+4,size_t(value.size()));
                records.push_back(std::move(record));
            }
        }
    }
    if(resource&&resource->size){
        if(resource->id)records.push_back(StreamAttribute(inode,ResourceAttribute,resource->id,resource->size));
        else{
            auto record=EmbeddedAttribute(inode,ResourceAttribute,size_t(resource->size));
            resource->input->Read(0,record.value.data()+4,size_t(resource->size));
            records.push_back(std::move(record));
        }
    }
    return records;
}

OwnedRecord LinkTargetRecord(uint64_t inode,const Bytes& path)
{
    Check(path.size()>=2&&path.size()<EmbeddedAttributeLimit&&path.back()==0&&
          !memchr(path.data(),0,path.size()-1),"Invalid APFS symbolic link target");
    auto record=EmbeddedAttribute(inode,SymlinkAttribute,path.size());
    Set(record.value,0,6,2);
    std::copy(path.begin(),path.end(),record.value.begin()+4);
    return record;
}

void WriteCompressionAttribute(std::vector<OwnedRecord>& attributes,Bytes& inode,
                               uint64_t id,uint64_t dataSize,const Bytes* compression)
{
    uint64_t logicalSize=0;
    if(compression){
        Check(compression->size()>=16&&compression->size()<EmbeddedAttributeLimit&&
              Le32(compression->data())==0x636d7066&&!dataSize,"Invalid APFS compressed file output");
        auto record=EmbeddedAttribute(id,CompressionAttribute,compression->size());
        std::copy(compression->begin(),compression->end(),record.value.begin()+4);
        attributes.push_back(std::move(record));
        logicalSize=Le64(compression->data()+8);
    }
    SetCompressionFlags(inode,logicalSize,compression!=nullptr);
}

void SetSibling(Bytes& value,uint64_t sibling)
{
    value.resize(34);
    Set(value,18,1,2);
    Set(value,20,8,2);
    value[22]=1;
    Set(value,24,8,2);
    Set(value,26,sibling,8);
}

struct CatalogSibling {
    uint64_t inode,parent;
    std::string name;
    std::optional<size_t> linkRecord,mapRecord;
    size_t directoryRecord=0;

    OwnedRecord LinkRecord(uint64_t id)const
    {
        OwnedRecord record{Bytes(16),Bytes(11+name.size())};
        Set(record.key,0,(uint64_t(5)<<60)|inode,8);
        Set(record.key,8,id,8);
        Set(record.value,0,parent,8);
        Set(record.value,8,name.size()+1,2);
        std::copy(name.begin(),name.end(),record.value.begin()+10);
        return record;
    }
    OwnedRecord MapRecord(uint64_t id)const
    {
        OwnedRecord record{Bytes(8),Bytes(8)};
        Set(record.key,0,(uint64_t(12)<<60)|id,8);
        Set(record.value,0,inode,8);
        return record;
    }
};

std::vector<OwnedRecord> EditCatalog(std::vector<OwnedRecord> records,const CatalogMutation& request,const std::string& name,
    uint64_t nextId,uint64_t entryId,uint64_t time,bool fold,uint64_t directoryCount,uint64_t fileCount,uint64_t symlinkCount,
    uint64_t dataSize,uint64_t blockCount,std::vector<FileRun>& fileRuns,std::optional<CatalogKind>& removedKind,
    size_t& streamAt,const std::optional<FileDates>& dates,const ResourceWrite* resourceWrite,const AttributeStreams& streams,std::set<uint64_t>& retiredStreams)
{
    Check(directoryCount<=records.size()&&fileCount<=records.size(),"Invalid APFS catalog entry counts");
    const auto at=[&](size_t i){
        const auto& record=records[i];
        return Record{{record.key.data(),record.key.size()},{record.value.data(),record.value.size()}};
    };
    const auto parent=std::visit([](const auto& edit){return edit.parent;},request);
    const auto* create=std::get_if<CatalogCreate>(&request);
    const auto* rename=std::get_if<CatalogRename>(&request);
    const auto* remove=std::get_if<CatalogDelete>(&request);
    const auto* replace=std::get_if<CatalogReplaceData>(&request);
    const auto* metadata=std::get_if<CatalogMetadata>(&request);
    const auto* attributesEdit=std::get_if<CatalogAttributes>(&request);
    const auto* resourceEdit=std::get_if<CatalogResourceFork>(&request);
    const auto* linkEdit=std::get_if<CatalogReplaceSymlink>(&request);
    const auto copyDates=[&](Bytes& inode){
        if(!dates)return;
        Set(inode,16,CatalogTimestamp(dates->created),8);
        Set(inode,24,CatalogTimestamp(dates->modified),8);
    };
    const auto editDates=[&](Bytes& inode){
        if(metadata->dates.created)Set(inode,16,CatalogTimestamp(*metadata->dates.created),8);
        if(metadata->dates.modified)Set(inode,24,CatalogTimestamp(*metadata->dates.modified),8);
    };
    const bool privateFirst=WriterDisk::DirectoryHash("private-dir")<WriterDisk::DirectoryHash("root");
    const uint64_t reserved[]={privateFirst?3u:2u,privateFirst?2u:3u};
    // APFS orders equal-hash directory keys by their stored names.
    using Key=std::tuple<uint64_t,uint64_t,uint32_t,std::string>;
    const auto requestedName=NormalizedName(name,fold);
    std::map<uint64_t,std::set<std::u32string>> directoryNames;
    OwnedRecord added;
    std::optional<Key> addedKey;
    const auto* hardLink=std::get_if<CatalogHardLink>(&request);
    if(create||rename||hardLink){
        added=WriterDisk::DirectoryEntry(name,entryId,time,parent,fold,
                                         uint16_t(create?create->kind:hardLink?CatalogKind::RegularFile:CatalogKind::Directory));
        addedKey=Key{parent,9,Le32(added.key.data()+8)>>10,name};
    }
    Key previous{};
    size_t insertAt=2;
    struct Directory {
        size_t record;
        uint32_t children=0;
    };
    std::map<uint64_t,Directory> directories;
    const auto readFinder=[](const uint8_t* value,size_t size)->std::optional<FinderBytes>{
        if(!(Le64(value+48)&WriterDisk::HasFinderInfo))return {};
        FinderBytes finder{};
        Check(size>=100+WriterDisk::FinderFieldSize,"Truncated APFS Finder information");
        std::copy_n(value+size-finder.size(),finder.size(),finder.begin());
        return finder;
    };
    struct Link {
        uint64_t parent;
        CatalogKind kind;
        std::string name;
        size_t entryRecord;
        size_t inodeRecord=0,attributes=0,streamRecords=0;
        uint64_t dataSize=0;
        std::optional<FinderBytes> finder;
        std::optional<size_t> resourceRecord,compressionRecord;
    };
    std::map<uint64_t,Link> links;
    std::optional<size_t> selectedRecord,replacedRecord;
    uint64_t selectedSibling=0;
    std::map<uint64_t,CatalogSibling> siblings;
    std::map<uint64_t,std::set<uint64_t>> siblingGroups;
    for(size_t i=2;i<records.size();++i){
        const auto item=at(i);
        if(item.key.size<14||Le64(item.key.data)>>60!=9||item.value.size!=34)continue;
        const auto sibling=Le64(item.value.data+26);
        const auto inode=Le64(item.value.data);
        Bytes expected(item.value.data,item.value.data+18);
        SetSibling(expected,sibling);
        Check(sibling>inode&&sibling<nextId&&Matches(item.value,expected),
              "Invalid APFS sibling directory record");
        const auto name=reinterpret_cast<const char*>(item.key.data+12);
        Check(siblings.emplace(sibling,CatalogSibling{inode,Le64(item.key.data)&IdMask,
              std::string(name,item.key.size-13),{},{},i}).second,"Repeated APFS sibling ID");
        siblingGroups[inode].insert(sibling);
    }
    // A promoted regular-file name can be in a folder with a later object ID.
    for(size_t i=2;i<records.size();++i){
        const auto item=at(i);
        if(item.key.size<14||Le64(item.key.data)>>60!=9||
           (item.value.size!=18&&item.value.size!=34)||
           Le16(item.value.data+16)!=uint16_t(CatalogKind::RegularFile))continue;
        const auto inode=Le64(item.value.data);
        if(item.value.size==34&&Le64(item.value.data+26)!=*siblingGroups.at(inode).begin())continue;
        Check(links.emplace(inode,Link{Le64(item.key.data)&IdMask,CatalogKind::RegularFile,
              std::string(reinterpret_cast<const char*>(item.key.data+12),item.key.size-13),i}).second,
              "Repeated APFS primary file name");
    }
    struct AttributeStream {uint64_t inode,blocks;};
    std::map<uint64_t,AttributeStream> attributeStreams;
    uint64_t files=0,symlinks=0;
    for(size_t i=0;i<records.size();++i){
        const auto view=at(i);
        size_t streamRecords=0;
        bool renamingThisEntry=false;
        if(i<2){
            const auto expected=WriterDisk::DirectoryEntry(reserved[i]==2?"root":"private-dir",reserved[i],0);
            Check(view.key.size==expected.key.size()&&!memcmp(view.key.data,expected.key.data(),view.key.size)&&
                  view.value.size==18&&Le64(view.value.data)==reserved[i]&&Le16(view.value.data+16)==4,
                  "Invalid APFS reserved directory entry");
        }else{
            Check(view.key.size>=8,"Invalid APFS directory key");
            const auto key=Le64(view.key.data);
            const auto inode=key&IdMask,type=key>>60;
            Check(type==12||!siblings.count(inode),"APFS sibling ID is also used by another catalog object");
            uint32_t hash=0;
            std::string storedName;
            if(type==9){
                const auto folder=directories.find(inode);
                Check(inode!=3&&folder!=directories.end()&&view.key.size>=14&&
                      (view.value.size==18||view.value.size==34),"Invalid APFS directory entry");
                const auto kind=CatalogKind(Le16(view.value.data+16));
                Check(kind==CatalogKind::Directory||kind==CatalogKind::RegularFile||kind==CatalogKind::SymbolicLink,
                      "APFS catalog contains an unsupported entry type");
                const auto nameKey=Le32(view.key.data+8);
                Check((nameKey&1023u)==view.key.size-12&&view.key.data[view.key.size-1]==0,
                      "Invalid APFS entry name");
                auto childName=std::string(reinterpret_cast<const char*>(view.key.data+12),view.key.size-13);
                Check(childName.size()<=255&&childName.find_first_of("/:")==childName.npos&&
                      childName.find('\0')==childName.npos&&childName!="."&&childName!="..",
                      "Invalid APFS directory name");
                hash=nameKey>>10;
                Check((WriterDisk::DirectoryHash(childName,fold)>>10)==hash,"Invalid APFS directory name hash");
                const auto child=Le64(view.value.data);
                const auto group=siblingGroups.find(child);
                const auto sibling=view.value.size==34?Le64(view.value.data+26):0;
                const auto primary=group==siblingGroups.end()?0:*group->second.begin();
                const auto aliasId=rename?rename->alias:remove?remove->alias:0;
                const bool selected=(rename||remove)&&entryId==child&&parent==inode&&
                                    sibling==(aliasId?aliasId:primary);
                renamingThisEntry=rename&&selected;
                if(selected){
                    selectedRecord=i;
                    selectedSibling=sibling;
                }
                const auto canonicalName=NormalizedName(childName,fold);
                Check(directoryNames[inode].insert(canonicalName).second,"Duplicate APFS directory name");
                if(hardLink&&hardLink->replaced==child&&parent==inode&&canonicalName==requestedName)
                    replacedRecord=i;
                Check(!addedKey||parent!=inode||renamingThisEntry||replacedRecord==i||canonicalName!=requestedName,
                      "The APFS entry name already exists");
                storedName=childName;
                const bool alias=view.value.size==34;
                Check(child>=16&&child<nextId&&alias==(group!=siblingGroups.end())&&
                      (!alias||(kind==CatalogKind::RegularFile&&group->second.size()>=2)),
                      "Invalid APFS directory entry ID or sibling group");
                if(kind!=CatalogKind::RegularFile){
                    Check(child>inode&&links.emplace(child,Link{inode,kind,std::move(childName),i}).second,
                          "Invalid or duplicate APFS primary directory entry");
                }
                ++folder->second.children;
            }else if(type==12){
                const auto sibling=siblings.find(inode);
                Check(sibling!=siblings.end()&&!sibling->second.mapRecord,"Unexpected APFS sibling map");
                const auto expected=sibling->second.MapRecord(inode);
                Check(Matches(view.key,expected.key)&&Matches(view.value,expected.value),"Invalid APFS sibling map");
                sibling->second.mapRecord=i;
            }else if(type==8){
                const auto stream=attributeStreams.find(inode);
                Check(stream!=attributeStreams.end(),"APFS extent has no attribute stream");
                const auto size=stream->second.blocks;
                for(uint64_t logical=0;logical<size;++streamRecords){
                    const auto index=i+streamRecords;
                    Check(index<records.size(),"Missing APFS attribute extent");
                    const auto extent=at(index);
                    Check(extent.value.size==24,"Invalid APFS attribute extent size");
                    const auto length=Le64(extent.value.data);
                    const FileRun run{stream->second.inode,Le64(extent.value.data+8),length/Block,inode};
                    Check(run.block&&run.count&&!(length%Block)&&run.count<=size-logical&&
                          Range(run.block,run.count,blockCount),"Invalid APFS attribute extent");
                    const auto encoded=FileExtent(run,logical);
                    Check(Matches(extent.key,encoded.key)&&Matches(extent.value,encoded.value),
                          "Invalid APFS attribute extent order");
                    fileRuns.push_back(run);
                    logical+=run.count;
                }
                --streamRecords;
                attributeStreams.erase(stream);
            }else{
                Check(type==3&&view.key.size==8&&view.value.size>=92&&Le64(view.value.data+8)==inode,
                      "Invalid APFS inode");
                const auto parentId=Le64(view.value.data);
                const bool reservedInode=inode==2||inode==3;
                const auto link=links.find(inode);
                Check(reservedInode?parentId==1:link!=links.end()&&link->second.parent==parentId,
                      "Invalid APFS inode parent");
                const auto kind=reservedInode?CatalogKind::Directory:link->second.kind;
                if(!reservedInode){
                    link->second.inodeRecord=i;
                    link->second.finder=readFinder(view.value.data,view.value.size);
                }
                bool hasSecurity=false,hasTarget=false;
                if(!reservedInode){
                    std::string previousAttribute;
                    for(;i+1+streamRecords<records.size();++streamRecords){
                        const auto attr=at(i+1+streamRecords);
                        if(attr.key.size<8||Le64(attr.key.data)!=((uint64_t(4)<<60)|inode))break;
                        Check(attr.key.size>=12&&attr.key.size<=138&&attr.key.data[attr.key.size-1]==0&&
                              Le16(attr.key.data+8)==attr.key.size-10&&attr.value.size>=4&&
                              Le16(attr.value.data+2)==attr.value.size-4&&
                              attr.value.size-4<EmbeddedAttributeLimit,"Invalid APFS embedded attribute");
                        const std::string name(reinterpret_cast<const char*>(attr.key.data+10),attr.key.size-11);
                        Check(name>previousAttribute&&name.find('\0')==name.npos&&(OrdinaryAttribute(name)||name==ResourceAttribute||name==CompressionAttribute||name==SymlinkAttribute),
                              "Invalid APFS attribute name or order");
                        hasSecurity|=name=="com.apple.system.Security";
                        if(name==ResourceAttribute){
                            Check(kind==CatalogKind::RegularFile&&attr.value.size>4,"Invalid APFS resource fork");
                            link->second.resourceRecord=i+1+streamRecords;
                        }
                        const auto storage=Le16(attr.value.data);
                        if(name==CompressionAttribute){
                            Check(kind==CatalogKind::RegularFile&&storage==2&&attr.value.size>=20&&
                                  Le32(attr.value.data+4)==0x636d7066,"Invalid APFS compression header");
                            link->second.compressionRecord=i+1+streamRecords;
                        }
                        if(name==SymlinkAttribute){
                            Check(kind==CatalogKind::SymbolicLink&&storage==6&&attr.value.size>=6&&
                                  attr.value.data[attr.value.size-1]==0&&
                                  !memchr(attr.value.data+4,0,attr.value.size-5),"Invalid APFS symbolic link target");
                            hasTarget=true;
                        }else if(storage==1){
                            Check(attr.value.size==52,"Invalid APFS attribute stream descriptor");
                            const auto id=Le64(attr.value.data+4),size=Le64(attr.value.data+12);
                            const auto blocks=DivideUp(size,Block);
                            Check(id>inode&&id<nextId&&size&&blocks<=blockCount&&
                                  Le64(attr.value.data+20)==blocks*Block&&
                                  !Le64(attr.value.data+28)&&!Le64(attr.value.data+36)&&!Le64(attr.value.data+44)&&
                                  attributeStreams.emplace(id,AttributeStream{inode,blocks}).second,
                                  "Invalid APFS attribute stream");
                        }else Check(storage==2,"Invalid APFS embedded attribute flags");
                        previousAttribute=name;
                    }
                    link->second.attributes=streamRecords;
                    for(;i+1+streamRecords<records.size();++streamRecords){
                        const auto item=at(i+1+streamRecords);
                        if(item.key.size<8||Le64(item.key.data)!=((uint64_t(5)<<60)|inode))break;
                        Check(item.key.size==16,"Invalid APFS sibling link key");
                        const auto id=Le64(item.key.data+8);
                        const auto sibling=siblings.find(id);
                        Check(sibling!=siblings.end()&&!sibling->second.linkRecord,"Unexpected APFS sibling link");
                        const auto expected=sibling->second.LinkRecord(id);
                        Check(Matches(item.key,expected.key)&&Matches(item.value,expected.value),"Invalid APFS sibling link");
                        sibling->second.linkRecord=i+1+streamRecords;
                    }
                }
                Check(bool(Le64(view.value.data+48)&HasSecurityAttribute)==hasSecurity,
                      "APFS security attribute and inode flag disagree");
                Check(hasTarget==(kind==CatalogKind::SymbolicLink),"APFS symbolic link target is missing");
                const auto mode=Le16(view.value.data+80)&0170000;
                if(kind==CatalogKind::Directory){
                    Check(mode==0040000&&directories.emplace(inode,Directory{i}).second,"Invalid APFS directory inode");
                }else{
                    const auto& primaryName=link->second.name;
                    const auto value=view.value.data;
                    const auto& finder=link->second.finder;
                    const size_t stream=104+(finder?4:0)+size_t(DivideUp(primaryName.size()+1,8)*8);
                    const size_t streamEnd=stream+40+(finder?finder->size():0);
                    const auto size=view.value.size==streamEnd?Le64(value+stream):0;
                    Check(DivideUp(size,Block)<=blockCount,"Unsupported APFS file size in writable catalog");
                    const bool symlink=kind==CatalogKind::SymbolicLink;
                    Check(!symlink||(!size&&!link->second.resourceRecord&&!link->second.compressionRecord),
                          "APFS symbolic link cannot have file streams");
                    const auto group=siblingGroups.find(inode);
                    const auto count=group==siblingGroups.end()?1:group->second.size();
                    Check(count<=INT32_MAX,"Too many APFS hard links");
                    auto expected=WriterDisk::Inode(primaryName,inode,0,parentId,symlink?0120777:0100644,
                                                    uint32_t(count),size,finder?&*finder:nullptr);
                    SetResourceFlag(expected.value,link->second.resourceRecord.has_value());
                    if(hasSecurity)Set(expected.value,48,Le64(expected.value.data()+48)|HasSecurityAttribute,8);
                    Set(expected.value,68,Le32(value+68)&UserImmutable,4);
                    const auto compression=link->second.compressionRecord;
                    if(compression){
                        Check(!size,"Compressed APFS files cannot have a data stream");
                        SetCompressionFlags(expected.value,Le64(records[*compression].value.data()+12),true);
                    }
                    Check(mode==(symlink?0120000:0100000)&&view.value.size==expected.value.size()&&
                          !memcmp(value+48,expected.value.data()+48,view.value.size-48),
                          "APFS writes require native regular-file data streams");
                    if(size){
                        Check(i+2+streamRecords<records.size(),"Missing APFS data-stream records");
                        const auto reference=at(i+1+streamRecords);
                        const auto streamRef=StreamReference(inode);
                        Check(Matches(reference.key,streamRef.key)&&Matches(reference.value,streamRef.value),
                              "Invalid APFS data-stream record");
                        ++streamRecords;
                        const auto blocks=DivideUp(size,Block);
                        for(uint64_t logical=0;logical<blocks;){
                            const auto index=i+1+streamRecords;
                            Check(index<records.size(),"Missing APFS file extent");
                            const auto extent=at(index);
                            Check(extent.value.size==24,"Invalid APFS file extent size");
                            const auto length=Le64(extent.value.data);
                            const FileRun run{inode,Le64(extent.value.data+8),length/Block};
                            Check(run.block&&run.count&&!(length%Block)&&run.count<=blocks-logical&&
                                  Range(run.block,run.count,blockCount),"Invalid length or address in APFS file extent");
                            const auto encoded=FileExtent(run,logical);
                            Check(Matches(extent.key,encoded.key)&&Matches(extent.value,encoded.value),
                                  "Invalid or noncontiguous logical APFS file extent");
                            logical+=run.count;
                            fileRuns.push_back(run);
                            ++streamRecords;
                        }
                    }
                    link->second.dataSize=size;
                    link->second.streamRecords=streamRecords-link->second.attributes;
                    if(symlink)++symlinks;else ++files;
                }
            }
            // Catalog keys sort by inode ID, record type, then directory name hash.
            const Key order{inode,type,hash,std::move(storedName)};
            Check(order>previous,"Invalid or unsupported APFS directory key order");
            // A regular file cannot be a destination parent, so its stream
            // records always sort on the same side of the insertion point.
            if(addedKey&&!renamingThisEntry&&replacedRecord!=i&&order<*addedKey)insertAt+=1+streamRecords;
            previous=order;
        }
        i+=streamRecords;
    }
    Check(attributeStreams.empty()&&directories.count(2)&&directories.count(3)&&directories.size()==directoryCount+2&&
          files==fileCount&&symlinks==symlinkCount&&links.size()==directoryCount+fileCount+symlinkCount,"APFS catalog records and volume counts disagree");
    for(const auto& entry:directories){
        const auto& folder=entry.second;
        Check(Le32(records[folder.record].value.data()+56)==folder.children,"Invalid APFS directory child count");
    }
    for(const auto& [id,sibling]:siblings)
        Check(sibling.linkRecord&&sibling.mapRecord,"Missing APFS sibling link or map");
    const auto target=directories.find(parent);
    Check(parent!=3&&target!=directories.end(),"The APFS destination folder does not exist");
    auto& parentInode=records[target->second.record].value;
    Check(!(Le32(parentInode.data()+68)&(UserImmutable|SystemImmutable)),
          "Unlock the APFS destination folder before making changes");
    if(!replace&&!metadata&&!attributesEdit&&!resourceEdit&&!linkEdit){
        Set(parentInode,24,time,8);
        Set(parentInode,32,time,8);
    }
    if(hardLink){
        const auto found=links.find(entryId);
        Check(found!=links.end()&&found->second.kind==CatalogKind::RegularFile,
              "APFS hard-link source must be a regular file");
        const auto& link=found->second;
        auto& inode=records[link.inodeRecord].value;
        Check(!Le32(inode.data()+68),"Unlock the APFS file before creating a hard link");
        const auto count=Le32(inode.data()+56);
        Check(count<INT32_MAX,"Too many APFS hard links");
        Set(inode,56,count+1,4);
        Set(inode,32,time,8);
        if(!hardLink->replaced)Set(parentInode,56,target->second.children+1,4);
        auto afterLinks=link.inodeRecord+1+link.attributes+(count==1?0:count);
        std::vector<OwnedRecord> newLinks,newMaps;
        const auto append=[&](uint64_t id,uint64_t folder,const std::string& filename){
            const CatalogSibling sibling{entryId,folder,filename};
            newLinks.push_back(sibling.LinkRecord(id));
            newMaps.push_back(sibling.MapRecord(id));
        };
        if(count==1){
            SetSibling(records[link.entryRecord].value,nextId);
            append(nextId,link.parent,link.name);
        }
        const auto aliasId=nextId+1;
        SetSibling(added.value,aliasId);
        append(aliasId,parent,name);
        if(hardLink->replaced){
            const auto victim=links.find(hardLink->replaced);
            Check(replacedRecord&&victim!=links.end()&&victim->second.entryRecord==*replacedRecord&&
                  victim->second.kind==CatalogKind::RegularFile&&victim->first!=entryId&&
                  !siblingGroups.count(victim->first),"APFS can only replace an unlinked regular file with a hard link");
            const auto& old=victim->second;
            Check(!Le32(records[old.inodeRecord].value.data()+68),"Unlock the APFS destination before replacing it");
            const auto first=old.inodeRecord+1;
            for(size_t index=first;index<first+old.attributes;++index){
                const auto& value=records[index].value;
                if(Le16(value.data())==1)retiredStreams.insert(Le64(value.data()+4));
            }
            const auto length=1+old.attributes+old.streamRecords;
            const auto oldAfterLinks=afterLinks;
            if(*replacedRecord<oldAfterLinks)--afterLinks;
            if(old.inodeRecord<oldAfterLinks)afterLinks-=length;
            if(old.inodeRecord<*replacedRecord){
                records.erase(records.begin()+*replacedRecord);
                records.erase(records.begin()+old.inodeRecord,records.begin()+old.inodeRecord+length);
            }else{
                records.erase(records.begin()+old.inodeRecord,records.begin()+old.inodeRecord+length);
                records.erase(records.begin()+*replacedRecord);
            }
            // The directory record was already excluded from the insertion position.
            if(victim->first<parent)insertAt-=length;
            removedKind=CatalogKind::RegularFile;
        }
        records.insert(records.begin()+afterLinks,std::make_move_iterator(newLinks.begin()),
                       std::make_move_iterator(newLinks.end()));
        if(insertAt>=afterLinks)insertAt+=newLinks.size();
        records.insert(records.begin()+insertAt,std::move(added));
        records.insert(records.end(),std::make_move_iterator(newMaps.begin()),
                       std::make_move_iterator(newMaps.end()));
        return records;
    }
    if(create){
        const bool isDirectory=create->kind==CatalogKind::Directory;
        const bool symlink=create->kind==CatalogKind::SymbolicLink;
        Set(parentInode,56,target->second.children+1,4);
        records.insert(records.begin()+insertAt,std::move(added));
        records.push_back(WriterDisk::Inode(name,entryId,time,parent,isDirectory?0040755:symlink?0120777:0100644,isDirectory?0:1,dataSize,create->finder));
        if(create->locked)Set(records.back().value,68,UserImmutable,4);
        if(create->attributes)SetSecurityFlag(records.back().value,*create->attributes);
        copyDates(records.back().value);
        auto attributes=EncodeAttributes(entryId,create->attributes,resourceWrite,streams);
        if(resourceWrite)SetResourceFlag(records.back().value,resourceWrite->size!=0);
        if(create->compression)WriteCompressionAttribute(attributes,records.back().value,entryId,dataSize,create->compression);
        if(symlink){
            Check(create->linkTarget!=nullptr,"Missing APFS symbolic link target");
            attributes.push_back(LinkTargetRecord(entryId,*create->linkTarget));
        }
        SortAttributes(attributes);
        records.insert(records.end(),std::make_move_iterator(attributes.begin()),std::make_move_iterator(attributes.end()));
        streamAt=records.size();
    }else{
        const auto found=links.find(entryId);
        Check(found!=links.end(),"The APFS entry no longer exists");
        const auto& link=found->second;
        const auto group=siblingGroups.find(entryId);
        const bool shared=group!=siblingGroups.end();
        const bool inFolder=link.parent==parent||(shared&&
            std::any_of(group->second.begin(),group->second.end(),
                        [&](uint64_t id){return siblings.at(id).parent==parent;}));
        Check((rename||remove)?selectedRecord.has_value():inFolder,
              "The APFS entry is no longer in the selected folder");
        Check(!replace||link.kind==CatalogKind::RegularFile,"APFS data replacement requires a regular file");
        auto& inode=records[link.inodeRecord].value;
        const auto bsdFlags=Le32(inode.data()+68);
        Check(!(bsdFlags&SystemImmutable)&&(!(bsdFlags&UserImmutable)||(metadata&&metadata->finder&&!metadata->locked)),
              "Unlock the APFS entry before making changes");
        if(remove&&selectedSibling){
            const auto& deleted=siblings.at(selectedSibling);
            const auto& group=siblingGroups.at(entryId);
            const auto first=*group.begin();
            const auto survivorId=first==selectedSibling?*std::next(group.begin()):first;
            const auto& survivor=siblings.at(survivorId);
            const auto count=uint32_t(group.size()-1);
            Set(inode,56,count,4);
            Set(inode,32,time,8);
            if(first==selectedSibling){
                const auto mode=Le16(inode.data()+80);
                auto updated=WriterDisk::Inode(survivor.name,entryId,0,survivor.parent,mode,count,
                                                link.dataSize,link.finder?&*link.finder:nullptr);
                std::copy_n(inode.begin(),size_t(92),updated.value.begin());
                Set(updated.value,0,survivor.parent,8);
                inode=std::move(updated.value);
            }
            Set(parentInode,56,target->second.children-1,4);
            std::vector<size_t> discard{*selectedRecord,*deleted.linkRecord,*deleted.mapRecord};
            if(count==1){
                records[survivor.directoryRecord].value.resize(18);
                discard.push_back(*survivor.linkRecord);
                discard.push_back(*survivor.mapRecord);
            }
            std::sort(discard.rbegin(),discard.rend());
            for(const auto index:discard)records.erase(records.begin()+index);
            return records;
        }
        if(rename&&selectedSibling){
            auto& sibling=siblings.at(selectedSibling);
            sibling.name=name;
            records[*sibling.linkRecord]=sibling.LinkRecord(selectedSibling);
            if(*selectedRecord!=link.entryRecord){
                Set(inode,32,time,8);
                added.value=std::move(records[*selectedRecord].value);
                records.erase(records.begin()+*selectedRecord);
                records.insert(records.begin()+insertAt,std::move(added));
                return records;
            }
        }
        if(metadata&&!metadata->finder){
            Check(link.kind==CatalogKind::Directory,"APFS folder timestamps require a directory");
            editDates(inode);
            Set(inode,32,time,8);
            return records;
        }

        const auto afterInode=link.inodeRecord+1;
        const auto retireAttribute=[&](size_t index){
            const auto& value=records[index].value;
            if(Le16(value.data())==1)retiredStreams.insert(Le64(value.data()+4));
        };
        const auto rewriteAttributes=[&](const HfsPlus::Attributes* ordinary,const ResourceWrite* resource,size_t fileRecords){
            auto attributes=EncodeAttributes(entryId,ordinary,resource,streams);
            for(size_t index=afterInode;index<afterInode+link.attributes;++index){
                if(link.compressionRecord==index){
                    if(!replace)attributes.push_back(std::move(records[index]));
                    continue;
                }
                const auto& oldAttribute=records[index];
                if(Le16(oldAttribute.value.data())==6){
                    if(!linkEdit)attributes.push_back(std::move(records[index]));
                    continue;
                }
                const bool isResource=link.resourceRecord==index;
                if(isResource?!resource:!ordinary)attributes.push_back(std::move(records[index]));
                else retireAttribute(index);
            }
            if(linkEdit)attributes.push_back(LinkTargetRecord(entryId,linkEdit->target));
            if(ordinary)SetSecurityFlag(inode,*ordinary);
            if(resource)SetResourceFlag(inode,resource->size!=0);
            if(replace)WriteCompressionAttribute(attributes,inode,entryId,dataSize,replace->compression);
            SortAttributes(attributes);
            // Replacing the data stream also removes the records following the
            // attributes. Keep sibling links ahead of the new stream records.
            if(replace&&shared){
                for(const auto id:group->second)
                    attributes.push_back(std::move(records[*siblings.at(id).linkRecord]));
            }
            records.erase(records.begin()+afterInode,records.begin()+afterInode+link.attributes+fileRecords);
            records.insert(records.begin()+afterInode,std::make_move_iterator(attributes.begin()),
                           std::make_move_iterator(attributes.end()));
            return afterInode+attributes.size();
        };
        if(linkEdit){
            Check(link.kind==CatalogKind::SymbolicLink,"Can only replace an APFS symbolic link");
            auto updated=WriterDisk::Inode(link.name,entryId,0,parent,0120777,1,0,&linkEdit->finder);
            const auto flags=(Le64(inode.data()+48)&~WriterDisk::HasFinderInfo)|
                             (Le64(updated.value.data()+48)&WriterDisk::HasFinderInfo);
            std::copy_n(inode.begin(),92,updated.value.begin());
            Set(updated.value,48,flags,8);
            Set(updated.value,68,linkEdit->locked?bsdFlags|UserImmutable:bsdFlags,4);
            Set(updated.value,24,time,8);
            Set(updated.value,32,time,8);
            copyDates(updated.value);
            inode=std::move(updated.value);
            rewriteAttributes(&linkEdit->attributes,nullptr,0);
            return records;
        }
        if(attributesEdit){
            Set(inode,32,time,8);
            rewriteAttributes(&attributesEdit->attributes,nullptr,0);
            return records;
        }
        if(resourceEdit){
            Check(link.kind==CatalogKind::RegularFile,"Select a regular file for APFS resource import");
            Set(inode,24,CatalogTimestamp(resourceEdit->modified),8);
            Set(inode,32,time,8);
            rewriteAttributes(nullptr,resourceWrite,0);
            return records;
        }
        const auto mode=Le16(inode.data()+80);
        const auto count=Le32(inode.data()+56);
        const auto existingFinder=link.finder?&*link.finder:nullptr;
        const auto old=WriterDisk::Inode(link.name,entryId,0,link.parent,mode,count,link.dataSize,existingFinder);
        constexpr size_t inodeHeader=92;
        Check(inode.size()==old.value.size()&&
              !memcmp(inode.data()+inodeHeader,old.value.data()+inodeHeader,inode.size()-inodeHeader),
              "APFS writes require native inode name fields");
        if(remove){
            Check(link.kind!=CatalogKind::Directory||directories.at(entryId).children==0,
                  "The APFS folder is not empty");
            for(size_t index=afterInode;index<afterInode+link.attributes;++index)retireAttribute(index);
            removedKind=link.kind;
            Set(parentInode,56,target->second.children-1,4);
            const auto first=records.begin()+link.inodeRecord;
            const auto end=first+1+link.attributes+link.streamRecords;
            if(link.entryRecord>link.inodeRecord){
                records.erase(records.begin()+link.entryRecord);
                records.erase(records.begin()+link.inodeRecord,
                              records.begin()+link.inodeRecord+1+link.attributes+link.streamRecords);
            }else{
                records.erase(first,end);
                records.erase(records.begin()+link.entryRecord);
            }
            return records;
        }
        const auto updatedFinder=metadata?metadata->finder:replace&&replace->finder?replace->finder:existingFinder;
        const auto& updatedName=(replace||metadata)?link.name:name;
        auto updated=WriterDisk::Inode(updatedName,entryId,0,link.parent,mode,count,
                                       replace?dataSize:link.dataSize,updatedFinder);
        const auto newFinderFlag=Le64(updated.value.data()+48)&WriterDisk::HasFinderInfo;
        std::copy_n(inode.begin(),inodeHeader,updated.value.begin());
        Set(updated.value,48,(Le64(inode.data()+48)&~WriterDisk::HasFinderInfo)|newFinderFlag,8);
        Set(updated.value,32,time,8);
        if(replace){
            if(replace->locked)Set(updated.value,68,bsdFlags|UserImmutable,4);
            Set(updated.value,24,time,8);
            copyDates(updated.value);
        }
        inode=std::move(updated.value);
        if(metadata){
            Set(inode,68,(bsdFlags&~UserImmutable)|(metadata->locked?UserImmutable:0),4);
            editDates(inode);
            return records;
        }
        if(replace){
            streamAt=rewriteAttributes(replace->attributes,resourceWrite,link.streamRecords);
            return records;
        }

        added.value=std::move(records[link.entryRecord].value);
        records.erase(records.begin()+link.entryRecord);
        records.insert(records.begin()+insertAt,std::move(added));
    }
    return records;
}

using CatalogLeaves=std::vector<std::vector<OwnedRecord>>;
CatalogLeaves PartitionCatalog(std::vector<OwnedRecord> records)
{
    size_t totalBytes=0;
    for(const auto& record:records)totalBytes+=record.key.size()+record.value.size();
    CatalogLeaves leaves;
    if(WriterDisk::FitsCatalogLeaf(records.size(),totalBytes)){
        leaves.push_back(std::move(records));
        return leaves;
    }
    leaves.emplace_back();
    size_t bytes=0;
    for(auto& record:records){
        const auto size=record.key.size()+record.value.size();
        if(!WriterDisk::FitsCatalogLeaf(leaves.back().size()+1,bytes+size)){
            leaves.emplace_back();
            bytes=0;
        }
        Check(WriterDisk::FitsCatalogLeaf(1,size),"APFS catalog record exceeds a leaf");
        bytes+=size;
        leaves.back().push_back(std::move(record));
    }
    Check(leaves.size()>1,"APFS catalog root has insufficient space");
    return leaves;
}

OwnedRecord CatalogSeparator(const std::vector<OwnedRecord>& child,uint64_t oid)
{
    OwnedRecord record{child.front().key,Bytes(8)};
    Set(record.value,0,oid,8);
    return record;
}

CatalogLeaves CatalogIndex(const CatalogLeaves& leaves,uint64_t firstOid)
{
    std::vector<OwnedRecord> separators;
    for(size_t i=0;i<leaves.size();++i)separators.push_back(CatalogSeparator(leaves[i],firstOid+i));
    return PartitionCatalog(std::move(separators));
}

std::vector<Mapping> StageCatalog(Writes& writes,const CatalogLeaves& leaves,const std::vector<uint64_t>& blocks,
                                  size_t firstChild,uint64_t rootOid,uint64_t firstOid,uint64_t tx)
{
    std::vector<Mapping> mappings{{rootOid,blocks[0]}};
    if(leaves.size()==1){
        StageObject(writes,blocks[0],EncodeLeaf(leaves.front(),0x40),rootOid,tx,2,14);
        return mappings;
    }
    auto index=CatalogIndex(leaves,firstOid);
    size_t keys=0,longestKey=0,longestValue=8;
    for(size_t i=0;i<leaves.size();++i){
        const auto& leaf=leaves[i];
        const auto oid=firstOid+i,block=blocks[firstChild+i];
        keys+=leaf.size();
        for(const auto& record:leaf){
            longestKey=(std::max)(longestKey,record.key.size());
            longestValue=(std::max)(longestValue,record.value.size());
        }
        StageObject(writes,block,WriterDisk::EncodeNode(leaf,0x40,false,0),oid,tx,3,14);
        mappings.push_back({oid,block});
    }
    std::vector<OwnedRecord> rootIndex;
    if(index.size()==1)rootIndex=std::move(index.front());
    else for(size_t i=0;i<index.size();++i){
        const auto offset=leaves.size()+i;
        const auto oid=firstOid+offset,block=blocks[firstChild+offset];
        rootIndex.push_back(CatalogSeparator(index[i],oid));
        StageObject(writes,block,WriterDisk::EncodeNode(index[i],0x40,false,1),oid,tx,3,14);
        mappings.push_back({oid,block});
    }
    auto root=WriterDisk::EncodeNode(rootIndex,0x40,true,index.size()==1?1:2);
    constexpr auto footer=Block-WriterDisk::TreeFooter;
    Set(root,footer+16,longestKey,4);
    Set(root,footer+20,longestValue,4);
    Set(root,footer+24,keys,8);
    Set(root,footer+32,mappings.size(),8);
    StageObject(writes,blocks[0],std::move(root),rootOid,tx,2,14);
    return mappings;
}

void StageExtentTree(Writes& writes,const CatalogLeaves& leaves,const std::vector<uint64_t>& allocation,
                     size_t firstLeaf,uint64_t transaction)
{
    constexpr uint32_t flags=0x50;
    const auto rootBlock=allocation[6];
    if(leaves.size()==1){
        StageObject(writes,rootBlock,EncodeLeaf(leaves.front(),flags),rootBlock,transaction,Physical|2,15);
        return;
    }
    std::vector<OwnedRecord> separators;
    size_t records=0;
    for(size_t index=0;index<leaves.size();++index){
        const auto block=allocation[firstLeaf+index];
        const auto& leaf=leaves[index];
        OwnedRecord separator{leaf.front().key,Bytes(8)};
        Set(separator.value,0,block,8);
        separators.push_back(std::move(separator));
        records+=leaf.size();
        StageObject(writes,block,WriterDisk::EncodeNode(leaf,flags,false,0),block,transaction,Physical|3,15);
    }
    auto root=WriterDisk::EncodeNode(separators,flags,true,1);
    constexpr auto footer=Block-WriterDisk::TreeFooter;
    Set(root,footer+20,20,4);
    Set(root,footer+24,records,8);
    Set(root,footer+32,leaves.size()+1,8);
    StageObject(writes,rootBlock,std::move(root),rootBlock,transaction,Physical|2,15);
}

struct CatalogVolume {
    WritableMap containerMap;
    uint64_t address;
    Bytes volume;
};

CatalogVolume ReadCatalogVolume(Source& source,const Container& container)
{
    Check(!container.offset&&container.checkpointBlock&&container.blockSize==Block&&
          !container.keybagBlock&&!container.keybagBlocks&&container.volumes.size()==1&&
          source.Size()==container.blockCount*Block,"APFS writes require a raw single-volume image");
    const auto oid=container.volumes.front();
    auto map=ReadWritableMap(source,container,container.objectMap);
    Check(map.mappings.size()==1&&map.mappings.front().oid==oid,
          "APFS writes require one container volume mapping");
    const auto address=map.mappings.front().block;
    auto volume=ReadObject(source,container,address,oid,container.transaction,13);
    return {std::move(map),address,std::move(volume)};
}

void RenameExternalInode(Bytes& inode,const std::string& name,const std::string& replacement)
{
    Check(inode.size()>=96,"Missing APFS inode name fields");
    const auto descriptors=Le16(inode.data()+92);
    size_t offset=96+size_t(descriptors)*4;
    Check(offset<=inode.size(),"Truncated APFS inode field table");
    bool found=false;
    for(size_t i=0;i<descriptors;++i){
        const auto field=96+i*4;
        const auto length=Le16(inode.data()+field+2);
        const auto padded=size_t(DivideUp(length,8)*8);
        Check(Range(offset,padded,inode.size()),"Truncated APFS inode field");
        if(inode[field]==4){
            Check(!found&&length==name.size()+1&&!memcmp(inode.data()+offset,name.c_str(),length),
                  "APFS inode name differs from its directory entry");
            std::copy(replacement.begin(),replacement.end(),inode.begin()+offset);
            found=true;
        }
        offset+=padded;
    }
    Check(found&&offset==inode.size(),"Invalid APFS inode name fields");
}

void CheckNativeVolume(const Bytes& volume)
{
    const auto flags=Le64(volume.data()+56);
    Check(!memcmp(volume.data()+32,"APSB",4)&&!memcmp(volume.data()+272,"HFVExplorer",12)&&
          (flags==1||flags==8)&&Le64(volume.data()+264)==1&&Le32(volume.data()+116)==2&&
          !Le64(volume.data()+208)&&!Le64(volume.data()+216),
          "APFS catalog writes currently require an unencrypted volume formatted by HFVExplorer");
}
}

void AssignDataRuns(std::vector<DataStreamWrite>& streams,const std::vector<DataRun>& runs)
{
    size_t index=0;
    uint64_t consumed=0;
    for(auto& stream:streams){
        stream.runs.clear();
        auto remaining=DivideUp(stream.size,Block);
        while(remaining){
            Check(index<runs.size()&&consumed<runs[index].count,"APFS data allocation is incomplete");
            const auto& run=runs[index];
            const auto count=(std::min)(remaining,run.count-consumed);
            Check(consumed<=UINT64_MAX-run.block,"APFS data allocation overflows");
            stream.runs.push_back({run.block+consumed,count});
            remaining-=count;
            consumed+=count;
            if(consumed==run.count){
                ++index;
                consumed=0;
            }
        }
    }
    Check(index==runs.size(),"APFS data allocation has unused blocks");
}

void ValidateWritableAttributes(const HfsPlus::Attributes& attributes)
{
    for(const auto& [wide,value]:attributes){
        const auto name=EncodeAttributeName(wide);
        Check(OrdinaryAttribute(name),"Special APFS attributes require dedicated write support");
        if(wide==SecurityAttribute)HfsPlus::ValidateAttributes({{wide,value}});
    }
}

uint64_t CatalogTimestamp(uint64_t secondsSince1904)
{
    constexpr int64_t UnixEpoch=2082844800,Second=1000000000;
    Check(secondsSince1904<=UnixEpoch+INT64_MAX/Second,"The timestamp is outside the APFS writable range");
    return uint64_t((int64_t(secondsSince1904)-UnixEpoch)*Second);
}

CatalogLayoutCounts InspectCatalogLayout(Source& source,const Container& container)
{
    const auto layout=ReadCatalogVolume(source,container);
    const auto& volume=layout.volume;
    const auto volumeMap=ReadWritableMap(source,container,Le64(volume.data()+128));
    const auto catalog=ReadWritableCatalog(source,container,volumeMap,Le64(volume.data()+136));
    const auto extents=ReadExtentTree(source,container,Le64(volume.data()+144));
    return {catalog.records.size(),extents.records.size()};
}

uint64_t VerifyAllocationBitmaps(Source& source,const Container& container,const Bytes& space)
{
    return ReadAllocationBitmaps(source,container,space).available*container.blockSize;
}

uint64_t VerifyFreeQueueAllocation(Source& source,const Container& container,uint64_t transaction,
                                   Bytes& superblock,const CheckpointObjectLookup& ephemeral)
{
    const auto [containerMap,address,volume]=ReadCatalogVolume(source,container);
    const auto volumeMap=ReadWritableMap(source,container,Le64(volume.data()+128));
    const auto catalog=ReadWritableCatalog(source,container,volumeMap,Le64(volume.data()+136));
    const auto extents=ReadExtentTree(source,container,Le64(volume.data()+144));
    const auto snapshots=Le64(volume.data()+152);
    const Node snapshotTree(ReadObject(source,container,snapshots,snapshots,container.transaction,2,16,true),true,{});
    Check(!snapshotTree.Level()&&!snapshotTree.Count(),"APFS allocation inspection requires an empty snapshot tree");
    std::vector<uint64_t> live{containerMap.headerBlock,containerMap.rootBlock,address,
                               volumeMap.headerBlock,volumeMap.rootBlock};
    live.insert(live.end(),containerMap.leaves.begin(),containerMap.leaves.end());
    live.insert(live.end(),volumeMap.leaves.begin(),volumeMap.leaves.end());
    live.insert(live.end(),catalog.blocks.begin(),catalog.blocks.end());
    std::vector<FileRun> files;
    for(const auto& extent:extents.records)
        files.push_back({Le64(extent.value.data()+8),Le64(extent.key.data())&IdMask,
                         Le64(extent.value.data())&IdMask});
    auto& space=ephemeral(Le64(superblock.data()+152));
    Writes discarded;
    std::vector<DataRun> data;
    const auto allocated=Allocate(discarded,source,container,space,superblock,ephemeral,
                                  live,extents.blocks,snapshots,files,{},1,data,transaction,[]{return 1;});
    Check(allocated.size()==1&&data.size()==1&&data.front().count==1&&allocated.front()!=data.front().block,
          "APFS allocation preflight did not reserve distinct data and metadata blocks");
    return Le64(space.data()+72)*container.blockSize;
}

void ValidateWritableLayout(Source& source,const Container& container)
{
    const auto layout=ReadCatalogVolume(source,container);
    const auto& volume=layout.volume;
    CheckNativeVolume(volume);
    const auto extents=Le64(volume.data()+144),snapshots=Le64(volume.data()+152);
    ReadExtentTree(source,container,extents);
    const Node snapshotTree(ReadObject(source,container,snapshots,snapshots,container.transaction,2,16,true),true,{});
    Check(!snapshotTree.Level()&&!snapshotTree.Count(),"Unsupported APFS extent or snapshot tree");
    const auto volumeMap=ReadWritableMap(source,container,Le64(volume.data()+128));
    ReadWritableCatalog(source,container,volumeMap,Le64(volume.data()+136));
}

CatalogWritePlan PrepareCatalogMutation(
    Source& source,const Container& current,uint64_t transaction,Bytes& superblock,
    const CheckpointObjectLookup& ephemeral,const CatalogMutation& request)
{
    Check(current.blockSize==Block,"APFS catalog writes currently require 4096-byte blocks");
    const auto* create=std::get_if<CatalogCreate>(&request);
    const auto* rename=std::get_if<CatalogRename>(&request);
    const auto* hardLink=std::get_if<CatalogHardLink>(&request);
    const auto* properties=std::get_if<CatalogFileProperties>(&request);
    const auto name=create?EncodeCatalogName(create->name):rename?EncodeCatalogName(rename->name):
                    hardLink?EncodeCatalogName(hardLink->name):properties?EncodeCatalogName(properties->name):std::string{};
    const auto* replace=std::get_if<CatalogReplaceData>(&request);
    const auto* linkReplacement=std::get_if<CatalogReplaceSymlink>(&request);
    const auto dates=replace?replace->dates:create?create->dates:linkReplacement?linkReplacement->dates:std::optional<FileDates>{};
    CatalogWritePlan plan;
    auto* input=replace?&replace->data:create?create->data:nullptr;
    const auto dataSize=input?input->Size():0;
    if(dataSize)plan.streams.push_back({*input,dataSize});
    const auto fileBlocks=DivideUp(dataSize,Block);
    const auto* resourceEdit=std::get_if<CatalogResourceFork>(&request);
    auto* resourceInput=resourceEdit?&resourceEdit->resource:replace?replace->resource:create?create->resource:nullptr;
    std::optional<ResourceWrite> resource;
    if(resourceInput)resource=ResourceWrite{resourceInput,resourceInput->Size(),0};
    const bool compressionResource=(replace?replace->compression:create?create->compression:nullptr)&&resource&&resource->size;
    const auto resourceBlocks=resource&&(resource->size>=EmbeddedAttributeLimit||compressionResource)?
                              DivideUp(resource->size,Block):0;
    Check(fileBlocks<=current.blockCount&&resourceBlocks<=current.blockCount-fileBlocks,
          "APFS forks exceed the container size");
    auto dataBlocks=fileBlocks+resourceBlocks;
    if(resourceBlocks)plan.streams.push_back({*resourceInput,resource->size});
    auto [containerMap,volumeAddress,volume]=ReadCatalogVolume(source,current);
    const auto oid=current.volumes.front();
    const auto flags=Le64(volume.data()+56);
    const auto fileCount=Le64(volume.data()+184);
    const auto directoryCount=Le64(volume.data()+192);
    const auto symlinkCount=Le64(volume.data()+200);
    const auto catalogOid=Le64(volume.data()+136);
    const auto extents=Le64(volume.data()+144),snapshots=Le64(volume.data()+152);
    if(properties){
        Check(!memcmp(volume.data()+32,"APSB",4)&&Le64(volume.data()+40)==2&&
              !Le64(volume.data()+48)&&flags==1&&Le64(volume.data()+264)==1&&
              Le32(volume.data()+116)==2&&!Le64(volume.data()+208)&&!Le64(volume.data()+216),
              "Unsupported APFS volume features for a Properties edit");
    }else CheckNativeVolume(volume);
    const auto extentTree=ReadExtentTree(source,current,extents);
    const Node snapshotTree(ReadObject(source,current,snapshots,snapshots,current.transaction,2,16,true),true,{});
    Check(!snapshotTree.Level()&&!snapshotTree.Count(),"APFS catalog writes require an empty snapshot tree");
    const auto volumeMap=ReadWritableMap(source,current,Le64(volume.data()+128));
    auto existing=ReadWritableCatalog(source,current,volumeMap,catalogOid);
    const auto nextId=Le64(volume.data()+176);
    Check(nextId>=16&&nextId<IdMask,"Invalid APFS next inode ID");
    auto nextAvailable=nextId+bool(create)+bool(resourceBlocks)+(hardLink?2:0);
    Check(nextAvailable<IdMask,"APFS file-system object IDs are exhausted");
    if(resourceBlocks){
        resource->id=nextId+bool(create);
        plan.streams.back().id=resource->id;
    }
    const auto* attributeEdit=std::get_if<CatalogAttributes>(&request);
    const auto* attributes=attributeEdit?&attributeEdit->attributes:replace?replace->attributes:
                           create?create->attributes:linkReplacement?&linkReplacement->attributes:nullptr;
    AttributeStreams attributeIds;
    if(attributes){
        ValidateWritableAttributes(*attributes);
        for(const auto& [name,value]:*attributes){
            if(value.size()<EmbeddedAttributeLimit)continue;
            const auto blocks=DivideUp(value.size(),Block);
            Check(blocks<=current.blockCount-dataBlocks,"APFS streams exceed the container size");
            Check(nextAvailable<IdMask-1,"APFS file-system object IDs are exhausted");
            attributeIds.emplace(name,nextAvailable);
            auto input=std::make_unique<AttributeInput>(value);
            plan.streams.push_back({*input,value.size(),{},nextAvailable++});
            plan.attributeInputs.push_back(std::move(input));
            dataBlocks+=blocks;
        }
    }
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    const auto time=(((uint64_t(fileTime.dwHighDateTime)<<32)|fileTime.dwLowDateTime)-116444736000000000ULL)*100;
    std::vector<FileRun> files;
    std::optional<CatalogKind> removedKind;
    size_t streamAt=0;
    const auto entryId=std::visit([nextId](const auto& edit){
        if constexpr(std::is_same_v<std::decay_t<decltype(edit)>,CatalogCreate>)return nextId;
        else if constexpr(std::is_same_v<std::decay_t<decltype(edit)>,CatalogFileProperties>)return uint64_t(0);
        else return edit.inode;
    },request);
    std::set<uint64_t> retiredStreams;
    std::vector<OwnedRecord> catalog;
    if(properties){
        catalog=std::move(existing.records);
        uint64_t file=0;
        std::optional<size_t> directoryRecord;
        const auto parentKey=(uint64_t(9)<<60)|properties->parent;
        for(size_t i=0;i<catalog.size();++i){
            const auto& record=catalog[i];
            if(record.key.size()!=13+name.size()||
               Le64(record.key.data())!=parentKey||
               memcmp(record.key.data()+12,name.c_str(),name.size()+1))continue;
            Check(!file&&record.value.size()==18&&Le16(record.value.data()+16)==8,
                  "APFS Properties target must be an unlinked regular file");
            file=Le64(record.value.data());
            directoryRecord=i;
        }
        Check(file>=16&&file<nextId,"APFS Properties target was not found");
        std::string replacement;
        if(properties->newName){
            replacement=EncodeCatalogName(*properties->newName);
            Check(replacement.size()==name.size(),"This APFS rename requires the same UTF-8 filename length");
        }
        bool edited=false;
        for(auto& record:catalog){
            if(record.key.size()!=8||Le64(record.key.data())!=((uint64_t(3)<<60)|file))continue;
            auto& value=record.value;
            Check(!edited&&value.size()>=92&&Le64(value.data())==properties->parent&&
                  Le32(value.data()+56)==1&&(Le16(value.data()+80)&0170000)==0100000,
                  "APFS Properties target inode is unsupported");
            const auto flags=Le32(value.data()+68);
            const bool unlocking=properties->locked==false;
            Check(!(flags&SystemImmutable)&&(!(flags&UserImmutable)||unlocking),
                  "Unlock the APFS file before editing its properties");
            if(properties->locked)
                Set(value,68,(flags&~UserImmutable)|(*properties->locked?UserImmutable:0),4);
            if(properties->dates.created)Set(value,16,CatalogTimestamp(*properties->dates.created),8);
            if(properties->dates.modified)Set(value,24,CatalogTimestamp(*properties->dates.modified),8);
            if(properties->newName)RenameExternalInode(value,name,replacement);
            Set(value,32,time,8);
            edited=true;
        }
        Check(edited,"APFS Properties target inode was not found");
        if(properties->newName){
            auto record=std::move(catalog[*directoryRecord]);
            catalog.erase(catalog.begin()+*directoryRecord);
            const auto hash=WriterDisk::DirectoryHash(replacement,true);
            const auto comparison=NormalizedName(replacement,true);
            Set(record.key,8,hash|(replacement.size()+1),4);
            std::copy(replacement.begin(),replacement.end(),record.key.begin()+12);
            auto insert=catalog.end();
            for(auto entry=catalog.begin();entry!=catalog.end();++entry){
                Check(entry->key.size()>=8,"Truncated APFS catalog key");
                const auto raw=Le64(entry->key.data());
                if((raw&IdMask)>properties->parent||((raw&IdMask)==properties->parent&&raw>>60>9)){
                    if(insert==catalog.end())insert=entry;
                    break;
                }
                if(raw!=parentKey)continue;
                const auto& key=entry->key;
                Check(key.size()>=14&&(Le32(key.data()+8)&1023)==key.size()-12&&!key.back(),
                      "Invalid APFS directory name");
                const std::string existing(reinterpret_cast<const char*>(key.data()+12),key.size()-13);
                Check(NormalizedName(existing,true)!=comparison,"The APFS entry name already exists");
                const auto existingHash=Le32(key.data()+8)&~1023u;
                if(insert==catalog.end()&&(existingHash>hash||(existingHash==hash&&existing>replacement)))insert=entry;
            }
            catalog.insert(insert,std::move(record));
        }
        for(const auto& extent:extentTree.records)
            files.push_back({Le64(extent.value.data()+8),Le64(extent.key.data())&IdMask,
                             Le64(extent.value.data())&IdMask});
    }else{
        catalog=EditCatalog(std::move(existing.records),request,name,nextId,entryId,time,flags==1,
                            directoryCount,fileCount,symlinkCount,dataSize,current.blockCount,files,removedKind,streamAt,dates,resource?&*resource:nullptr,attributeIds,retiredStreams);
    }
    ValidateExtentReferences(extentTree,files);
    if(!retiredStreams.empty()){
        catalog.erase(std::remove_if(catalog.begin(),catalog.end(),[&](const auto& record){
            const auto key=Le64(record.key.data());
            return key>>60==8&&retiredStreams.count(key&IdMask);
        }),catalog.end());
    }
    const bool retireFile=replace||removedKind.has_value();
    const auto retiredInode=hardLink&&hardLink->replaced?hardLink->replaced:entryId;
    const auto retiredRun=[&](const FileRun& run){
        return run.stream?retiredStreams.count(run.stream)!=0:retireFile&&run.owner==retiredInode;
    };
    std::vector<DataRun> retiredData;
    uint64_t removedBlocks=0;
    for(const auto& run:files){
        if(!retiredRun(run))continue;
        retiredData.push_back({run.block,run.count});
        removedBlocks+=run.count;
    }
    const bool changeExtents=dataBlocks||removedBlocks;
    auto& space=ephemeral(Le64(superblock.data()+152));
    auto& writes=plan.metadata;
    std::vector<uint64_t> retired{containerMap.headerBlock,containerMap.rootBlock,volumeAddress,
                                  volumeMap.headerBlock,volumeMap.rootBlock};
    retired.insert(retired.end(),existing.blocks.begin(),existing.blocks.end());
    retired.insert(retired.end(),volumeMap.leaves.begin(),volumeMap.leaves.end());
    retired.insert(retired.end(),containerMap.leaves.begin(),containerMap.leaves.end());
    if(changeExtents)retired.insert(retired.end(),extentTree.blocks.begin(),extentTree.blocks.end());
    const size_t firstChild=changeExtents?7:6;
    CatalogLeaves leaves;
    size_t children=0,extentChildren=0,mapChildren=0;
    CatalogLeaves extentLeaves;
    std::vector<DataRun> allocatedData;
    const auto allocated=Allocate(writes,source,current,space,superblock,ephemeral,
        retired,extentTree.blocks,snapshots,files,std::move(retiredData),dataBlocks,allocatedData,transaction,[&]{
            AssignDataRuns(plan.streams,allocatedData);
            if(fileBlocks){
                std::vector<OwnedRecord> stream;
                stream.push_back(StreamReference(entryId));
                uint64_t logical=0;
                for(const auto& data:plan.streams.front().runs){
                    const FileRun run{entryId,data.block,data.count};
                    stream.push_back(FileExtent(run,logical));
                    logical+=data.count;
                }
                catalog.insert(catalog.begin()+streamAt,std::make_move_iterator(stream.begin()),
                               std::make_move_iterator(stream.end()));
            }
            for(const auto& stream:plan.streams){
                if(!stream.id)continue;
                uint64_t logical=0;
                for(const auto& data:stream.runs){
                    catalog.push_back(FileExtent({entryId,data.block,data.count,stream.id},logical));
                    logical+=data.count;
                }
            }
            leaves=PartitionCatalog(std::move(catalog));
            children=leaves.size()>1?leaves.size():0;
            if(children){
                const auto indexes=CatalogIndex(leaves,0).size();
                if(indexes>1)children+=indexes;
            }
            if(changeExtents){
                std::vector<FileRun> updated;
                for(const auto& run:files)if(!retiredRun(run))updated.push_back(run);
                for(const auto& stream:plan.streams)
                    for(const auto& run:stream.runs)updated.push_back({entryId,run.block,run.count,stream.id});
                std::sort(updated.begin(),updated.end(),[](const auto& a,const auto& b){return a.block<b.block;});
                std::vector<OwnedRecord> references;
                for(const auto& run:updated)references.push_back(ExtentReference(run));
                extentLeaves=PartitionCatalog(std::move(references));
                extentChildren=extentLeaves.size()>1?extentLeaves.size():0;
            }
            mapChildren=MapLeaves(children+1);
            return firstChild+children+extentChildren+mapChildren;
        });
    if(changeExtents){
        StageExtentTree(writes,extentLeaves,allocated,firstChild+children,transaction);
        Set(volume,144,allocated[6],8);
    }
    const auto firstOid=Le64(superblock.data()+88);
    if(children){
        Check(firstOid>volumeMap.mappings.back().oid&&firstOid>oid&&children<=UINT64_MAX-firstOid,
              "APFS virtual object ID space is exhausted or invalid");
        Set(superblock,88,firstOid+children,8);
    }
    const auto mappings=StageCatalog(writes,leaves,allocated,firstChild,catalogOid,firstOid,transaction);
    StageMap(writes,volumeMap,allocated[1],allocated[2],mappings,transaction,allocated,firstChild+children+extentChildren);
    const auto oldChildren=existing.blocks.size()-1+volumeMap.leaves.size()+(changeExtents?extentTree.blocks.size()-1:0);
    const auto newChildren=children+extentChildren+mapChildren;
    const auto volumeBlocks=Le64(volume.data()+88);
    const auto removedVolumeBlocks=oldChildren+removedBlocks;
    Check(volumeBlocks>=removedVolumeBlocks&&volumeBlocks<=current.blockCount&&
          dataBlocks+newChildren<=current.blockCount-(volumeBlocks-removedVolumeBlocks),
          "Invalid APFS volume allocation count");
    Set(volume,88,volumeBlocks-removedVolumeBlocks+newChildren+dataBlocks,8);
    Set(volume,128,allocated[1],8);
    Set(volume,176,nextAvailable,8);
    if(create){
        if(create->kind==CatalogKind::Directory)Set(volume,192,directoryCount+1,8);
        else if(create->kind==CatalogKind::SymbolicLink)Set(volume,200,symlinkCount+1,8);
        else Set(volume,184,fileCount+1,8);
    }else if(removedKind){
        if(*removedKind==CatalogKind::Directory)Set(volume,192,directoryCount-1,8);
        else if(*removedKind==CatalogKind::SymbolicLink)Set(volume,200,symlinkCount-1,8);
        else Set(volume,184,fileCount-1,8);
    }
    Set(volume,256,time,8);
    const uint64_t replaced=changeExtents?4:3;
    const auto newVolumeBlocks=replaced+newChildren+dataBlocks,oldVolumeBlocks=replaced+removedVolumeBlocks;
    Check(Le64(volume.data()+224)<=UINT64_MAX-newVolumeBlocks&&Le64(volume.data()+232)<=UINT64_MAX-oldVolumeBlocks,
          "APFS volume allocation history overflow");
    Set(volume,224,Le64(volume.data()+224)+newVolumeBlocks,8);
    Set(volume,232,Le64(volume.data()+232)+oldVolumeBlocks,8);
    StageObject(writes,allocated[3],std::move(volume),oid,transaction,13);
    StageMap(writes,containerMap,allocated[4],allocated[5],{{oid,allocated[3]}},transaction,allocated,0);
    Set(superblock,160,allocated[4],8);
    return plan;
}
}
