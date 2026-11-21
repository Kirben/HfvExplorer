// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsContainer.h"
#include "ApfsCatalogWrite.h"
#include "DiskPartitions.h"
#include "HfsPlusDisk.h"
#include "StorageWorker.h"

namespace Apfs {
namespace {
using namespace HfsPlus;
using Disk::Check;
using Disk::Range;
uint32_t Le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
uint64_t Le64(const uint8_t* p){return Le32(p) | uint64_t(Le32(p+4))<<32;}
constexpr uint64_t MinimumSize=1024*1024;
constexpr size_t MagicOffset=32;
constexpr size_t VolumeArrayOffset=184;
constexpr uint32_t MaxVolumes=100;
constexpr size_t MapHeader=40,MappingBytes=40;

Container Superblock(const Bytes& block,uint64_t offset,uint64_t length)
{
    Check(block.size()>=4096&&!memcmp(block.data()+MagicOffset,"NXSB",4),"Missing APFS container signature");
    const auto data=block.data();
    const auto blockSize=Le32(data+36);
    const auto blocks=Le64(data+40);
    Check(blockSize>=4096&&blockSize<=65536&&(blockSize&(blockSize-1))==0&&
          block.size()==blockSize&&blocks>=MinimumSize/blockSize&&blocks<=length/blockSize,
          "Invalid APFS block geometry");
    Check(Le64(data)==ObjectChecksum(block),"APFS superblock checksum mismatch");
    Check(Le64(data+8)==1&&(Le32(data+24)&0xffff)==1&&Le32(data+28)==0,
          "Invalid APFS container object header");
    Container result{offset,blocks,Le64(data+16),Le64(data+160),blockSize,{},{}};
    memcpy(result.uuid.data(),data+72,result.uuid.size());
    result.keybagBlock=Le64(data+1296);
    result.keybagBlocks=Le64(data+1304);
    Check(result.transaction&&result.objectMap&&result.objectMap<blocks,"Invalid APFS container object map");
    const auto capacity=Le32(data+180);
    Check(capacity<=MaxVolumes,"APFS volume table exceeds its superblock");
    std::set<uint64_t> ids;
    for(uint32_t i=0;i<MaxVolumes;++i){
        const auto id=Le64(data+VolumeArrayOffset+i*8);
        if(!id)continue;
        Check(i<capacity&&ids.insert(id).second,"Invalid or duplicate APFS volume ID");
        result.volumes.push_back(id);
    }
    return result;
}

struct Ring {
    uint64_t base;
    uint32_t capacity,index,used;
};
Ring ReadRing(const uint8_t* data,uint64_t blocks,bool descriptors)
{
    const auto base=Le64(data+(descriptors?112:120));
    const auto count=Le32(data+(descriptors?104:108));
    Check(!(count&0x80000000),"Noncontiguous APFS checkpoint areas are unsupported");
    Check(!count||(base&&Range(base,count,blocks)),"APFS checkpoint area extends outside the container");
    return {base,count,Le32(data+(descriptors?136:144)),Le32(data+(descriptors?140:148))};
}
uint64_t Slot(const Ring& ring,uint64_t index)
{
    return ring.base+index%ring.capacity;
}

bool ValidMap(const Bytes& map,uint64_t transaction,bool last)
{
    const auto p=map.data();
    return Le64(p)==ObjectChecksum(map)&&(Le32(p+24)&0xffff)==12&&Le64(p+16)==transaction&&
           Le32(p+32)==unsigned(last)&&Le32(p+36)<=(map.size()-MapHeader)/MappingBytes;
}
std::optional<uint32_t> DataIndex(const Ring& data,uint64_t block,uint32_t count)
{
    if(!count||block<data.base||block-data.base>=data.capacity)return {};
    const auto relative=(block-data.base+data.capacity-data.index)%data.capacity;
    if(!Range(relative,count,data.used))return {};
    return uint32_t(relative);
}

bool ValidateMappings(Source& source,const Container& container,const Ring& desc,const Ring& data)
{
    std::set<std::pair<uint64_t,uint64_t>> objects;
    for(uint32_t i=0;i+1<desc.used;++i){
        const auto slot=Slot(desc,uint64_t(desc.index)+i);
        const auto map=Disk::Read(source,container.offset+slot*container.blockSize,container.blockSize);
        const auto p=map.data();
        if(!ValidMap(map,container.transaction,i+2==desc.used))return false;
        const auto count=Le32(p+36);
        for(uint32_t j=0;j<count;++j){
            const auto entry=p+MapHeader+j*MappingBytes;
            const auto size=Le32(entry+8);
            const auto volume=Le64(entry+16),id=Le64(entry+24),physical=Le64(entry+32);
            if(size%container.blockSize||!DataIndex(data,physical,size/container.blockSize)||
               !id||!objects.emplace(volume,id).second)
                return false;
        }
    }
    return true;
}

Container ReadContainer(Source& source,uint64_t offset,uint64_t length)
{
    Check(Range(offset,length,source.Size())&&length>=4096,"Truncated APFS container");
    const auto prefix=Disk::Read(source,offset,48);
    const auto size=Le32(prefix.data()+36);
    Check(size>=4096&&size<=65536&&(size&(size-1))==0&&size<=length,"Invalid APFS block geometry");
    const auto initial=Disk::Read(source,offset,size);
    auto bootstrap=Superblock(initial,offset,length);
    Check(!(Le64(initial.data()+64)&~uint64_t(2)),"Unsupported APFS container features");
    const auto ring=ReadRing(initial.data(),bootstrap.blockCount,true);
    if(!ring.capacity)return bootstrap;
    std::optional<Container> newest;
    for(uint64_t i=0;i<ring.capacity;++i){
        const auto address=ring.base+i;
        const auto block=Disk::Read(source,offset+address*size,size);
        const auto p=block.data();
        if(memcmp(p+MagicOffset,"NXSB",4)||Le64(p+16)<bootstrap.transaction||
           (newest&&Le64(p+16)<=newest->transaction))
            continue;
        Container candidate;
        Ring desc{},data{};
        try{
            candidate=Superblock(block,offset,length);
            if(candidate.uuid!=bootstrap.uuid)continue;
            desc=ReadRing(p,candidate.blockCount,true);
            data=ReadRing(p,candidate.blockCount,false);
        }catch(const Error&){continue;}
        if(desc.base!=ring.base||desc.capacity!=ring.capacity||desc.index>=desc.capacity||
           desc.used<2||desc.used>desc.capacity||Slot(desc,uint64_t(desc.index)+desc.used-1)!=address||
           !data.capacity||data.index>=data.capacity||data.used>data.capacity||
           !(desc.base+desc.capacity<=data.base||data.base+data.capacity<=desc.base))
            continue;
        if(!ValidateMappings(source,candidate,desc,data))continue;
        Check(!(Le64(p+64)&~uint64_t(2)),"Unsupported APFS checkpoint features");
        candidate.checkpointBlock=address;
        newest=std::move(candidate);
    }
    Check(newest.has_value(),"No complete APFS checkpoint");
    return std::move(*newest);
}

void Store(Bytes& bytes,size_t at,uint64_t value,size_t width)
{
    for(size_t i=0;i<width;++i)bytes[at+i]=uint8_t(value>>(8*i));
}
void Stamp(Bytes& bytes,uint64_t transaction)
{
    Store(bytes,16,transaction,8);
    Store(bytes,0,ObjectChecksum(bytes),8);
}
uint32_t NextStart(const Ring& ring,uint32_t next)
{
    Check(ring.used&&ring.used<=ring.capacity/2&&next==(uint64_t(ring.index)+ring.used)%ring.capacity,
          "APFS checkpoint ring cannot hold the next transaction without overwriting the current one");
    return next;
}
struct CheckpointObject {
    uint32_t index;
    Bytes bytes;
};
struct PendingCheckpoint {
    uint32_t blockSize;
    uint64_t transaction;
    Ring descriptors,data;
    Bytes superblock;
    std::vector<Bytes> mappings;
    std::vector<CheckpointObject> objects;
};
PendingCheckpoint PlanCheckpoint(Source& source,const Container& current)
{
    const auto size=current.blockSize;
    auto superblock=Disk::Read(source,current.checkpointBlock*size,size);
    const auto p=superblock.data();
    Check(Le32(p+24)==0x80000001&&Le64(p+64)==2,"Unsupported APFS checkpoint write layout");
    const auto descriptors=ReadRing(p,current.blockCount,true);
    const auto data=ReadRing(p,current.blockCount,false);
    const auto descStart=NextStart(descriptors,Le32(p+128)),dataStart=NextStart(data,Le32(p+132));
    const auto transaction=Le64(p+96);
    Check(transaction>current.transaction&&transaction<UINT64_MAX,"Invalid next APFS transaction ID");

    PendingCheckpoint plan{size,transaction,{descriptors.base,descriptors.capacity,descStart,descriptors.used},
        {data.base,data.capacity,dataStart,data.used},std::move(superblock),{},{}};
    std::set<uint32_t> occupied;
    std::set<uint64_t> objectIds;
    for(uint32_t i=0;i+1<descriptors.used;++i){
        const auto original=Slot(descriptors,uint64_t(descriptors.index)+i);
        auto map=Disk::Read(source,original*size,size);
        Check(ValidMap(map,current.transaction,i+2==descriptors.used)&&Le64(map.data()+8)==original&&
              Le32(map.data()+24)==0x4000000c&&Le32(map.data()+28)==0,
              "Unsupported APFS checkpoint map header");
        const auto count=Le32(map.data()+36);
        for(uint32_t j=0;j<count;++j){
            const auto at=MapHeader+j*MappingBytes;
            const auto entry=map.data()+at;
            const auto type=Le32(entry),subtype=Le32(entry+4),bytes=Le32(entry+8);
            const auto volume=Le64(entry+16),oid=Le64(entry+24),block=Le64(entry+32);
            Check((type&0xf0000000)==0x80000000&&(!volume||std::find(current.volumes.begin(),current.volumes.end(),volume)!=current.volumes.end()),
                  "Unsupported APFS checkpoint object type");
            const auto blocks=bytes/size;
            const auto relative=DataIndex(data,block,blocks);
            Check(relative&&bytes%size==0&&oid,"APFS checkpoint object lies outside the active data range");
            Check(objectIds.insert(oid).second,"Repeated APFS checkpoint object ID");
            Bytes object(bytes);
            for(uint32_t n=0;n<blocks;++n){
                Check(occupied.insert(*relative+n).second,"Overlapping APFS checkpoint objects");
                const auto sourceBlock=Slot(data,uint64_t(data.index)+*relative+n);
                source.Read(sourceBlock*size,object.data()+size_t(n)*size,size);
            }
            Check(Le64(object.data())==ObjectChecksum(object)&&Le64(object.data()+8)==oid&&
                  Le64(object.data()+16)==current.transaction&&Le32(object.data()+24)==type&&
                  Le32(object.data()+28)==subtype,"Invalid APFS checkpoint object");
            plan.objects.push_back({*relative,std::move(object)});
            Store(map,at+32,Slot(data,uint64_t(dataStart)+*relative),8);
        }
        Store(map,8,Slot(descriptors,uint64_t(descStart)+i),8);
        Stamp(map,transaction);
        plan.mappings.push_back(std::move(map));
    }
    Check(occupied.size()==data.used,"Unmapped blocks in the APFS checkpoint data range");
    Store(plan.superblock,96,transaction+1,8);
    Store(plan.superblock,128,(uint64_t(descStart)+descriptors.used)%descriptors.capacity,4);
    Store(plan.superblock,132,(uint64_t(dataStart)+data.used)%data.capacity,4);
    Store(plan.superblock,136,descStart,4);
    Store(plan.superblock,144,dataStart,4);
    return plan;
}

Bytes& CheckpointObjectById(PendingCheckpoint& plan,uint64_t oid)
{
    const auto found=std::find_if(plan.objects.begin(),plan.objects.end(),[&](const auto& entry){
        return Le64(entry.bytes.data()+8)==oid;
    });
    Check(found!=plan.objects.end(),"Missing APFS checkpoint object");
    return found->bytes;
}
}

uint64_t ObjectChecksum(const HfsPlus::Bytes& object)
{
    // APFS stores the two Fletcher check words before the data they cover.
    constexpr uint64_t modulus=UINT32_MAX;
    Check(object.size()>=8&&object.size()%4==0,"Invalid APFS object checksum range");
    uint64_t first=0,second=0;
    for(size_t i=8;i<object.size();i+=4){
        first=(first+Le32(object.data()+i))%modulus;
        second=(second+first)%modulus;
    }
    const auto low=modulus-(first+second)%modulus;
    const auto high=modulus-(first+low)%modulus;
    return low|(high<<32);
}

std::optional<Container> Probe(Source& source)
{
    if(source.Size()<512)return {};
    const auto boot=Disk::Read(source,0,512);
    using namespace DiskPartitions;
    if(auto partitions=ProbeGptImage(source,boot,GptType::AppleApfs)){
        if(partitions->empty())return {};
        const auto& part=partitions->front();
        return ReadContainer(source,part.offset,part.length);
    }
    if(memcmp(boot.data()+MagicOffset,"NXSB",4))return {};
    return ReadContainer(source,0,source.Size());
}

namespace {
uint64_t Commit(Source& source,const Container& expected,Storage::Job& job,const CatalogMutation* mutation)
{
    if(job.cancelled())throw Storage::Cancelled();
    Check(source.Writable(),"An exclusive writable APFS image is required");
    source.CheckMedia();
    const auto current=Probe(source);
    Check(current&&current->uuid==expected.uuid&&current->transaction==expected.transaction&&
          current->checkpointBlock==expected.checkpointBlock,"The APFS checkpoint changed");
    Check(!current->offset&&current->checkpointBlock&&!current->keybagBlock&&!current->keybagBlocks&&
          (current->volumes.size()==1||(mutation&&expected.writeVolume))&&
          source.Size()==current->blockCount*current->blockSize,
          "Checkpoint writes require a selected APFS volume and a contiguous checkpoint ring");
    auto plan=PlanCheckpoint(source,*current);
    CatalogWritePlan changes;
    if(mutation){
        const auto object=[&](uint64_t oid) -> Bytes& {return CheckpointObjectById(plan,oid);};
        changes=PrepareCatalogMutation(source,*current,plan.transaction,plan.superblock,object,*mutation,expected.writeVolume);
    }
    Stamp(plan.superblock,plan.transaction);
    auto write=[&](uint64_t block,const uint8_t* bytes){
        if(job.cancelled())throw Storage::Cancelled();
        source.Write(block*plan.blockSize,bytes,plan.blockSize);
    };
    source.CheckMedia();
    for(const auto& stream:changes.streams){
        auto& data=stream.input;
        data.CheckMedia();
        Bytes buffer(plan.blockSize);
        uint64_t offset=0;
        for(const auto& run:stream.runs){
            for(uint64_t i=0;i<run.count;++i){
                if(job.cancelled())throw Storage::Cancelled();
                const auto size=size_t((std::min)(uint64_t(plan.blockSize),stream.size-offset));
                std::fill(buffer.begin()+size,buffer.end(),0);
                data.Read(offset,buffer.data(),size);
                write(run.block+i,buffer.data());
                offset+=size;
            }
        }
        Check(offset==stream.size,"APFS data allocation did not cover the file");
        data.CheckMedia();
        Check(data.Size()==stream.size,"The APFS file source changed size");
    }
    for(const auto& block:changes.metadata)write(block.block,block.bytes.data());
    for(auto& object:plan.objects){
        Stamp(object.bytes,plan.transaction);
        for(size_t i=0;i<object.bytes.size()/plan.blockSize;++i){
            const auto block=Slot(plan.data,uint64_t(plan.data.index)+object.index+i);
            write(block,object.bytes.data()+i*plan.blockSize);
        }
    }
    for(size_t i=0;i<plan.mappings.size();++i)
        write(Slot(plan.descriptors,uint64_t(plan.descriptors.index)+i),plan.mappings[i].data());

    // The superblock is the commit record. Until this write, both rings leave
    // the selected checkpoint intact and no reader can observe the new XID.
    source.Flush();
    source.CheckMedia();
    if(job.cancelled())throw Storage::Cancelled();
    const auto commit=Slot(plan.descriptors,uint64_t(plan.descriptors.index)+plan.mappings.size());
    try{
        source.Write(commit*plan.blockSize,plan.superblock.data(),plan.blockSize);
        source.Flush();
    }catch(const std::exception& error){
        throw CheckpointPublicationUncertain(std::string("APFS checkpoint publication may have completed: ")+error.what());
    }
    return plan.transaction;
}
}

uint64_t InspectAllocationFreeBytes(Source& source,const Container& container)
{
    auto checkpoint=PlanCheckpoint(source,container);
    const auto& space=CheckpointObjectById(checkpoint,Le64(checkpoint.superblock.data()+152));
    return VerifyAllocationBitmaps(source,container,space);
}

uint64_t InspectFreeQueueAllocation(Source& source,const Container& container,uint64_t selected)
{
    auto checkpoint=PlanCheckpoint(source,container);
    const auto object=[&](uint64_t oid)->Bytes& {return CheckpointObjectById(checkpoint,oid);};
    return VerifyFreeQueueAllocation(source,container,checkpoint.transaction,checkpoint.superblock,object,selected);
}

uint64_t WritableFreeBytes(Source& source,const Container& container)
{
    auto checkpoint=PlanCheckpoint(source,container);
    const auto& space=CheckpointObjectById(checkpoint,Le64(checkpoint.superblock.data()+152));
    Check(space.size()==container.blockSize&&Le32(space.data()+24)==0x80000005&&
          Le32(space.data()+32)==container.blockSize&&Le64(space.data()+48)==container.blockCount&&
          !Le32(space.data()+68)&&!Le64(space.data()+96)&&Le32(space.data()+160)==1,
          "Unsupported APFS space-manager layout");
    const auto free=Le64(space.data()+72);
    Check(free<=container.blockCount,"Invalid APFS free-space count");
    return free*container.blockSize;
}

uint64_t CommitCheckpoint(Source& source,const Container& expected,Storage::Job& job)
{
    return Commit(source,expected,job,nullptr);
}

uint64_t CreateCatalogDirectory(Source& source,const Container& expected,uint64_t parent,const std::u16string& name,
                                Storage::Job& job,std::optional<FileDates> dates,const FinderBytes* finder,const HfsPlus::Attributes* attributes,bool external)
{
    CatalogCreate create{parent,name,CatalogKind::Directory,nullptr,dates,finder,false,attributes};
    create.external=external;
    const CatalogMutation request=create;
    return Commit(source,expected,job,&request);
}

uint64_t SetDirectoryDates(Source& source,const Container& expected,uint64_t parent,uint64_t inode,
                           FileDates dates,Storage::Job& job)
{
    const CatalogMutation request=CatalogMetadata{parent,inode,{dates.created,dates.modified}};
    return Commit(source,expected,job,&request);
}

uint64_t SetExternalCatalogProperties(Source& source,const Container& expected,uint64_t parent,
                                  const std::u16string& name,TimestampEdits dates,Storage::Job& job,
                                  std::optional<bool> locked,const FinderBytes* finder)
{
    Check(parent>=2&&parent<(uint64_t(1)<<60)&&(dates.created||dates.modified||locked.has_value()||finder),
          "Select an APFS folder and at least one property to edit");
    const CatalogMutation request=ExternalCatalogEdit{parent,name,dates,locked,nullptr,finder};
    return Commit(source,expected,job,&request);
}

uint64_t RenameExternalCatalogEntry(Source& source,const Container& expected,uint64_t parent,
                                   const std::u16string& name,const std::u16string& newName,Storage::Job& job,uint64_t destinationParent)
{
    Check(parent>=2&&parent<(uint64_t(1)<<60),"Select an APFS folder");
    const CatalogMutation request=ExternalCatalogEdit{parent,name,{},{},&newName,nullptr,destinationParent};
    return Commit(source,expected,job,&request);
}

uint64_t SetEntryProperties(Source& source,const Container& expected,uint64_t parent,uint64_t inode,
                       const FinderBytes& finder,TimestampEdits dates,bool locked,Storage::Job& job)
{
    const CatalogMutation request=CatalogMetadata{parent,inode,dates,locked,&finder};
    return Commit(source,expected,job,&request);
}

uint64_t SetResourceFork(Source& source,const Container& expected,uint64_t parent,uint64_t inode,
                                 Source& resource,uint64_t modified,Storage::Job& job,bool external)
{
    const CatalogMutation request=CatalogResourceFork{parent,inode,resource,modified,external};
    return Commit(source,expected,job,&request);
}

uint64_t SetCatalogAttributes(Source& source,const Container& expected,uint64_t parent,uint64_t inode,
                               const HfsPlus::Attributes& attributes,Storage::Job& job,bool external)
{
    const CatalogMutation request=CatalogAttributes{parent,inode,attributes,external};
    return Commit(source,expected,job,&request);
}

uint64_t CreateCatalogSymlink(Source& source,const Container& expected,uint64_t parent,const std::u16string& name,
                              const HfsPlus::Bytes& target,Storage::Job& job,std::optional<FileDates> dates,
                              const FinderBytes& finder,bool locked,const HfsPlus::Attributes& attributes,bool external,
                              uint64_t replacedEntry)
{
    Check(!replacedEntry||external,"APFS symbolic-link replacement requires the external catalog writer");
    const CatalogMutation request=CatalogCreate{parent,name,CatalogKind::SymbolicLink,nullptr,dates,&finder,
                                                locked,&attributes,nullptr,nullptr,&target,external,replacedEntry};
    return Commit(source,expected,job,&request);
}

uint64_t ReplaceCatalogSymlink(Source& source,const Container& expected,uint64_t parent,uint64_t inode,
                               const HfsPlus::Bytes& target,Storage::Job& job,std::optional<FileDates> dates,
                               const FinderBytes& finder,bool locked,const HfsPlus::Attributes& attributes,bool external)
{
    const CatalogMutation request=CatalogReplaceSymlink{parent,inode,target,dates,finder,locked,attributes,external};
    return Commit(source,expected,job,&request);
}

uint64_t CreateCatalogHardLink(Source& source,const Container& expected,uint64_t parent,uint64_t inode,
                               const std::u16string& name,Storage::Job& job,uint64_t replaced,bool external,bool copiedLockedInode)
{
    const CatalogMutation request=CatalogHardLink{parent,inode,name,replaced,external,copiedLockedInode};
    return Commit(source,expected,job,&request);
}

uint64_t CreateEmptyFile(Source& source,const Container& expected,uint64_t parent,const std::u16string& name,Storage::Job& job)
{
    const CatalogMutation request=CatalogCreate{parent,name,CatalogKind::RegularFile};
    return Commit(source,expected,job,&request);
}

uint64_t CreateCatalogFile(Source& source,const Container& expected,uint64_t parent,const std::u16string& name,
                    Source& data,Storage::Job& job,std::optional<FileDates> dates,const FinderBytes* finder,bool locked,const HfsPlus::Attributes* attributes,Source* resource,const HfsPlus::Bytes* compression,bool external,uint64_t replacedEntry)
{
    Check(&source!=&data,"APFS file data requires a separate source");
    Check(!replacedEntry||external,"APFS entry replacement requires the external catalog writer");
    CatalogCreate create{parent,name,CatalogKind::RegularFile,&data,dates,finder,locked,attributes,resource,compression};
    create.external=external;
    create.replacedEntry=replacedEntry;
    const CatalogMutation request=create;
    return Commit(source,expected,job,&request);
}

uint64_t RenameEntry(Source& source,const Container& expected,uint64_t parent,uint64_t inode,
                     const std::u16string& name,Storage::Job& job,uint64_t alias)
{
    const CatalogMutation request=CatalogRename{parent,inode,name,alias};
    return Commit(source,expected,job,&request);
}

uint64_t DeleteEntry(Source& source,const Container& expected,uint64_t parent,uint64_t inode,Storage::Job& job,uint64_t alias,bool external,bool recursive)
{
    const CatalogMutation request=CatalogDelete{parent,inode,alias,external,recursive};
    return Commit(source,expected,job,&request);
}

uint64_t ReplaceFileData(Source& source,const Container& expected,uint64_t parent,uint64_t inode,
                         Source& data,Storage::Job& job,std::optional<FileDates> dates,const FinderBytes* finder,bool locked,const HfsPlus::Attributes* attributes,Source* resource,const HfsPlus::Bytes* compression,bool external,uint64_t alias,bool preserveDates)
{
    Check(&source!=&data,"APFS file data requires a separate source");
    const CatalogMutation request=CatalogReplaceData{parent,inode,data,dates,finder,locked,attributes,resource,compression,external,alias,preserveDates};
    return Commit(source,expected,job,&request);
}
}
