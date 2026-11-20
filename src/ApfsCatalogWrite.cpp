// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsCatalogWrite.h"
#include "ApfsTree.h"
#include "ApfsWriteDisk.h"
#include <windows.h>
#include <map>
#include <tuple>

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
struct FileRun {
    uint64_t owner,block,count;
};
constexpr uint64_t Physical=0x40000000,Ephemeral=0x80000000,Chunk=Block*8;
constexpr uint64_t IdMask=(uint64_t(1)<<60)-1;
constexpr size_t MainFreeQueue=240,PoolFreeQueue=200;

std::string CatalogName(const std::u16string& name)
{
    Check(!name.empty()&&name.size()<=255&&name!=u"."&&name!=u"..","Invalid APFS entry name");
    std::string result;
    for(const auto c:name){
        Check(c>=32&&c<127&&c!='/'&&c!=':',"APFS catalog updates currently require an ASCII entry name");
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
                              const std::vector<FileRun>& files,uint64_t dataBlocks,uint64_t& dataBlock,
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
    const auto isMetadata=[&](uint64_t block){
        return !block||block==extents||block==snapshots||
               within(block,Le64(superblock.data()+112),Le32(superblock.data()+104))||
               within(block,Le64(superblock.data()+120),Le32(superblock.data()+108))||
               within(block,currentBitmapBase,bitmapSlots)||within(block,pool,poolSize)||
               std::find(retire.begin(),retire.end(),block)!=retire.end();
    };
    for(const auto& run:files){
        VerifyAllocated(mainMap,run.block,run.count,mainLimit);
        for(uint64_t i=0;i<run.count;++i)
            Check(!isMetadata(run.block+i),"APFS file extent overlaps live metadata");
    }
    const auto reclaimed=Reclaim(space,mainQueue,MainFreeQueue,mainMap,0,mainLimit,container.transaction,[&](uint64_t block){
        return isMetadata(block)||std::any_of(files.begin(),files.end(),[&](const auto& run){
            return within(block,run.block,run.count);
        });
    });
    // Select the contiguous data run before distributing metadata blocks.
    uint64_t available=0,consecutive=0;
    for(uint64_t i=0;i<mainLimit;++i){
        if(Bit(mainMap,i)){
            consecutive=0;
            continue;
        }
        ++available;
        if(++consecutive==dataBlocks&&!dataBlock)dataBlock=i+1-dataBlocks;
    }
    Check(!dataBlocks||dataBlock,"No contiguous APFS space for the file data");
    std::vector<uint64_t> result;
    for(uint64_t i=0;i<mainLimit&&result.size()<retire.size();++i)
        if(!Bit(mainMap,i)&&!within(i,dataBlock,dataBlocks))result.push_back(i);
    Check(available>=reclaimed&&available-reclaimed==Le32(cib.data()+60)&&
          available-reclaimed+(container.blockCount-mainLimit)==Le64(space.data()+72)&&
          result.size()==retire.size(),"APFS allocation counts disagree or the active chunk is full");

    Retire(space,mainQueue,MainFreeQueue,retire,transaction);
    Retire(space,poolQueue,PoolFreeQueue,{oldCib,oldMainBitmap},transaction);

    for(const auto block:result)SetBit(mainMap,block);
    for(uint64_t i=0;i<dataBlocks;++i)SetBit(mainMap,dataBlock+i);
    for(const auto block:internal)SetBit(poolMap,block-pool);
    writes.push_back({internal[0],std::move(mainMap)});
    writes.push_back({currentBitmapBase+head,std::move(poolMap)});
    Set(cib,40,transaction,8);
    Set(cib,60,available-result.size()-dataBlocks,4);
    Set(cib,64,internal[0],8);
    StageObject(writes,internal[1],std::move(cib),internal[1],transaction,Physical|7);
    Set(space,cibOffset,internal[1],8);
    Set(space,72,available+(container.blockCount-mainLimit)-result.size()-dataBlocks,8);
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

OwnedRecord FileExtent(const FileRun& run)
{
    OwnedRecord record{Bytes(16),Bytes(24)};
    Set(record.key,0,(uint64_t(8)<<60)|run.owner,8);
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

std::vector<OwnedRecord> AddCatalogEntry(Bytes catalog,const CatalogCreate& request,const std::string& name,
    uint64_t id,uint64_t time,bool fold,uint64_t directoryCount,uint64_t fileCount,uint64_t dataSize,
    std::vector<FileRun>& fileRuns)
{
    const Node tree(std::move(catalog),true,{});
    Check(tree.Level()==0&&directoryCount<=tree.Count()&&fileCount<=tree.Count(),
          "APFS catalog writes require single-leaf catalogs");
    const auto parent=request.parent;
    const bool isDirectory=request.kind==CatalogKind::Directory;
    const bool privateFirst=WriterDisk::DirectoryHash("private-dir")<WriterDisk::DirectoryHash("root");
    const uint64_t reserved[]={privateFirst?3u:2u,privateFirst?2u:3u};
    auto added=WriterDisk::DirectoryEntry(name,id,time,parent,fold,uint16_t(request.kind));
    using Key=std::tuple<uint64_t,uint64_t,uint32_t>;
    const Key addedKey{parent,9,Le32(added.key.data()+8)>>10};
    Key previous{};
    size_t insertAt=2;
    struct Directory {
        size_t record;
        uint32_t children=0;
    };
    std::map<uint64_t,Directory> directories;
    struct Link {
        uint64_t parent;
        CatalogKind kind;
        std::string name;
    };
    std::map<uint64_t,Link> links;
    uint64_t files=0;
    std::vector<OwnedRecord> records;
    for(size_t i=0;i<tree.Count();++i){
        const auto view=tree.At(i);
        bool streamRecords=false;
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
                auto childName=CatalogName(std::u16string(view.key.data+12,view.key.data+view.key.size-1));
                hash=nameKey>>10;
                Check((WriterDisk::DirectoryHash(childName,fold)>>10)==hash,"Invalid APFS directory name hash");
                const auto child=Le64(view.value.data);
                Check(child>=16&&child>inode&&child<id&&links.emplace(child,Link{inode,kind,std::move(childName)}).second,
                      "Invalid or duplicate APFS directory entry ID");
                ++folder->second.children;
            }else{
                Check(type==3&&view.key.size==8&&view.value.size>=92&&Le64(view.value.data+8)==inode,
                      "Invalid APFS inode");
                const auto parentId=Le64(view.value.data);
                const bool reservedInode=inode==2||inode==3;
                const auto link=links.find(inode);
                Check(reservedInode?parentId==1:link!=links.end()&&link->second.parent==parentId,
                      "Invalid APFS inode parent");
                const auto kind=reservedInode?CatalogKind::Directory:link->second.kind;
                const auto mode=Le16(view.value.data+80)&0170000;
                if(kind==CatalogKind::Directory){
                    Check(mode==0040000&&directories.emplace(inode,Directory{i}).second,"Invalid APFS directory inode");
                }else{
                    const auto& primaryName=link->second.name;
                    const auto value=view.value.data;
                    const size_t stream=104+size_t(DivideUp(primaryName.size()+1,8)*8);
                    const auto size=view.value.size==stream+40?Le64(value+stream):0;
                    Check(size<=Block*Chunk,"Unsupported APFS file size in writable catalog");
                    const auto expected=WriterDisk::Inode(primaryName,inode,0,parentId,0100644,1,size);
                    Check(mode==0100000&&view.value.size==expected.value.size()&&
                          !memcmp(value+48,expected.value.data()+48,view.value.size-48),
                          "APFS writes require native regular files with one data extent at most");
                    if(size){
                        Check(i+2<tree.Count(),"Missing APFS data-stream records");
                        const auto reference=tree.At(i+1),extent=tree.At(i+2);
                        const auto streamRef=StreamReference(inode);
                        Check(extent.value.size==24,"Invalid APFS file extent size");
                        const FileRun run{inode,Le64(extent.value.data+8),DivideUp(size,Block)};
                        Check(run.block&&Range(run.block,run.count,Chunk),"APFS file extent outside the writable chunk");
                        const auto encoded=FileExtent(run);
                        Check(Matches(reference.key,streamRef.key)&&Matches(reference.value,streamRef.value)&&
                              Matches(extent.key,encoded.key)&&Matches(extent.value,encoded.value),
                              "Invalid APFS data-stream or extent record");
                        fileRuns.push_back(run);
                        streamRecords=true;
                    }
                    ++files;
                }
            }
            // Catalog keys sort by inode ID, record type, then directory name hash.
            const Key order{inode,type,hash};
            Check(order>previous,"Invalid or unsupported APFS directory key order");
            Check(order!=addedKey,"The APFS entry name already exists or has a conflicting hash");
            // A regular file cannot be a destination parent, so its stream
            // records always sort on the same side of the insertion point.
            if(order<addedKey)insertAt+=streamRecords?3:1;
            previous=order;
        }
        records.push_back({Bytes(view.key.data,view.key.data+view.key.size),
                           Bytes(view.value.data,view.value.data+view.value.size)});
        if(streamRecords){
            records.push_back(StreamReference(fileRuns.back().owner));
            records.push_back(FileExtent(fileRuns.back()));
            i+=2;
        }
    }
    Check(directories.count(2)&&directories.count(3)&&directories.size()==directoryCount+2&&
          files==fileCount&&links.size()==directoryCount+fileCount,"APFS catalog records and volume counts disagree");
    for(const auto& entry:directories){
        const auto& folder=entry.second;
        Check(Le32(records[folder.record].value.data()+56)==folder.children,"Invalid APFS directory child count");
    }
    const auto target=directories.find(parent);
    Check(parent!=3&&target!=directories.end(),"The APFS destination folder does not exist");
    auto& parentInode=records[target->second.record].value;
    Set(parentInode,24,time,8);
    Set(parentInode,32,time,8);
    Set(parentInode,56,target->second.children+1,4);
    records.insert(records.begin()+insertAt,std::move(added));
    records.push_back(WriterDisk::Inode(name,id,time,parent,isDirectory?0040755:0100644,isDirectory?0:1,dataSize));
    return records;
}
}

