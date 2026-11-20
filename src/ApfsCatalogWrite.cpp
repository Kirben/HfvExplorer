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
struct MapLeaf {
    uint64_t headerBlock,rootBlock;
    Bytes header;
    std::vector<Mapping> mappings;
};
MapLeaf ReadWritableMap(Source& source,const Container& container,uint64_t block)
{
    auto header=ReadObject(source,container,block,block,container.transaction,11,0,true);
    const auto root=Le64(header.data()+48);
    Check(Le32(header.data()+40)==(Physical|2)&&!Le32(header.data()+36)&&!Le64(header.data()+56),
          "APFS object map contains unsupported snapshot references");
    const Node node(ReadObject(source,container,root,root,container.transaction,2,11,true),true,{});
    Check(!node.Level()&&node.Layout().keySize==16&&node.Layout().valueSize==16,
          "APFS writes require a single-leaf object map");
    MapLeaf map{block,root,std::move(header),{}};
    std::set<uint64_t> physicalBlocks{block,root};
    uint64_t previous=0;
    for(size_t i=0;i<node.Count();++i){
        const auto record=node.At(i);
        const auto oid=Le64(record.key.data),xid=Le64(record.key.data+8);
        const auto physical=Le64(record.value.data+8);
        Check(oid>previous&&xid&&xid<=container.transaction&&Le32(record.value.data)==0&&
              Le32(record.value.data+4)==Block&&physical&&physical<container.blockCount&&
              physicalBlocks.insert(physical).second,"Invalid APFS writable object mapping");
        map.mappings.push_back({oid,physical});
        previous=oid;
    }
    return map;
}

void StageMap(Writes& writes,MapLeaf map,uint64_t headerBlock,uint64_t rootBlock,
              const std::vector<Mapping>& mappings,uint64_t transaction)
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
    StageObject(writes,rootBlock,EncodeLeaf(records,0x10,16,16),rootBlock,transaction,Physical|2,11);
}

struct CatalogRecords {
    std::vector<OwnedRecord> records;
    std::vector<uint64_t> blocks;
};
CatalogRecords ReadWritableCatalog(Source& source,const Container& container,const MapLeaf& map,uint64_t oid)
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
    Check(root.Level()<=1&&!root.Layout().keySize&&!root.Layout().valueSize,
          "APFS catalog writes currently support at most two tree levels");
    catalog.blocks.push_back(rootBlock);
    if(!root.Level())append(root);
    else{
        std::set<uint64_t> children;
        for(size_t i=0;i<root.Count();++i){
            const auto record=root.At(i);
            Check(record.value.size==8,"Invalid APFS catalog index record");
            const auto child=Le64(record.value.data);
            Check(child!=oid&&children.insert(child).second,"Repeated APFS catalog leaf");
            const auto block=find(child);
            const Node leaf(ReadObject(source,container,block,child,container.transaction,3,14),false,{});
            Check(!leaf.Level()&&leaf.Count(),"Invalid APFS catalog child level or count");
            const auto first=leaf.At(0).key;
            Check(first.size==record.key.size&&!memcmp(first.data,record.key.data,first.size),
                  "APFS catalog separator does not match its first leaf key");
            append(leaf);
            catalog.blocks.push_back(block);
        }
    }
    Check(catalog.blocks.size()==map.mappings.size(),"APFS volume map has unrelated live objects");
    return catalog;
}

template<class ReleaseBlock>
void Reclaim(const Bytes& space,const Bytes& queue,size_t field,uint64_t base,uint64_t limit,
             uint64_t committed,ReleaseBlock releaseBlock)
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
    auto updated=EncodeLeaf(records,0x0c,16,8);
    std::copy_n(queue.begin(),size_t(32),updated.begin());
    queue=std::move(updated);
    Set(space,field,retired,8);
    Set(space,field+16,transaction,8);
}