CatalogWritePlan PrepareCatalogCreate(
    Source& source,const Container& current,uint64_t transaction,Bytes& superblock,
    const CheckpointObjectLookup& ephemeral,const CatalogCreate& request)
{
    Check(current.blockSize==Block,"APFS catalog writes currently require 4096-byte blocks");
    const auto ascii=CatalogName(request.name);
    CatalogWritePlan plan;
    plan.dataSize=request.data?request.data->Size():0;
    const auto dataBlocks=DivideUp(plan.dataSize,Block);
    Check(dataBlocks<=(std::min)(current.blockCount,Chunk),"APFS file exceeds the writable allocation chunk");
    const auto oid=current.volumes.front();
    const auto containerMap=ReadSingleMapping(source,current,current.objectMap,oid);
    auto volume=ReadObject(source,current,containerMap.physical,oid,current.transaction,13);
    const auto flags=Le64(volume.data()+56);
    const auto fileCount=Le64(volume.data()+184);
    const auto directoryCount=Le64(volume.data()+192);
    const auto catalogOid=Le64(volume.data()+136);
    const auto extents=Le64(volume.data()+144),snapshots=Le64(volume.data()+152);
    Check(!memcmp(volume.data()+32,"APSB",4)&&!memcmp(volume.data()+272,"HFVExplorer",12)&&
          (flags==1||flags==8)&&Le64(volume.data()+264)==1&&Le32(volume.data()+116)==2&&
          !Le64(volume.data()+200)&&!Le64(volume.data()+208)&&!Le64(volume.data()+216),
          "APFS catalog writes currently require an unencrypted volume formatted by HFVExplorer");
    const Node extentTree(ReadObject(source,current,extents,extents,current.transaction,2,15,true),true,{});
    const Node snapshotTree(ReadObject(source,current,snapshots,snapshots,current.transaction,2,16,true),true,{});
    Check(!snapshotTree.Level()&&!snapshotTree.Count(),"APFS catalog writes require an empty snapshot tree");
    const auto volumeMap=ReadSingleMapping(source,current,Le64(volume.data()+128),catalogOid);
    const auto nextId=Le64(volume.data()+176);
    Check(nextId>=16&&nextId<IdMask,"Invalid APFS next inode ID");
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    const auto time=(((uint64_t(fileTime.dwHighDateTime)<<32)|fileTime.dwLowDateTime)-116444736000000000ULL)*100;
    std::vector<FileRun> files;
    auto catalog=AddCatalogEntry(ReadObject(source,current,volumeMap.physical,catalogOid,current.transaction,2,14),
                                 request,ascii,nextId,time,flags==1,directoryCount,fileCount,plan.dataSize,files);
    ValidateExtentReferences(extentTree,files);
    auto& space=ephemeral(Le64(superblock.data()+152));
    auto& writes=plan.metadata;
    std::vector<uint64_t> retired{containerMap.headerBlock,containerMap.rootBlock,containerMap.physical,
                                  volumeMap.headerBlock,volumeMap.rootBlock,volumeMap.physical};
    if(dataBlocks)retired.push_back(extents);
    const auto allocated=Allocate(writes,source,current,space,superblock,ephemeral,
                                  retired,extents,snapshots,files,dataBlocks,plan.dataBlock,transaction);
    if(dataBlocks){
        const FileRun run{nextId,plan.dataBlock,dataBlocks};
        catalog.push_back(StreamReference(nextId));
        catalog.push_back(FileExtent(run));
        const auto at=std::lower_bound(files.begin(),files.end(),run.block,
            [](const auto& file,uint64_t block){return file.block<block;});
        files.insert(at,run);
        std::vector<OwnedRecord> references;
        for(const auto& file:files)references.push_back(ExtentReference(file));
        StageObject(writes,allocated[6],EncodeLeaf(references,0x50),allocated[6],transaction,Physical|2,15);
        Set(volume,144,allocated[6],8);
        const auto volumeBlocks=Le64(volume.data()+88);
        Check(volumeBlocks<=current.blockCount-dataBlocks,"Invalid APFS volume allocation count");
        Set(volume,88,volumeBlocks+dataBlocks,8);
    }
    StageObject(writes,allocated[0],EncodeLeaf(catalog,0x40),catalogOid,transaction,2,14);
    StageMap(writes,volumeMap,allocated[1],allocated[2],catalogOid,allocated[0],transaction);
    Set(volume,128,allocated[1],8);
    Set(volume,176,nextId+1,8);
    if(request.kind==CatalogKind::Directory)Set(volume,192,directoryCount+1,8);
    else Set(volume,184,fileCount+1,8);
    Set(volume,256,time,8);
    const uint64_t replaced=dataBlocks?4:3;
    Check(Le64(volume.data()+224)<=UINT64_MAX-replaced-dataBlocks&&Le64(volume.data()+232)<=UINT64_MAX-replaced,
          "APFS volume allocation history overflow");
    Set(volume,224,Le64(volume.data()+224)+replaced+dataBlocks,8);
    Set(volume,232,Le64(volume.data()+232)+replaced,8);
    StageObject(writes,allocated[3],std::move(volume),oid,transaction,13);
    StageMap(writes,containerMap,allocated[4],allocated[5],oid,allocated[3],transaction);
    Set(superblock,160,allocated[4],8);
    return plan;
}
}