template<class RequiredMetadata>
std::vector<uint64_t> Allocate(Writes& writes,Source& source,const Container& container,
                              Bytes& space,Bytes& superblock,const CheckpointObjectLookup& ephemeral,
                              const std::vector<uint64_t>& retire,uint64_t extents,uint64_t snapshots,
                              const std::vector<FileRun>& files,std::vector<DataRun> retiredData,
                              uint64_t dataBlocks,std::vector<DataRun>& dataRuns,
                              uint64_t transaction,RequiredMetadata requiredMetadata)
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
    verify(extents,1);
    verify(snapshots,1);

    const auto within=[](uint64_t block,uint64_t start,uint64_t count){
        return block>=start&&block-start<count;
    };
    const auto isMetadata=[&](uint64_t block){
        return !block||block==extents||block==snapshots||
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
    for(uint64_t chunk=0;chunk<chunks&&dataBlocks;++chunk){
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

void ValidateExtentReferences(const Node& tree,std::vector<FileRun>& files)
{
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    Check(!tree.Level()&&tree.Count()==files.size(),"APFS extent reference count disagrees with the catalog");
    uint64_t end=0;
    for(size_t i=0;i<files.size();++i){
        const auto& run=files[i];
        const auto record=tree.At(i);
        const auto expected=ExtentReference(run);
        Check(run.block>=end&&Matches(record.key,expected.key)&&Matches(record.value,expected.value),
              "Invalid, shared or overlapping APFS extent reference");
        end=run.block+run.count;
    }
}

constexpr auto ResourceAttribute="com.apple.ResourceFork";
constexpr auto CompressionAttribute="com.apple.decmpfs";
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

std::vector<OwnedRecord> EditCatalog(std::vector<OwnedRecord> records,const CatalogMutation& request,const std::string& name,
    uint64_t nextId,uint64_t entryId,uint64_t time,bool fold,uint64_t directoryCount,uint64_t fileCount,
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
    if(create||rename){
        added=WriterDisk::DirectoryEntry(name,entryId,time,parent,fold,
                                         uint16_t(create?create->kind:CatalogKind::Directory));
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
    struct AttributeStream {uint64_t inode,blocks;};
    std::map<uint64_t,AttributeStream> attributeStreams;
    uint64_t files=0;
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
            uint32_t hash=0;
            std::string storedName;
            if(type==9){
                const auto folder=directories.find(inode);
                Check(inode!=3&&folder!=directories.end()&&view.key.size>=14&&
                      view.value.size==18,"Invalid APFS directory entry");
                const auto kind=CatalogKind(Le16(view.value.data+16));
                Check(kind==CatalogKind::Directory||kind==CatalogKind::RegularFile,
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
                renamingThisEntry=rename&&rename->inode==child&&parent==inode;
                const auto canonicalName=NormalizedName(childName,fold);
                Check(directoryNames[inode].insert(canonicalName).second,"Duplicate APFS directory name");
                Check(!addedKey||parent!=inode||renamingThisEntry||canonicalName!=requestedName,
                      "The APFS entry name already exists");
                storedName=childName;
                Check(child>=16&&child>inode&&child<nextId&&links.emplace(child,Link{inode,kind,std::move(childName),i}).second,
                      "Invalid or duplicate APFS directory entry ID");
                ++folder->second.children;
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
                bool hasSecurity=false;
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
                        Check(name>previousAttribute&&name.find('\0')==name.npos&&(OrdinaryAttribute(name)||name==ResourceAttribute||name==CompressionAttribute),
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
                        if(storage==1){
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
                }
                Check(bool(Le64(view.value.data+48)&HasSecurityAttribute)==hasSecurity,
                      "APFS security attribute and inode flag disagree");
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
                    auto expected=WriterDisk::Inode(primaryName,inode,0,parentId,0100644,1,size,finder?&*finder:nullptr);
                    SetResourceFlag(expected.value,link->second.resourceRecord.has_value());
                    if(hasSecurity)Set(expected.value,48,Le64(expected.value.data()+48)|HasSecurityAttribute,8);
                    Set(expected.value,68,Le32(value+68)&UserImmutable,4);
                    const auto compression=link->second.compressionRecord;
                    if(compression){
                        Check(!size,"Compressed APFS files cannot have a data stream");
                        SetCompressionFlags(expected.value,Le64(records[*compression].value.data()+12),true);
                    }
                    Check(mode==0100000&&view.value.size==expected.value.size()&&
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
                    ++files;
                }
            }
            // Catalog keys sort by inode ID, record type, then directory name hash.
            const Key order{inode,type,hash,std::move(storedName)};
            Check(order>previous,"Invalid or unsupported APFS directory key order");
            // A regular file cannot be a destination parent, so its stream
            // records always sort on the same side of the insertion point.
            if(addedKey&&!renamingThisEntry&&order<*addedKey)insertAt+=1+streamRecords;
            previous=order;
        }
        i+=streamRecords;
    }
    Check(attributeStreams.empty()&&directories.count(2)&&directories.count(3)&&directories.size()==directoryCount+2&&
          files==fileCount&&links.size()==directoryCount+fileCount,"APFS catalog records and volume counts disagree");
    for(const auto& entry:directories){
        const auto& folder=entry.second;
        Check(Le32(records[folder.record].value.data()+56)==folder.children,"Invalid APFS directory child count");
    }
    const auto target=directories.find(parent);
    Check(parent!=3&&target!=directories.end(),"The APFS destination folder does not exist");
    auto& parentInode=records[target->second.record].value;
    Check(!(Le32(parentInode.data()+68)&(UserImmutable|SystemImmutable)),
          "Unlock the APFS destination folder before making changes");
    if(!replace&&!metadata&&!attributesEdit&&!resourceEdit){
        Set(parentInode,24,time,8);
        Set(parentInode,32,time,8);
    }
    if(create){
        const bool isDirectory=create->kind==CatalogKind::Directory;
        Set(parentInode,56,target->second.children+1,4);
        records.insert(records.begin()+insertAt,std::move(added));
        records.push_back(WriterDisk::Inode(name,entryId,time,parent,isDirectory?0040755:0100644,isDirectory?0:1,dataSize,create->finder));
        if(create->locked)Set(records.back().value,68,UserImmutable,4);
        if(create->attributes)SetSecurityFlag(records.back().value,*create->attributes);
        copyDates(records.back().value);
        auto attributes=EncodeAttributes(entryId,create->attributes,resourceWrite,streams);
        if(resourceWrite)SetResourceFlag(records.back().value,resourceWrite->size!=0);
        SortAttributes(attributes);
        records.insert(records.end(),std::make_move_iterator(attributes.begin()),std::make_move_iterator(attributes.end()));
        streamAt=records.size();
    }else{
        const auto found=links.find(entryId);
        Check(found!=links.end()&&found->second.parent==parent,"The APFS entry is no longer in the selected folder");
        const auto& link=found->second;
        Check(!replace||link.kind==CatalogKind::RegularFile,"APFS data replacement requires a regular file");
        auto& inode=records[link.inodeRecord].value;
        const auto bsdFlags=Le32(inode.data()+68);
        Check(!(bsdFlags&SystemImmutable)&&(!(bsdFlags&UserImmutable)||(metadata&&metadata->finder&&!metadata->locked)),
              "Unlock the APFS entry before making changes");
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
                const bool isResource=link.resourceRecord==index;
                if(isResource?!resource:!ordinary)attributes.push_back(std::move(records[index]));
                else retireAttribute(index);
            }
            if(ordinary)SetSecurityFlag(inode,*ordinary);
            if(resource)SetResourceFlag(inode,resource->size!=0);
            if(replace){
                uint64_t logicalSize=0;
                if(const auto compression=replace->compression){
                    Check(compression->size()>=16&&compression->size()<EmbeddedAttributeLimit&&
                          Le32(compression->data())==0x636d7066&&!dataSize,"Invalid APFS compressed file output");
                    auto record=EmbeddedAttribute(entryId,CompressionAttribute,compression->size());
                    std::copy(compression->begin(),compression->end(),record.value.begin()+4);
                    attributes.push_back(std::move(record));
                    logicalSize=Le64(compression->data()+8);
                }
                SetCompressionFlags(inode,logicalSize,replace->compression!=nullptr);
            }
            SortAttributes(attributes);
            records.erase(records.begin()+afterInode,records.begin()+afterInode+link.attributes+fileRecords);
            records.insert(records.begin()+afterInode,std::make_move_iterator(attributes.begin()),
                           std::make_move_iterator(attributes.end()));
            return afterInode+attributes.size();
        };
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
        const auto old=WriterDisk::Inode(link.name,entryId,0,parent,mode,count,link.dataSize,existingFinder);
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
            records.erase(first,first+1+link.attributes+link.streamRecords);
            records.erase(records.begin()+link.entryRecord);
            return records;
        }
        const auto updatedFinder=metadata?metadata->finder:replace&&replace->finder?replace->finder:existingFinder;
        const auto& updatedName=(replace||metadata)?link.name:name;
        auto updated=WriterDisk::Inode(updatedName,entryId,0,parent,mode,count,
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

std::vector<Mapping> StageCatalog(Writes& writes,const CatalogLeaves& leaves,const std::vector<uint64_t>& blocks,
                                  size_t firstChild,uint64_t rootOid,uint64_t firstOid,uint64_t tx)
{
    std::vector<Mapping> mappings{{rootOid,blocks[0]}};
    if(leaves.size()==1){
        StageObject(writes,blocks[0],EncodeLeaf(leaves.front(),0x40),rootOid,tx,2,14);
        return mappings;
    }
    std::vector<OwnedRecord> index;
    size_t keys=0,longestKey=0,longestValue=8;
    for(size_t i=0;i<leaves.size();++i){
        const auto& leaf=leaves[i];
        const auto oid=firstOid+i,block=blocks[firstChild+i];
        OwnedRecord child{leaf.front().key,Bytes(8)};
        Set(child.value,0,oid,8);
        index.push_back(std::move(child));
        keys+=leaf.size();
        for(const auto& record:leaf){
            longestKey=(std::max)(longestKey,record.key.size());
            longestValue=(std::max)(longestValue,record.value.size());
        }
        StageObject(writes,block,WriterDisk::EncodeNode(leaf,0x40,false,0),oid,tx,3,14);
        mappings.push_back({oid,block});
    }
    auto root=WriterDisk::EncodeNode(index,0x40,true,1);
    constexpr auto footer=Block-WriterDisk::TreeFooter;
    Set(root,footer+16,longestKey,4);
    Set(root,footer+20,longestValue,4);
    Set(root,footer+24,keys,8);
    Set(root,footer+32,leaves.size()+1,8);
    StageObject(writes,blocks[0],std::move(root),rootOid,tx,2,14);
    return mappings;
}
void CheckNativeVolume(const Bytes& volume)
{
    const auto flags=Le64(volume.data()+56);
    Check(!memcmp(volume.data()+32,"APSB",4)&&!memcmp(volume.data()+272,"HFVExplorer",12)&&
          (flags==1||flags==8)&&Le64(volume.data()+264)==1&&Le32(volume.data()+116)==2&&
          !Le64(volume.data()+200)&&!Le64(volume.data()+208)&&!Le64(volume.data()+216),
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

void ValidateWritableLayout(Source& source,const Container& container)
{
    Check(!container.offset&&container.checkpointBlock&&container.blockSize==Block&&
          !container.keybagBlock&&!container.keybagBlocks&&container.volumes.size()==1&&
          source.Size()==container.blockCount*Block,"APFS writes require a raw single-volume image");
    const auto oid=container.volumes.front();
    const auto map=ReadWritableMap(source,container,container.objectMap);
    Check(map.mappings.size()==1&&map.mappings.front().oid==oid,
          "APFS writes require one container volume mapping");
    const auto volume=ReadObject(source,container,map.mappings.front().block,oid,container.transaction,13);
    CheckNativeVolume(volume);
    const auto extents=Le64(volume.data()+144),snapshots=Le64(volume.data()+152);
    const Node extentTree(ReadObject(source,container,extents,extents,container.transaction,2,15,true),true,{});
    const Node snapshotTree(ReadObject(source,container,snapshots,snapshots,container.transaction,2,16,true),true,{});
    Check(!extentTree.Level()&&!snapshotTree.Level()&&!snapshotTree.Count(),"Unsupported APFS extent or snapshot tree");
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
    const auto name=create?EncodeCatalogName(create->name):rename?EncodeCatalogName(rename->name):std::string{};
    const auto* replace=std::get_if<CatalogReplaceData>(&request);
    auto dates=replace?replace->dates:create?create->dates:std::optional<FileDates>{};
    CatalogWritePlan plan;
    auto* input=replace?&replace->data:create?create->data:nullptr;
    const auto dataSize=input?input->Size():0;
    if(dataSize)plan.streams.push_back({*input,dataSize});
    const auto fileBlocks=DivideUp(dataSize,Block);
    const auto* resourceEdit=std::get_if<CatalogResourceFork>(&request);
    auto* resourceInput=resourceEdit?&resourceEdit->resource:replace?replace->resource:create?create->resource:nullptr;
    std::optional<ResourceWrite> resource;
    if(resourceInput)resource=ResourceWrite{resourceInput,resourceInput->Size(),0};
    const bool compressionResource=replace&&replace->compression&&resource&&resource->size;
    const auto resourceBlocks=resource&&(resource->size>=EmbeddedAttributeLimit||compressionResource)?
                              DivideUp(resource->size,Block):0;
    Check(fileBlocks<=current.blockCount&&resourceBlocks<=current.blockCount-fileBlocks,
          "APFS forks exceed the container size");
    auto dataBlocks=fileBlocks+resourceBlocks;
    if(resourceBlocks)plan.streams.push_back({*resourceInput,resource->size});
    const auto oid=current.volumes.front();
    const auto containerMap=ReadWritableMap(source,current,current.objectMap);
    Check(containerMap.mappings.size()==1&&containerMap.mappings.front().oid==oid,
          "APFS writes require one container volume mapping");
    const auto volumeAddress=containerMap.mappings.front().block;
    auto volume=ReadObject(source,current,volumeAddress,oid,current.transaction,13);
    const auto flags=Le64(volume.data()+56);
    const auto fileCount=Le64(volume.data()+184);
    const auto directoryCount=Le64(volume.data()+192);
    const auto catalogOid=Le64(volume.data()+136);
    const auto extents=Le64(volume.data()+144),snapshots=Le64(volume.data()+152);
    CheckNativeVolume(volume);
    const Node extentTree(ReadObject(source,current,extents,extents,current.transaction,2,15,true),true,{});
    const Node snapshotTree(ReadObject(source,current,snapshots,snapshots,current.transaction,2,16,true),true,{});
    Check(!snapshotTree.Level()&&!snapshotTree.Count(),"APFS catalog writes require an empty snapshot tree");
    const auto volumeMap=ReadWritableMap(source,current,Le64(volume.data()+128));
    auto existing=ReadWritableCatalog(source,current,volumeMap,catalogOid);
    const auto nextId=Le64(volume.data()+176);
    Check(nextId>=16&&nextId<IdMask,"Invalid APFS next inode ID");
    auto nextAvailable=nextId+bool(create)+bool(resourceBlocks);
    Check(nextAvailable<IdMask,"APFS file-system object IDs are exhausted");
    if(resourceBlocks){
        resource->id=nextId+bool(create);
        plan.streams.back().id=resource->id;
    }
    const auto* attributeEdit=std::get_if<CatalogAttributes>(&request);
    const auto* attributes=attributeEdit?&attributeEdit->attributes:replace?replace->attributes:create?create->attributes:nullptr;
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
        else return edit.inode;
    },request);
    std::set<uint64_t> retiredStreams;
    auto catalog=EditCatalog(std::move(existing.records),request,name,nextId,entryId,time,flags==1,
                            directoryCount,fileCount,dataSize,current.blockCount,files,removedKind,streamAt,dates,resource?&*resource:nullptr,attributeIds,retiredStreams);
    ValidateExtentReferences(extentTree,files);
    if(!retiredStreams.empty()){
        catalog.erase(std::remove_if(catalog.begin(),catalog.end(),[&](const auto& record){
            const auto key=Le64(record.key.data());
            return key>>60==8&&retiredStreams.count(key&IdMask);
        }),catalog.end());
    }
    const bool retireFile=replace||removedKind.has_value();
    const auto retiredRun=[&](const FileRun& run){
        return run.stream?retiredStreams.count(run.stream)!=0:retireFile&&run.owner==entryId;
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
    if(changeExtents)retired.push_back(extents);
    const size_t firstChild=changeExtents?7:6;
    CatalogLeaves leaves;
    size_t children=0;
    std::vector<DataRun> allocatedData;
    const auto allocated=Allocate(writes,source,current,space,superblock,ephemeral,
        retired,extents,snapshots,files,std::move(retiredData),dataBlocks,allocatedData,transaction,[&]{
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
            return firstChild+children;
        });
    if(changeExtents){
        files.erase(std::remove_if(files.begin(),files.end(),retiredRun),files.end());
        if(fileBlocks)
            for(const auto& data:plan.streams.front().runs)files.push_back({entryId,data.block,data.count});
        for(const auto& stream:plan.streams)if(stream.id)
            for(const auto& data:stream.runs)files.push_back({entryId,data.block,data.count,stream.id});
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.block<b.block;});
        std::vector<OwnedRecord> references;
        for(const auto& file:files)references.push_back(ExtentReference(file));
        StageObject(writes,allocated[6],EncodeLeaf(references,0x50),allocated[6],transaction,Physical|2,15);
        Set(volume,144,allocated[6],8);
    }
    const auto firstOid=Le64(superblock.data()+88);
    if(children){
        Check(firstOid>volumeMap.mappings.back().oid&&firstOid>oid&&children<=UINT64_MAX-firstOid,
              "APFS virtual object ID space is exhausted or invalid");
        Set(superblock,88,firstOid+children,8);
    }
    const auto mappings=StageCatalog(writes,leaves,allocated,firstChild,catalogOid,firstOid,transaction);
    StageMap(writes,volumeMap,allocated[1],allocated[2],mappings,transaction);
    const auto oldChildren=existing.blocks.size()-1;
    const auto volumeBlocks=Le64(volume.data()+88);
    const auto removedVolumeBlocks=oldChildren+removedBlocks;
    Check(volumeBlocks>=removedVolumeBlocks&&volumeBlocks<=current.blockCount&&
          dataBlocks+children<=current.blockCount-(volumeBlocks-removedVolumeBlocks),
          "Invalid APFS volume allocation count");
    Set(volume,88,volumeBlocks-removedVolumeBlocks+children+dataBlocks,8);
    Set(volume,128,allocated[1],8);
    Set(volume,176,nextAvailable,8);
    if(create){
        if(create->kind==CatalogKind::Directory)Set(volume,192,directoryCount+1,8);
        else Set(volume,184,fileCount+1,8);
    }else if(removedKind){
        if(*removedKind==CatalogKind::Directory)Set(volume,192,directoryCount-1,8);
        else Set(volume,184,fileCount-1,8);
    }
    Set(volume,256,time,8);
    const uint64_t replaced=changeExtents?4:3;
    const auto newVolumeBlocks=replaced+children+dataBlocks,oldVolumeBlocks=replaced+removedVolumeBlocks;
    Check(Le64(volume.data()+224)<=UINT64_MAX-newVolumeBlocks&&Le64(volume.data()+232)<=UINT64_MAX-oldVolumeBlocks,
          "APFS volume allocation history overflow");
    Set(volume,224,Le64(volume.data()+224)+newVolumeBlocks,8);
    Set(volume,232,Le64(volume.data()+232)+oldVolumeBlocks,8);
    StageObject(writes,allocated[3],std::move(volume),oid,transaction,13);
    StageMap(writes,containerMap,allocated[4],allocated[5],{{oid,allocated[3]}},transaction);
    Set(superblock,160,allocated[4],8);
    return plan;
}
}
