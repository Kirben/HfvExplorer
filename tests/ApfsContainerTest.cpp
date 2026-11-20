// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "ApfsContainer.h"
#include "ApfsFormat.h"
#include "ApfsTree.h"
#include "ApfsWriteDisk.h"
#include "HfsPlusDisk.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include "CopyProcess.h"
#include "FileOperations.h"
#include "DmgImage.h"
#include "DmgEncryption.h"
#include "ApfsVolume.h"
#include "WorkerEndpoint.h"
#include <algorithm>
#include <cstdio>

namespace {
using namespace HfsPlus;
using Disk::Check;
class Memory final : public Source {
public:
    Bytes bytes;
    explicit Memory(size_t size):bytes(size){}
    uint64_t Size()const override{return bytes.size();}
    bool Writable()const override{return false;}
    void Flush()override{}
    void Write(uint64_t,const void*,size_t)override{throw Error("The APFS probe attempted a write");}
    void Read(uint64_t at,void* dest,size_t size)override{
        Check(Disk::Range(at,size,bytes.size()),"Read outside APFS test image");
        memcpy(dest,bytes.data()+at,size);
    }
};
void Little(uint8_t* p,uint64_t value,unsigned bytes)
{
    for(unsigned i=0;i<bytes;++i)p[i]=uint8_t(value>>(i*8));
}
uint32_t Crc(const Bytes& bytes)
{
    uint32_t crc=~0u;
    for(auto b:bytes){
        crc^=b;
        for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&uint32_t(-int(crc&1)));
    }
    return ~crc;
}
template<class F>void Reject(F work,const char* detail)
{
    try{work();}catch(const Error&){return;}
    throw Error(detail);
}
void Seal(Bytes& disk,size_t offset,size_t blockSize)
{
    const auto first=disk.begin()+offset;
    Bytes block(first,first+blockSize);
    Little(disk.data()+offset,Apfs::ObjectChecksum(block),8);
}

class CheckpointFaultImage final:public Source {
    Bytes live_;
public:
    enum class Fault {None, BeforeWrite, TornWrite, FailedFlush};
    Bytes durable;
    unsigned event=0,failAt=0;
    Fault fault=Fault::None;
    bool persistBeforeFailedFlush=false;
    std::function<void()> onFlush;
    explicit CheckpointFaultImage(const Bytes& base):live_(base),durable(base){}
    uint64_t Size()const override{return live_.size();}
    bool Writable()const override{return true;}
    void Read(uint64_t at,void* dest,size_t size)override{
        Check(Disk::Range(at,size,live_.size()),"Checkpoint test read exceeded the image");
        memcpy(dest,live_.data()+at,size);
    }
    void Write(uint64_t at,const void* data,size_t size)override{
        Check(Disk::Range(at,size,live_.size()),"Checkpoint test write exceeded the image");
        if(++event==failAt&&fault==Fault::BeforeWrite)throw Error("Injected checkpoint write failure");
        if(event==failAt&&fault==Fault::TornWrite){
            memcpy(live_.data()+at,data,23);
            memcpy(durable.data()+at,data,23);
            throw Error("Injected torn checkpoint write");
        }
        memcpy(live_.data()+at,data,size);
    }
    void Flush()override{
        if(++event==failAt&&fault==Fault::FailedFlush){
            if(persistBeforeFailedFlush)durable=live_;
            throw Error("Injected checkpoint flush failure");
        }
        durable=live_;
        if(onFlush)onFlush();
    }
};

void CheckpointWrites(const std::string& root,FILE* report)
{
    constexpr size_t imageSize=8*1024*1024;
    const auto baselinePath=VolumeCoord::Wide(root+"checkpoint-before.img");
    {
        auto image=CreateFormatOutput(baselinePath.c_str(),imageSize);
        Storage::Job job;
        Apfs::Format(*image,u"Checkpoint fixture",false,job);
        image->Keep();
    }
    Bytes original;
    {
        auto image=HfsPlus::OpenImage(baselinePath.c_str(),false);
        original=Disk::Read(*image,0,imageSize);
        Storage::Job job;
        const auto checkpoint=Apfs::Probe(*image);
        Reject([&]{Apfs::CommitCheckpoint(*image,*checkpoint,job);},"Read-only APFS source accepted a checkpoint");
    }
    const auto reopen=[](const CheckpointFaultImage& source){
        Memory disk(source.durable.size());
        disk.bytes=source.durable;
        const auto checkpoint=Apfs::Probe(disk);
        Check(checkpoint.has_value(),"No APFS checkpoint survived");
        return checkpoint->transaction;
    };
    CheckpointFaultImage control(original);
    const auto first=Apfs::Probe(control);
    Check(first&&first->transaction==1,"APFS formatter did not create the initial checkpoint");
    Storage::Job controlJob;
    Check(Apfs::CommitCheckpoint(control,*first,controlJob)==2&&reopen(control)==2,
          "New APFS checkpoint was not durable and readable");
    const auto events=control.event;
    Check(events>=4,"Too few checkpoint publication steps to test");
    Reject([&]{Apfs::CommitCheckpoint(control,*first,controlJob);},"Stale checkpoint was accepted");
    using Fault=CheckpointFaultImage::Fault;
    const auto interrupt=[&](unsigned step,Fault fault,bool persisted){
        CheckpointFaultImage attempt(original);
        attempt.failAt=step;
        attempt.fault=fault;
        attempt.persistBeforeFailedFlush=persisted;
        Storage::Job job;
        bool failed=false,uncertain=false;
        try{Apfs::CommitCheckpoint(attempt,*first,job);}
        catch(const Apfs::CheckpointPublicationUncertain&){failed=true;uncertain=true;}
        catch(const Error&){failed=true;}
        Check(failed&&uncertain==(step>=events-1)&&reopen(attempt)==(persisted?2u:1u),
              "Interrupted APFS publication lost the expected checkpoint or misreported the outcome");
    };
    for(unsigned step=1;step<=events;++step){
        if(step==events||step==events-2)interrupt(step,Fault::FailedFlush,false);
        else{
            interrupt(step,Fault::BeforeWrite,false);
            interrupt(step,Fault::TornWrite,false);
        }
    }
    interrupt(events,Fault::FailedFlush,true);

    auto invalid=original;
    // Two distinct object IDs in the map point to the same physical block.
    memcpy(invalid.data()+4096+112,invalid.data()+4096+72,8);
    Seal(invalid,4096,4096);
    CheckpointFaultImage overlapping(invalid);
    Reject([&]{Apfs::CommitCheckpoint(overlapping,*first,controlJob);},
           "Overlapping checkpoint mappings were accepted");
    Check(!overlapping.event&&overlapping.durable==invalid,"Invalid checkpoint did not fail before writing");
    {
        CheckpointFaultImage attempt(original);
        Storage::Job job;
        attempt.onFlush=[&]{job.cancel();};
        bool cancelled=false;
        try{Apfs::CommitCheckpoint(attempt,*first,job);}catch(const Storage::Cancelled&){cancelled=true;}
        Check(cancelled&&reopen(attempt)==1,"Cancellation after the data flush published an APFS checkpoint");
    }

    for(uint64_t tx=3;tx<=19;++tx){
        const auto checkpoint=Apfs::Probe(control);
        Check(Apfs::CommitCheckpoint(control,*checkpoint,controlJob)==tx&&reopen(control)==tx,
              "Repeated checkpoint publication failed at a ring boundary");
    }
    // Write the result of normal file-backed commits for an independent checker.
    const auto afterPath=VolumeCoord::Wide(root+"checkpoint-after.img");
    {
        auto image=CreateFormatOutput(afterPath.c_str(),imageSize);
        image->Write(0,original.data(),original.size());
        for(uint64_t tx=2;tx<=19;++tx){
            const auto checkpoint=Apfs::Probe(*image);
            Check(Apfs::CommitCheckpoint(*image,*checkpoint,controlJob)==tx,"File-backed APFS checkpoint commit failed");
        }
        image->Keep();
    }
    const auto volume=MacBrowser::OpenBrowserImage(afterPath);
    Check(volume&&volume->Name()==u"Checkpoint fixture"&&volume->List().empty(),
          "The browser could not reopen a checkpointed image");
    fprintf(report,"PASS: APFS checkpoint publication, previous-state recovery at every write/flush boundary, cancellation, stale detection, and descriptor/data ring wrap.\n");

    CheckpointFaultImage created(original);
    const auto start=Apfs::Probe(created);
    Check(Apfs::CreateRootDirectory(created,*start,u"New Folder",controlJob)==2&&reopen(created)==2,
          "APFS catalog transaction did not publish its checkpoint");
    for(unsigned step=1;step<=created.event;++step){
        CheckpointFaultImage attempt(original);
        attempt.failAt=step;
        attempt.fault=step==created.event||step==created.event-2?Fault::FailedFlush:Fault::TornWrite;
        Reject([&]{Apfs::CreateRootDirectory(attempt,*start,u"New Folder",controlJob);},
               "Injected APFS catalog interruption succeeded");
        Check(reopen(attempt)==1,"Interrupted APFS directory creation published a new checkpoint");
        auto memory=std::make_unique<Memory>(attempt.durable.size());
        memory->bytes=attempt.durable;
        std::unique_ptr<Source> source=std::move(memory);
        const auto old=Apfs::Open(source,"apfs-checkpoint-fault");
        Check(old&&old->List().empty()&&old->Name()==u"Checkpoint fixture",
              "Interrupted APFS directory creation made the previous catalog unreadable");
    }
    const auto output=VolumeCoord::Wide(root+"checkpoint-directory.img");
    {
        auto image=CreateFormatOutput(output.c_str(),imageSize);
        image->Write(0,created.durable.data(),created.durable.size());
        image->Keep();
    }
    auto changed=MacBrowser::OpenBrowserImage(output);
    MacBrowser::Entry folder;
    Check(changed&&changed->List().size()==1&&changed->FindChild(2,u"new folder",folder)&&
          folder.name==u"New Folder"&&folder.directory&&changed->List(folder.id).empty(),
          "New APFS directory was not found on reopen");
    const auto state=Apfs::Probe(created);
    const auto eventsAfter=created.event;
    Reject([&]{Apfs::CreateRootDirectory(created,*start,u"Stale",controlJob);},
           "A stale catalog mutation was accepted");
    Reject([&]{Apfs::CreateRootDirectory(created,*state,u"new folder",controlJob);},
           "A case-insensitive APFS name collision was accepted");
    Check(created.event==eventsAfter,"Rejected APFS catalog write modified the image");

    const auto listFolders=[](const CheckpointFaultImage& disk){
        auto memory=std::make_unique<Memory>(disk.durable.size());
        memory->bytes=disk.durable;
        std::unique_ptr<Source> source=std::move(memory);
        const auto mounted=Apfs::Open(source,"apfs-reclaim-test");
        Check(mounted!=nullptr,"Reclaimed APFS image could not be opened");
        std::vector<std::u16string> names;
        for(const auto& entry:mounted->List()){
            Check(entry.directory&&mounted->List(entry.id).empty(),"Unexpected APFS directory contents");
            names.push_back(entry.name);
        }
        std::sort(names.begin(),names.end());
        return names;
    };
    CheckpointFaultImage reused(created.durable);
    Check(Apfs::CreateRootDirectory(reused,*state,u"Another",controlJob)==3&&reopen(reused)==3,
          "APFS catalog write could not reuse retired blocks");
    const auto oldMap=first->objectMap*4096;
    Check(!std::equal(original.begin()+oldMap,original.begin()+oldMap+4096,reused.durable.begin()+oldMap),
          "APFS catalog write did not reuse the initial checkpoint's object map block");
    for(unsigned step=1;step<=reused.event;++step){
        CheckpointFaultImage attempt(created.durable);
        attempt.failAt=step;
        attempt.fault=step==reused.event||step==reused.event-2?Fault::FailedFlush:Fault::TornWrite;
        Reject([&]{Apfs::CreateRootDirectory(attempt,*state,u"Another",controlJob);},
               "Injected APFS reuse interruption succeeded");
        Check(reopen(attempt)==2&&listFolders(attempt)==std::vector<std::u16string>{u"New Folder"},
              "Interrupted APFS block reuse damaged the preceding catalog");
    }
    // A validly encoded free queue must still never reclaim live metadata.
    auto badQueue=created.durable;
    const auto nx=badQueue.data()+state->checkpointBlock*4096;
    const auto descriptor=Apfs::Le64(nx+112)+Apfs::Le32(nx+136);
    const auto queueCheckpointMap=badQueue.data()+descriptor*4096;
    const auto spaceman=Apfs::Le64(queueCheckpointMap+112);
    auto queuedSpace=badQueue.data()+spaceman*4096;
    const auto queueOid=Apfs::Le64(queuedSpace+248);
    uint64_t queueBlock=0;
    for(uint32_t i=0;i<Apfs::Le32(queueCheckpointMap+36);++i){
        const auto entry=queueCheckpointMap+40+40*i;
        if(Apfs::Le64(entry+24)==queueOid)queueBlock=Apfs::Le64(entry+32);
    }
    Check(queueBlock,"Cannot locate the APFS free queue in the test fixture");
    Apfs::WriterDisk::Record retired{Bytes(16),{}};
    Little(retired.key.data(),state->transaction,8);
    Little(retired.key.data()+8,state->objectMap,8);
    auto queue=Apfs::WriterDisk::EncodeLeaf({retired},0x0c,16,8);
    memcpy(queue.data()+8,badQueue.data()+queueBlock*4096+8,24);
    memcpy(badQueue.data()+queueBlock*4096,queue.data(),queue.size());
    Seal(badQueue,queueBlock*4096,4096);
    Little(queuedSpace+240,1,8);
    Seal(badQueue,spaceman*4096,4096);
    CheckpointFaultImage liveBlockQueued(badQueue);
    Check(reopen(liveBlockQueued)==2,"Corrupt free-queue fixture did not preserve the selected checkpoint");
    Reject([&]{Apfs::CreateRootDirectory(liveBlockQueued,*state,u"Another",controlJob);},
           "APFS reclamation accepted a retired range overlapping live metadata");
    Check(!liveBlockQueued.event&&liveBlockQueued.durable==badQueue,"APFS reclamation wrote invalid allocation data");

    std::vector<std::u16string> expected{u"Another",u"New Folder"};
    Check(listFolders(reused)==expected,"APFS block reuse lost a previously created directory");
    for(uint64_t tx=4;tx<=13;++tx){
        std::u16string folderName=u"Folder ";
        folderName+=char16_t(u'A'+tx-4);
        const auto checkpoint=Apfs::Probe(reused);
        Check(checkpoint&&Apfs::CreateRootDirectory(reused,*checkpoint,folderName,controlJob)==tx&&reopen(reused)==tx,
              "Repeated APFS directory creation exhausted the allocator");
        expected.push_back(folderName);
    }
    std::sort(expected.begin(),expected.end());
    Check(listFolders(reused)==expected,"Repeated APFS block reclamation lost a directory");
    {
        const auto path=VolumeCoord::Wide(root+"checkpoint-reclaimed.img");
        auto image=CreateFormatOutput(path.c_str(),imageSize);
        image->Write(0,reused.durable.data(),reused.durable.size());
        image->Keep();
    }
    CheckpointFaultImage invalidName(original);
    Reject([&]{Apfs::CreateRootDirectory(invalidName,*start,u"Bad/Name",controlJob);},
           "An illegal APFS directory name was accepted");
    Check(!invalidName.event&&invalidName.durable==original,"Invalid APFS name modified the image");

    auto badBitmap=original;
    const auto map=badBitmap.data()+4096;
    const auto spaceBlock=Apfs::Le64(map+112);
    const auto space=badBitmap.data()+spaceBlock*4096;
    const auto cib=Apfs::Le64(space+Apfs::Le32(space+80));
    const auto bitmap=Apfs::Le64(badBitmap.data()+cib*4096+64);
    // Mark the existing container object map free in a damaged allocation bitmap.
    badBitmap[bitmap*4096+start->objectMap/8]&=uint8_t(~(1u<<(start->objectMap%8)));
    CheckpointFaultImage wrongAllocation(badBitmap);
    Reject([&]{Apfs::CreateRootDirectory(wrongAllocation,*start,u"Must not exist",controlJob);},
           "The APFS allocator trusted a free bit for a live metadata block");
    Check(!wrongAllocation.event&&wrongAllocation.durable==badBitmap,"Invalid APFS allocation wrote to the image");
    fprintf(report,"PASS: APFS allocation, repeated directory creation, retired block reuse, copy-on-write maps, and interrupted commits.\n");
}

Memory Raw(unsigned blockSize=4096)
{
    Memory disk(1024*1024);
    auto p=disk.bytes.data();
    Little(p+8,1,8);Little(p+16,5,8);Little(p+24,0x80000001,4);
    memcpy(p+32,"NXSB",4);
    Little(p+36,blockSize,4);Little(p+40,disk.Size()/blockSize,8);
    Little(p+64,2,8);p[72]=42;
    Little(p+160,12,8);Little(p+180,100,4);
    Little(p+184,1024,8);Little(p+200,1029,8);
    Seal(disk.bytes,0,blockSize);
    return disk;
}
Memory WithCheckpoints()
{
    constexpr size_t size=4096;
    auto disk=Raw();
    auto bytes=disk.bytes.data();
    Little(bytes+104,8,4);Little(bytes+108,8,4);
    Little(bytes+112,1,8);Little(bytes+120,20,8);
    Seal(disk.bytes,0,size);
    const auto checkpoint=[&](unsigned mapSlot,unsigned superSlot,uint64_t transaction){
        auto map=bytes+(mapSlot+1)*size;
        Little(map+8,mapSlot+1,8);Little(map+16,transaction,8);Little(map+24,0x4000000c,4);
        Little(map+32,1,4);Little(map+36,1,4);
        Little(map+48,size,4);Little(map+64,800,8);Little(map+72,20,8);
        Seal(disk.bytes,(mapSlot+1)*size,size);
        auto super=bytes+(superSlot+1)*size;
        memcpy(super,bytes,size);
        Little(super+16,transaction,8);Little(super+136,mapSlot,4);Little(super+140,2,4);
        Little(super+148,1,4);
        Seal(disk.bytes,(superSlot+1)*size,size);
    };
    checkpoint(3,4,6);
    checkpoint(7,0,7);
    return disk;
}


void Header(uint8_t* p,uint64_t oid,uint64_t tx,uint32_t type,uint32_t subtype=0)
{
    Little(p+8,oid,8);Little(p+16,tx,8);Little(p+24,type,4);Little(p+28,subtype,4);
}
void ObjectMapFixture(Memory& disk)
{
    constexpr size_t size=4096;
    auto map=disk.bytes.data()+12*size;
    Header(map,12,5,0x4000000b);
    Little(map+40,0x40000002,4);Little(map+48,13,8);
    auto root=map+size;
    Header(root,13,5,0x40000002,11);
    Little(root+32,5,2);Little(root+34,1,2);Little(root+36,1,4);
    Little(root+42,4,2);Little(root+44,16,2);Little(root+60,1024,8);Little(root+68,1,8);
    const auto end=root+size-40;
    Little(root+58,8,2);Little(end-8,14,8);
    Little(end+4,size,4);Little(end+8,16,4);Little(end+12,16,4);
    auto leaf=root+size;
    Header(leaf,14,5,0x40000003,11);
    Little(leaf+32,6,2);Little(leaf+36,5,4);Little(leaf+42,20,2);Little(leaf+44,80,2);
    for(unsigned i=0;i<5;++i){
        auto key=leaf+76+i*16;
        auto value=leaf+size-(i+1)*16;
        Little(leaf+56+i*4,i*16,2);Little(leaf+58+i*4,(i+1)*16,2);
        Little(key,i<3?1024:1029,8);Little(key+8,i<3?1+i*2:1+(i-3)*6,8);
        Little(value,i==2?1:0,4);Little(value+4,size,4);Little(value+8,35+i,8);
    }
    for(size_t block:{12,13,14})Seal(disk.bytes,block*size,size);
}

enum class Symlink { None, Valid, Malformed };
Memory BrowseFixture(bool attributes=false,bool links=false,bool compressed=false,Symlink symlink=Symlink::None)
{
    auto disk=Raw();
    constexpr size_t size=4096;
    ObjectMapFixture(disk);
    auto leaf=disk.bytes.data()+14*size;
    Little(leaf+size-48,0,4);
    Seal(disk.bytes,14*size,size);

    auto apsb=disk.bytes.data()+37*size;
    Header(apsb,1024,5,13);memcpy(apsb+32,"APSB",4);
    Little(apsb+56,1,8);Little(apsb+116,2,4);
    Little(apsb+128,24,8);Little(apsb+136,2048,8);Little(apsb+264,1,8);
    memcpy(apsb+704,"APFS Fixture",13);
    Seal(disk.bytes,37*size,size);

    auto map=disk.bytes.data()+24*size;
    Header(map,24,5,0x4000000b);
    Little(map+40,0x40000002,4);Little(map+48,25,8);
    auto mapRoot=map+size;
    Header(mapRoot,25,5,0x40000002,11);
    Little(mapRoot+32,7,2);Little(mapRoot+36,1,4);
    Little(mapRoot+42,4,2);Little(mapRoot+44,16,2);Little(mapRoot+60,2048,8);Little(mapRoot+68,5,8);
    auto end=mapRoot+size-40;
    Little(mapRoot+58,16,2);Little(end-12,size,4);Little(end-8,26,8);
    Little(end+4,size,4);Little(end+8,16,4);Little(end+12,16,4);
    Seal(disk.bytes,24*size,size);Seal(disk.bytes,25*size,size);

    auto catalog=mapRoot+size;
    Header(catalog,2048,5,2,14);
    const unsigned records=8+(attributes?3:0)+(links?5:0)+(compressed?1:0)+(symlink!=Symlink::None?1:0);
    Little(catalog+32,3,2);Little(catalog+36,records,4);Little(catalog+42,records*8,2);
    end=catalog+size-40;
    Little(end+4,size,4);
    size_t keyOffset=0,valueOffset=0;
    unsigned index=0;
    auto record=[&](uint64_t id,unsigned type,Bytes key,Bytes value){
        auto toc=catalog+56+index++*8;
        Little(toc,keyOffset,2);Little(toc+2,8+key.size(),2);
        auto start=catalog+56+records*8+keyOffset;
        Little(start,id|(uint64_t(type)<<60),8);
        if(!key.empty())memcpy(start+8,key.data(),key.size());
        keyOffset+=8+key.size();
        valueOffset+=value.size();
        Little(toc+4,valueOffset,2);Little(toc+6,value.size(),2);
        memcpy(end-valueOffset,value.data(),value.size());
    };
    auto inode=[&](uint64_t id,bool directory,uint64_t length,bool linked=false){
        Bytes value(directory?92:140);
        Little(value.data()+8,id,8);Little(value.data()+56,linked?2:1,4);
        Little(value.data()+80,directory?0040755:symlink!=Symlink::None?0120777:0100644,2);
        if(!directory){
            Little(value.data()+92,1,2);Little(value.data()+94,44,2);
            value[96]=8;Little(value.data()+98,40,2);
            Little(value.data()+100,length,8);
        }
        record(id,3,{},std::move(value));
    };
    auto drec=[&](uint64_t directory,uint64_t id,const char* name){
        const auto len=strlen(name)+1;
        Bytes key(4+len),value(18);
        Little(key.data(),len,4);memcpy(key.data()+4,name,len);
        Little(value.data(),id,8);
        record(directory,9,std::move(key),std::move(value));
    };
    auto extent=[&](uint64_t at,uint64_t block,uint64_t stream=330){
        Bytes key(8),value(24);
        Little(key.data(),at,8);
        Little(value.data(),size,8);Little(value.data()+8,block,8);
        record(stream,8,std::move(key),std::move(value));
    };
    inode(2,true,0);drec(2,320,"Folder");
    inode(320,true,0);drec(320,330,"Long \xc3\xa9.txt");
    if(links){drec(320,330,"Alias A");drec(320,340,"Alias B");drec(320,340,"Other");}
    inode(330,false,2*size+7,links);
    auto xattr=[&](const char* name,uint16_t flags,const Bytes& data){
        const auto length=strlen(name)+1;
        Bytes key(2+length),value(4+data.size());
        Little(key.data(),length,2);memcpy(key.data()+2,name,length);
        Little(value.data(),flags,2);Little(value.data()+2,data.size(),2);
        memcpy(value.data()+4,data.data(),data.size());
        record(330,4,std::move(key),std::move(value));
    };
    if(attributes){
        Bytes stream(48);
        Little(stream.data(),331,8);Little(stream.data()+8,8,8);Little(stream.data()+16,size,8);
        xattr("com.apple.ResourceFork",1,stream);
        xattr("user.example",2,Bytes{1,2,3});
    }
    if(symlink!=Symlink::None){
        Bytes target{'/','a','/','b','\n',0};
        if(symlink==Symlink::Malformed)target.back()='c';
        xattr("com.apple.fs.symlink",6,target);
    }
    if(compressed){
        Bytes data{102,112,109,99,3,0,0,0,0,0,0,0,0,0,0,0,120,156,237,193,49,1,0,0,0,194,160,112,235,223,201,18,158,64,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,189,13,21,40,148,189};
        Little(data.data()+8,70001,8);
        xattr("com.apple.decmpfs",2,data);
    }
    extent(0,40);extent(size,0);extent(2*size,42);
    if(attributes){
        extent(0,43,331);
        memcpy(disk.bytes.data()+43*size,"resource",8);
    }
    if(links){inode(340,false,1,true);extent(0,42,340);}
    Little(catalog+44,keyOffset,2);
    Seal(disk.bytes,26*size,size);
    memset(disk.bytes.data()+40*size,'A',size);
    memcpy(disk.bytes.data()+42*size,"last!!!",7);
    return disk;
}


Memory SecondVolume()
{
    auto disk=BrowseFixture();
    constexpr size_t size=4096;
    auto block=[&](size_t index){return disk.bytes.data()+index*size;};
    memcpy(block(38),block(37),size);
    Header(block(38),1029,5,13);
    Little(block(38)+56,8,8); // Case-sensitive with normalization-insensitive directory keys.
    Little(block(38)+128,27,8);
    memcpy(block(27),block(24),size);
    Header(block(27),27,5,0x4000000b);
    Little(block(27)+48,28,8);
    memcpy(block(28),block(25),size);
    Header(block(28),28,5,0x40000002,11);
    Little(block(28)+size-48,29,8);
    memcpy(block(29),block(26),size);
    const auto end=block(29)+size-40;
    const auto extent=end-Apfs::Le16(block(29)+56+5*8+4);
    Little(extent+8,44,8);
    block(44)[0]='B';
    for(size_t i:{27,28,29,38})Seal(disk.bytes,i*size,size);
    return disk;
}

Memory SnapshotFixture()
{
    auto disk=BrowseFixture();
    constexpr size_t size=4096;
    const auto block=[&](size_t n){return disk.bytes.data()+n*size;};
    auto live=block(37);
    auto frozen=block(39);
    memcpy(frozen,live,size);
    Header(frozen,39,4,0x4000000d);
    Little(frozen+128,0,8); // A frozen volume has no independent object map.
    Little(live+124,0x40000002,4);
    Little(live+152,32,8);
    Little(live+216,1,8);

    auto metadata=block(32);
    Header(metadata,32,5,0x40000002,16);
    Little(metadata+32,3,2);Little(metadata+36,1,4);Little(metadata+42,8,2);
    Little(metadata+44,8,2);Little(metadata+58,8,2);
    Little(metadata+64,(uint64_t(1)<<60)|4,8);
    constexpr char name[]="Before edit";
    constexpr auto recordSize=50+sizeof(name);
    Little(metadata+60,recordSize,2);Little(metadata+62,recordSize,2);
    auto metadataEnd=metadata+size-40;
    Little(metadataEnd+4,size,4);
    auto record=metadataEnd-recordSize;
    Little(record+8,39,8);
    Little(record+16,1000000000,8);
    Little(record+48,sizeof(name),2);
    memcpy(record+50,name,sizeof(name));

    auto map=block(25);
    Little(map+36,2,4);Little(map+42,8,2);Little(map+44,32,2);
    Little(map+56,0,2);Little(map+58,16,2);Little(map+60,16,2);Little(map+62,32,2);
    auto mapEnd=map+size-40;
    for(unsigned i=0;i<2;++i){
        auto key=map+64+i*16;
        Little(key,2048,8);Little(key+8,4+i,8);
        auto value=mapEnd-(i+1)*16;
        Little(value+4,size,4);Little(value+8,i==0?30:26,8);
    }
    auto historical=block(30);
    memcpy(historical,block(26),size);
    Header(historical,2048,4,2,14);
    auto extent=historical+size-40-Apfs::Le16(historical+56+5*8+4);
    Little(extent+8,44,8);
    memset(block(44),'S',size);
    for(const auto object:{25,30,32,37,39})Seal(disk.bytes,object*size,size);
    return disk;
}

Memory Gpt(const Memory& raw,unsigned sector)
{
    const size_t start=1024*1024;
    Memory disk(start+size_t(raw.Size())+sector*40);
    const auto sectors=disk.Size()/sector;
    auto boot=disk.bytes.data();
    boot[510]=0x55;boot[511]=0xaa;boot[450]=0xee;
    Little(boot+454,1,4);Little(boot+458,sectors-1,4);
    Bytes table(sector);
    const uint8_t guid[]={0xef,0x57,0x34,0x7c,0,0,0xaa,0x11,0xaa,0x11,0,0x30,0x65,0x43,0xec,0xac};
    memcpy(table.data(),guid,sizeof(guid));table[16]=1;
    Little(table.data()+32,start/sector,8);
    Little(table.data()+40,(start+raw.Size())/sector-1,8);
    for(bool backup:{false,true}){
        const auto current=backup?sectors-1:1,entries=backup?sectors-2:2;
        Bytes header(92);
        memcpy(header.data(),"EFI PART",8);
        Little(header.data()+8,0x10000,4);Little(header.data()+12,header.size(),4);
        Little(header.data()+24,current,8);Little(header.data()+32,backup?1:sectors-1,8);
        Little(header.data()+40,34,8);Little(header.data()+48,sectors-34,8);
        header[56]=1;Little(header.data()+72,entries,8);
        Little(header.data()+80,sector/128,4);Little(header.data()+84,128,4);
        Little(header.data()+88,Crc(table),4);Little(header.data()+16,Crc(header),4);
        memcpy(boot+current*sector,header.data(),header.size());
        memcpy(boot+entries*sector,table.data(),table.size());
    }
    memcpy(boot+start,raw.bytes.data(),raw.bytes.size());
    return disk;
}
}

Bytes ApfsRemovableFixture(unsigned sector)
{
    return sector?Gpt(BrowseFixture(),sector).bytes:BrowseFixture().bytes;
}

int RunApfsContainerTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const auto root=std::string(directory)+"\\";
    FILE* report=fopen((root+"apfs-container-report.txt").c_str(),"w");
    if(!report)return 2;
    int result=0;
    try{
        Memory unknown(4096);
        Check(!Apfs::Probe(unknown),"Unrelated image recognized as APFS");
        auto disk=Raw();
        const auto found=Apfs::Probe(disk);
        Check(found&&found->offset==0&&found->blockSize==4096&&found->blockCount==256&&
              found->objectMap==12&&found->transaction==5&&found->uuid[0]==42&&
              found->volumes==std::vector<uint64_t>{1024,1029},"Unexpected APFS geometry or volume IDs");
        auto large=Raw(65536);
        Check(Apfs::Probe(large)->blockSize==65536,"64 KiB APFS block size was rejected");
        auto bad=disk;
        bad.bytes[2048]^=1;
        Reject([&]{Apfs::Probe(bad);},"Corrupt APFS checksum was accepted");
        bad=disk;Little(bad.bytes.data()+40,UINT64_MAX,8);Seal(bad.bytes,0,4096);
        Reject([&]{Apfs::Probe(bad);},"Out-of-bounds APFS geometry was accepted");
        bad=disk;Little(bad.bytes.data()+192,1024,8);Seal(bad.bytes,0,4096);
        Reject([&]{Apfs::Probe(bad);},"Duplicate APFS volume was accepted");
        for(unsigned sector:{512,4096}){
            auto gpt=Gpt(disk,sector);
            auto container=Apfs::Probe(gpt);
            Check(container&&container->offset==1024*1024&&container->volumes==found->volumes,
                  "GPT APFS partition was not recognized");
            gpt.bytes[gpt.bytes.size()-sector+16]^=1;
            Reject([&]{Apfs::Probe(gpt);},"APFS probe ignored an invalid GPT backup");
        }
        auto mapped=disk;
        ObjectMapFixture(mapped);
        // The object-map nodes belong to transaction 5; only leaf keys can be newer.
        const Apfs::ObjectMap map(mapped,*found,12,5);
        Check(!map.Lookup(1024)&&map.Lookup(1029)==38&&!map.Lookup(1030),
              "APFS object map ignored a tombstone or selected a future version");
        constexpr size_t leaf=14*4096;
        Little(mapped.bytes.data()+leaf+58,0,2);Seal(mapped.bytes,leaf,4096);
        Reject([&]{map.Lookup(1029);},"APFS object map accepted an invalid record range");
        ObjectMapFixture(mapped);
        Little(mapped.bytes.data()+13*4096+4096-48,13,8);Seal(mapped.bytes,13*4096,4096);
        Reject([&]{map.Lookup(1029);},"APFS object map accepted a circular child reference");
        fprintf(report,"PASS: APFS object-map index, object versions, tombstones, bounds and cycles.\n");
        auto ring=WithCheckpoints();
        auto checkpoint=Apfs::Probe(ring);
        Check(checkpoint&&checkpoint->transaction==7&&checkpoint->checkpointBlock==1,
              "Newest APFS checkpoint across the ring boundary was not selected");
        ring.bytes[8*4096+64]^=1;
        checkpoint=Apfs::Probe(ring);
        Check(checkpoint&&checkpoint->transaction==6&&checkpoint->checkpointBlock==5,
              "Incomplete newest APFS checkpoint did not fall back to the previous checkpoint");
        ring.bytes[4*4096+64]^=1;
        Reject([&]{Apfs::Probe(ring);},"Container with no complete checkpoint was accepted");
        fprintf(report,"PASS: latest complete checkpoint, wraparound and rejection of corrupt checkpoint maps.\n");
        CheckpointWrites(root,report);
        if(const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE")){
            const auto authentic=MacBrowser::OpenBrowserImage(fixture);
            Check(authentic&&authentic->Kind()==std::string("APFS"),"Independent APFS fixture did not mount");
            const auto roots=authentic->List();
            MacBrowser::Entry many;
            Check(roots.size()>=1&&authentic->FindChild(2,u"many",many),"Independent APFS fixture lost its directory");
            const auto files=authentic->List(many.id);
            Check(files.size()==400,"Independent APFS fixture lost files across catalog nodes");
            MacBrowser::Entry a,b;
            Check(authentic->FindChild(2,u"hard_a.txt",a)&&authentic->FindChild(2,u"hard_b.txt",b)&&
                  a.hardLink&&b.hardLink&&a.id!=b.id&&a.resolvedId==b.resolvedId&&
                  !a.identity.SameObject(b.identity),
                  "APFS hard-linked names did not retain distinct selections and a shared inode");
            const auto name=std::find_if(files.begin(),files.end(),[](const auto& file){return file.name==u"file_0222.txt";});
            Check(name!=files.end()&&name->forks[0].size==10,"Independent APFS fixture lost a file size");
            char data[10];
            authentic->Read(name->id,0,0,data,sizeof(data));
            Check(!memcmp(data,"entry 222\n",sizeof(data)),"Independent APFS file differed from expected data");
            MacBrowser::Entry symlink;
            Check(authentic->FindChild(2,u"link.txt",symlink)&&!symlink.directory&&!symlink.readError.empty()&&
                  authentic->PropertiesDetails(symlink.id)=="Symbolic link to small.txt"&&
                  !authentic->GetAttributes(symlink.id).count(u"com.apple.fs.symlink"),
                  "macOS-generated APFS symbolic link target was not exposed safely");
            Reject([&]{authentic->Read(symlink.id,0,0,nullptr,0);},"Reading an APFS symlink was allowed");
            MacBrowser::Entry compressed;
            Check(authentic->FindChild(2,u"decmpfs.txt",compressed)&&compressed.compressed&&
                  compressed.forks[0].size==112000,
                  "Independent APFS compressed size differed from expected");
            Bytes decoded(112000);
            authentic->Read(compressed.id,0,0,decoded.data(),decoded.size());
            for(size_t i=0;i<decoded.size();i+=28)
                Check(!memcmp(decoded.data()+i,"a compressible line of text\n",28),
                      "Independent APFS compressed contents differed from expected");
            fprintf(report,"PASS: macOS-generated APFS image and 400-file multi-node directory.\n");
        }
        // Decompress apfs-encrypted and apfs-converted from qnxprobe's tests/fixtures.
        if(const auto fixtures=_wgetenv(L"HFV_APFS_LOCKED_FIXTURES")){
            const auto checkLocked=[](MacBrowser::Volume& container,const std::u16string& name){
                MacBrowser::Entry locked;
                Check(container.FindChild(2,name,locked)&&locked.directory&&!locked.readError.empty()&&
                      container.PropertiesDetails(locked.id)=="This APFS volume is encrypted and locked",
                      "Encrypted APFS volume did not appear as a locked volume");
                Reject([&]{container.List(locked.id);},"Encrypted APFS catalog was read before unlocking");
            };
            const auto mixed=MacBrowser::OpenBrowserImage(std::wstring(fixtures)+L"\\apfs-encrypted.sparseimage");
            Check(mixed&&mixed->List().size()==2,"Expected both volumes of the encrypted APFS fixture");
            checkLocked(*mixed,u"SECRETVOL");
            MacBrowser::Entry plain,docs,file;
            Check(mixed->FindChild(2,u"PLAINVOL",plain)&&mixed->FindChild(plain.id,u"docs",docs)&&
                  mixed->FindChild(docs.id,u"pattern.bin",file)&&file.forks[0].size==262144,
                  "Locked APFS volume blocked its readable sibling");
            Bytes pattern(262144);
            mixed->Read(file.id,0,0,pattern.data(),pattern.size());
            for(size_t i=0;i<pattern.size();++i)
                Check(pattern[i]==(i*131+17)%253,"Unexpected contents in the readable sibling APFS volume");
            const auto converted=MacBrowser::OpenBrowserImage(std::wstring(fixtures)+L"\\apfs-converted.sparseimage");
            Check(converted&&converted->List().size()==1,"Expected the single converted APFS volume");
            checkLocked(*converted,u"CONVVOL");
            fprintf(report,"PASS: macOS-generated encrypted APFS volumes appear locked, with readable siblings still accessible.\n");
            for(const bool convertedVolume:{false,true}){
                const auto& opened=convertedVolume?converted:mixed;
                MacBrowser::Entry lockedRoot;
                Check(opened->FindChild(2,convertedVolume?u"CONVVOL":u"SECRETVOL",lockedRoot),
                      "Missing locked APFS volume");
                const std::wstring expectedHint=convertedVolume?L"the qnxprobe conversion test":L"";
                bool initialPrompt=false;
                try{opened->List(lockedRoot.id);}
                catch(const Apfs::LockedVolume& result){initialPrompt=!result.retry&&result.hint==expectedHint;}
                Check(initialPrompt,"Incorrect password hint on the initial APFS prompt");
                const Dmg::Password wrong(L"wrong-password");
                bool retry=false;
                try{Apfs::Unlock(*opened,lockedRoot.id,wrong);}
                catch(const Apfs::LockedVolume& result){retry=result.retry&&result.hint==expectedHint;}
                Check(retry&&!opened->Stat(lockedRoot.id).readError.empty(),
                      "An incorrect password changed the locked APFS volume");
                const Dmg::Password password(convertedVolume?L"qnxprobe-apfs-convert":L"qnxprobe-apfs-test");
                Apfs::Unlock(*opened,lockedRoot.id,password);
                Check(!opened->Writable()&&MacBrowser::ExecutionFor(*opened).processLocal&&
                      opened->Stat(lockedRoot.id).readError.empty()&&
                      opened->Stat(lockedRoot.id).identity==lockedRoot.identity,
                      "Decrypted APFS mount did not keep its identity and stay local and read-only");
                const auto rootId=lockedRoot.id;
                MacBrowser::Entry docs,patternFile;
                Check(opened->FindChild(rootId,u"docs",docs)&&
                      opened->FindChild(docs.id,u"pattern.bin",patternFile)&&patternFile.forks[0].size==262144,
                      "Decrypted APFS catalog is missing the test file");
                Bytes decoded(262144);
                opened->Read(patternFile.id,0,0,decoded.data(),decoded.size());
                for(size_t i=0;i<decoded.size();++i)
                    Check(decoded[i]==(i*131+17)%253,"Decrypted APFS bytes differ");
                Bytes slice(1500);
                opened->Read(patternFile.id,0,505,slice.data(),slice.size());
                Check(std::equal(slice.begin(),slice.end(),decoded.begin()+505),
                      "Unaligned decrypted APFS read differs");
                if(!convertedVolume){
                    const auto to=VolumeCoord::Wide(root+"encrypted-export");
                    Check(CreateDirectoryW(to.c_str(),nullptr)!=FALSE,"Create the APFS export directory");
                    FileOperations::Request request=FileOperations::OrdinaryCopy{
                        {FileOperations::Location::Mounted(opened,patternFile.id)},FileOperations::Location::Windows(to)};
                    request.background=true;
                    CopyProcess::enabled=true;
                    CopyProcess::lastProcess=0;
                    Check(!FileOperations::InspectPlanForTest(request,opened->ResourceKey()).temporaryWorker,
                          "A temporary worker was selected for encrypted APFS");
                    const auto copy=FileOperations::Submit(std::move(request));
                    Check(WaitForSingleObject(copy.Job()->event(),15000)==WAIT_OBJECT_0,
                          "Encrypted APFS export timed out");
                    const auto result=copy.Job()->result();
                    Check(!result.code&&!CopyProcess::lastProcess,result.message.c_str());
                    const auto file=HfsPlus::OpenImage((to+L"\\pattern.bin").c_str(),false);
                    Check(file->Size()==decoded.size()&&Disk::Read(*file,0,decoded.size())==decoded,
                          "Encrypted APFS export did not match the plaintext");
                }
            }
            fprintf(report,"PASS: APFS software password unlock, wrong-password rejection, complete and unaligned reads, and process-local export.\n");
            fprintf(report,"PASS: genuine APFS password hint on initial and retry prompts, and a volume without a hint.\n");
        }
        if(const auto fixture=_wgetenv(L"HFV_APFS_SNAPSHOT_FIXTURE")){
            const auto volume=MacBrowser::OpenBrowserImage(fixture);
            MacBrowser::Entry current,snapshot,folder,file;
            Check(volume&&volume->FindChild(2,u"Independent APFS",current)&&
                  volume->FindChild(2,u"Independent APFS [Snapshot: Independent backup]",snapshot),
                  "Independently authored APFS snapshot was not listed");
            constexpr char expected[]="Independently written APFS data\n";
            for(const auto id:{current.id,snapshot.id}){
                Check(volume->FindChild(id,u"Folder",folder)&&volume->FindChild(folder.id,u"sample.txt",file)&&
                      file.forks[0].size==sizeof(expected)-1,"Independent snapshot catalog was not readable");
                char bytes[sizeof(expected)-1];
                volume->Read(file.id,0,0,bytes,sizeof(bytes));
                Check(!memcmp(bytes,expected,sizeof(bytes)),"Independent snapshot bytes differ");
            }
            Storage::Job conversion;
            const auto extracted=VolumeCoord::Wide(root+"independent-snapshot-raw.img");
            Check(!Dmg::Convert(fixture,extracted,Dmg::Output::RawImage,conversion).code,
                  "Could not extract the independent APFS fixture for filesystem checking");
            {
                auto raw=HfsPlus::OpenImage(extracted.c_str(),true);
                const auto checkpoint=Apfs::Probe(*raw);
                Check(checkpoint&&Apfs::CommitCheckpoint(*raw,*checkpoint,conversion)>checkpoint->transaction,
                      "Could not commit a checkpoint to the independently authored APFS image");
            }
            const auto committed=MacBrowser::OpenBrowserImage(extracted);
            Check(committed&&committed->FindChild(2,u"Independent APFS",current)&&
                  committed->FindChild(2,u"Independent APFS [Snapshot: Independent backup]",snapshot),
                  "Checkpoint publication lost the live volume or snapshot");
            for(const auto id:{current.id,snapshot.id}){
                Check(committed->FindChild(id,u"Folder",folder)&&committed->FindChild(folder.id,u"sample.txt",file),
                      "Checkpoint publication lost the independent APFS file");
                char bytes[sizeof(expected)-1];
                committed->Read(file.id,0,0,bytes,sizeof(bytes));
                Check(!memcmp(bytes,expected,sizeof(bytes)),"Checkpoint publication changed independent APFS file data");
            }
            fprintf(report,"PASS: independently authored APFS snapshot and live file survived checkpoint publication.\n");
        }
        auto browsable=Gpt(BrowseFixture(),4096);
        const auto path=VolumeCoord::Wide(root+"apfs.img");
        {
            auto image=CreateFormatOutput(path.c_str(),browsable.Size());
            image->Write(0,browsable.bytes.data(),browsable.bytes.size());image->Flush();image->Keep();
        }

        auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&volume->Name()==u"APFS Fixture"&&!volume->Writable()&&!volume->CaseSensitive(),
              "APFS image did not mount read-only through the normal browser");
        const auto folders=volume->List();
        Check(folders.size()==1&&folders.front().directory&&folders.front().name==u"Folder",
              "APFS root directory was not listed");
        const auto files=volume->List(folders.front().id);
        Check(files.size()==1&&files.front().name==u"Long \u00e9.txt"&&files.front().forks[0].size==8199,
              "APFS file name or size was misread");
        Bytes bytes(4104);
        volume->Read(files.front().id,0,4095,bytes.data(),bytes.size());
        Check(bytes.front()=='A'&&std::all_of(bytes.begin()+1,bytes.begin()+4097,[](uint8_t b){return b==0;})&&
              !memcmp(bytes.data()+4097,"last!!!",7),"APFS fragmented/sparse read returned incorrect bytes");
        Reject([&]{volume->Remove(files.front().id);},"APFS read-only volume accepted a delete");
        Reject([&]{volume->Read(files.front().id,0,8198,bytes.data(),2);},"APFS read accepted an out-of-range request");
        const auto host=VolumeCoord::Wide(root+"export");
        Check(CreateDirectoryW(host.c_str(),nullptr)!=FALSE,"Create APFS export folder");
        FileOperations::Request request=FileOperations::OrdinaryCopy{
            {FileOperations::Location::Mounted(volume,files.front().id)},FileOperations::Location::Windows(host)};
        request.background=true;
        CopyProcess::enabled=true;CopyProcess::lastProcess=0;
        auto operation=FileOperations::Submit(std::move(request));
        const auto start=GetTickCount();
        while(!operation.Job()->done()){
            Check(GetTickCount()-start<15000,"APFS worker copy timed out");
            Sleep(5);
        }
        const auto copy=operation.Job()->result();
        Check(!copy.code,copy.message.c_str());
        Check(CopyProcess::lastProcess!=0,"APFS export did not use the subprocess");
        auto target=MacBrowser::OpenWindows(host);
        MacBrowser::Entry copied;
        Check(target->FindChild(2,files.front().name,copied)&&copied.forks[0].size==8199,
              "APFS copy did not publish the file with its full length");
        Bytes exported(bytes.size());
        target->Read(copied.id,0,4095,exported.data(),exported.size());
        Check(exported==bytes,"APFS subprocess copy changed file data");
        const auto withFork=BrowseFixture(true);
        const auto withForkPath=VolumeCoord::Wide(root+"attributes.img");
        {
            auto image=CreateFormatOutput(withForkPath.c_str(),withFork.Size());
            image->Write(0,withFork.bytes.data(),withFork.bytes.size());image->Flush();image->Keep();
        }
        auto macFile=MacBrowser::OpenBrowserImage(withForkPath);
        auto folder=macFile->List().at(0);
        auto withMetadata=macFile->List(folder.id).at(0);
        auto attributes=macFile->GetAttributes(withMetadata.id);
        Check(attributes.size()==1&&attributes.at(u"user.example")==AttributeData{1,2,3},
              "APFS embedded attribute was not preserved");
        Check(withMetadata.forks[1].size==8,"APFS resource-fork stream was not discovered");
        Bytes resource(8);
        macFile->Read(withMetadata.id,1,0,resource.data(),resource.size());
        Check(!memcmp(resource.data(),"resource",8),"APFS resource fork was read incorrectly");
        const auto output=VolumeCoord::Wide(root+"metadata.hfs");
        auto image=CreateFormatOutput(output.c_str(),8*1024*1024);
        HfsPlus::Volume::Format(*image,u"Destination",false);
        image->Keep();image.reset();
        auto targetMac=MacBrowser::OpenImage(output);
        Storage::Job job;
        Check(MacBrowser::Copy(*macFile,withMetadata.id,*targetMac,2,job),"APFS copy with attributes failed");
        MacBrowser::Entry preserved;
        const auto targets=targetMac->List();
        Check(targets.size()==1,"APFS transfer did not publish exactly one file");
        preserved=targets.front();
        Check(preserved.name==HfsPlus::Normalize(withMetadata.name,targetMac->CaseSensitive()),"APFS transfer changed its name");
        Check(preserved.forks[1].size==8,"APFS transfer lost the resource fork");
        const auto copiedAttributes=targetMac->GetAttributes(preserved.id);
        Check(copiedAttributes==attributes,"APFS transfer lost the extended attribute");
        Bytes restored(8);
        targetMac->Read(preserved.id,1,0,restored.data(),restored.size());
        Check(restored==resource,"APFS transfer changed the resource fork");
        fprintf(report,"PASS: APFS embedded metadata and extent-backed resource fork preserved through copy to HFS Plus.\n");
        for(const auto link:{Symlink::Valid,Symlink::Malformed}){
            const auto disk=BrowseFixture(false,false,false,link);
            const auto path=VolumeCoord::Wide(root+(link==Symlink::Valid?"symlink.img":"bad-symlink.img"));
            {
                auto image=CreateFormatOutput(path.c_str(),disk.Size());
                image->Write(0,disk.bytes.data(),disk.bytes.size());image->Flush();image->Keep();
            }
            auto volume=MacBrowser::OpenBrowserImage(path);
            const auto folder=volume->List().at(0);
            const auto file=volume->List(folder.id).at(0);
            const auto details=link==Symlink::Valid?"Symbolic link to /a/b\\x0a":"Symbolic link target unavailable";
            Check(volume->PropertiesDetails(file.id)==details&&!file.readError.empty()&&
                  volume->GetAttributes(file.id).empty(),"APFS symbolic link properties are incorrect");
            Reject([&]{volume->Read(file.id,0,0,nullptr,0);},"APFS symlink was readable");
        }
        fprintf(report,"PASS: APFS symbolic link targets, malformed targets, and read refusal.\n");
        auto linkedDisk=BrowseFixture(false,true);
        const auto linkedPath=VolumeCoord::Wide(root+"linked.img");
        {
            auto image=CreateFormatOutput(linkedPath.c_str(),linkedDisk.Size());
            image->Write(0,linkedDisk.bytes.data(),linkedDisk.bytes.size());image->Flush();image->Keep();
        }
        auto linked=MacBrowser::OpenBrowserImage(linkedPath);
        const auto sourceFolder=linked->List().at(0);
        MacBrowser::Entry sourceAlias,sourceFile;
        Check(linked->FindChild(sourceFolder.id,u"Alias A",sourceAlias)&&
              linked->FindChild(sourceFolder.id,withMetadata.name,sourceFile)&&
              sourceAlias.resolvedId==sourceFile.resolvedId&&!sourceAlias.identity.SameObject(sourceFile.identity),
              "Same-folder APFS hard links were treated as one browser selection");
        auto reopened=MacBrowser::OpenBrowserImage(linkedPath);
        MacBrowser::Entry reopenedAlias;
        Check(reopened->FindChild(reopened->List().at(0).id,u"Alias A",reopenedAlias)&&
              reopenedAlias.identity==sourceAlias.identity,"APFS hard-link selection changed after reopen");
        Storage::Job linkCopy;
        Check(MacBrowser::Copy(*linked,linked->List().at(0).id,*targetMac,2,linkCopy),
              "APFS hard-link group copy did not complete");
        MacBrowser::Entry targetFolder,aliasA,aliasB,other,first;
        Check(targetMac->FindChild(2,u"Folder",targetFolder)&&
              targetMac->FindChild(targetFolder.id,u"Alias A",aliasA)&&targetMac->FindChild(targetFolder.id,u"Alias B",aliasB)&&
              targetMac->FindChild(targetFolder.id,HfsPlus::Normalize(withMetadata.name,false),first)&&
              targetMac->FindChild(targetFolder.id,u"Other",other),"APFS hard-link names did not all copy");
        Check(aliasA.linkCount==2&&aliasB.linkCount==2&&aliasA.resolvedId==first.resolvedId&&
              aliasB.resolvedId==other.resolvedId&&aliasA.resolvedId!=aliasB.resolvedId,
              "APFS copy conflated distinct hard-link groups");
        char aByte=0,bByte=0;
        targetMac->Read(aliasA.id,0,0,&aByte,1);targetMac->Read(aliasB.id,0,0,&bByte,1);
        Check(aByte=='A'&&bByte=='l',"APFS hard-link groups lost independent file contents");
        fprintf(report,"PASS: separate APFS hard-link groups retain file identity and contents through transfer.\n");
        auto compressedDisk=BrowseFixture(false,false,true);
        const auto compressedPath=VolumeCoord::Wide(root+"compressed.img");
        {
            auto image=CreateFormatOutput(compressedPath.c_str(),compressedDisk.Size());
            image->Write(0,compressedDisk.bytes.data(),compressedDisk.bytes.size());image->Flush();image->Keep();
        }
        auto compressionVolume=MacBrowser::OpenBrowserImage(compressedPath);
        const auto compressedFile=compressionVolume->List(compressionVolume->List().at(0).id).at(0);
        Check(compressedFile.compressed&&compressedFile.forks[0].size==70001&&
              compressionVolume->GetAttributes(compressedFile.id).empty(),
              "APFS inline compression logical size or attributes were misread");
        Bytes decoded(5000);
        compressionVolume->Read(compressedFile.id,0,65000,decoded.data(),decoded.size());
        Check(std::all_of(decoded.begin(),decoded.end(),[](uint8_t b){return b=='C';}),
              "APFS inline zlib crossed 64 KiB incorrectly");
        fprintf(report,"PASS: bounded APFS inline decmpfs decoding beyond 64 KiB.\n");
        {
            auto disk=SnapshotFixture();
            const auto save=[&](const char* name){
                const auto path=VolumeCoord::Wide(root+name);
                auto image=CreateFormatOutput(path.c_str(),disk.Size());
                image->Write(0,disk.bytes.data(),disk.bytes.size());image->Flush();image->Keep();
                return path;
            };
            auto snapshots=MacBrowser::OpenBrowserImage(save("snapshot.img"));
            MacBrowser::Entry active,historical,folder,newFile,oldFile;
            Check(snapshots&&snapshots->List().size()==2&&
                  snapshots->FindChild(2,u"APFS Fixture",active)&&
                  snapshots->FindChild(2,u"APFS Fixture [Snapshot: Before edit]",historical)&&
                  historical.datesValid&&historical.created==2082844801&&
                  snapshots->PropertiesDetails(historical.id)=="APFS snapshot (transaction 4)",
                  "APFS snapshot did not appear alongside its live volume");
            Check(snapshots->FindChild(active.id,u"Folder",folder)&&
                  snapshots->FindChild(folder.id,u"Long \u00e9.txt",newFile)&&
                  snapshots->FindChild(historical.id,u"Folder",folder)&&
                  snapshots->FindChild(folder.id,u"Long \u00e9.txt",oldFile)&&
                  !newFile.identity.SameObject(oldFile.identity),"Historical APFS catalog reused the live identity");
            char current=0,previous=0;
            snapshots->Read(newFile.id,0,0,&current,1);
            snapshots->Read(oldFile.id,0,0,&previous,1);
            Check(current=='A'&&previous=='S',"APFS snapshot returned the current file contents");
            Reject([&]{snapshots->Remove(oldFile.id);},"An APFS snapshot accepted a write");
            const auto to=VolumeCoord::Wide(root+"snapshot-export");
            Check(CreateDirectoryW(to.c_str(),nullptr)!=FALSE,"Create APFS snapshot export directory");
            FileOperations::Request request=FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(snapshots,oldFile.id)},FileOperations::Location::Windows(to)};
            request.background=true;
            CopyProcess::lastProcess=0;
            const auto copy=FileOperations::Submit(std::move(request));
            Check(WaitForSingleObject(copy.Job()->event(),15000)==WAIT_OBJECT_0,"APFS snapshot export timed out");
            const auto result=copy.Job()->result();
            Check(!result.code&&CopyProcess::lastProcess,result.message.c_str());
            const auto exported=MacBrowser::OpenWindows(to);
            MacBrowser::Entry file;
            Check(exported->FindChild(2,oldFile.name,file),"APFS snapshot export was missing");
            exported->Read(file.id,0,0,&previous,1);
            Check(previous=='S',"Worker exported the live APFS file instead of the snapshot");

            disk.bytes[39*4096+32]^=1;
            auto recovered=MacBrowser::OpenBrowserImage(save("bad-snapshot.img"));
            Check(recovered&&recovered->Name()==u"APFS Fixture"&&recovered->FindChild(2,u"Folder",folder),
                  "A corrupt snapshot prevented the live volume from opening");
            fprintf(report,"PASS: APFS read-only snapshot roots, historic object versions, worker export, and recovery from a corrupt snapshot.\n");
        }
        auto multiple=SecondVolume();
        const auto multiPath=VolumeCoord::Wide(root+"multi-volume.img");
        {
            auto image=CreateFormatOutput(multiPath.c_str(),multiple.Size());
            image->Write(0,multiple.bytes.data(),multiple.bytes.size());image->Flush();image->Keep();
        }
        auto container=MacBrowser::OpenBrowserImage(multiPath);
        const auto volumes=container->List();
        Check(container->Name()==u"APFS Container"&&volumes.size()==2&&volumes[0].name==u"APFS Fixture"&&
              volumes[1].name==u"APFS Fixture (2)"&&volumes[0].id!=volumes[1].id,
              "APFS container did not expose both volumes with unambiguous names");
        MacBrowser::Entry insensitive,sensitive;
        Check(container->FindChild(volumes[0].id,u"folder",insensitive)&&
              !container->FindChild(volumes[1].id,u"folder",sensitive)&&
              container->FindChild(volumes[1].id,u"Folder",sensitive)&&insensitive.id!=sensitive.id,
              "APFS child lookups did not honor each volume's case sensitivity");
        uint32_t second=0;
        for(size_t i=0;i<volumes.size();++i){
            const auto folder=container->List(volumes[i].id).at(0);
            const auto file=container->List(folder.id).at(0);
            char data=0;
            container->Read(file.id,0,0,&data,1);
            Check(data=='A'+i,"APFS reused an extent from another volume with the same stream ID");
            if(i==1)second=file.id;
        }
        const auto multiTarget=VolumeCoord::Wide(root+"multi-export");
        Check(CreateDirectoryW(multiTarget.c_str(),nullptr)!=FALSE,"Create APFS multi-volume export folder");
        FileOperations::Request routed=FileOperations::OrdinaryCopy{
            {FileOperations::Location::Mounted(container,second)},FileOperations::Location::Windows(multiTarget)};
        routed.background=true;
        CopyProcess::lastProcess=0;
        auto multiCopy=FileOperations::Submit(std::move(routed));
        const auto multiStart=GetTickCount();
        while(!multiCopy.Job()->done()){
            Check(GetTickCount()-multiStart<15000,"APFS multi-volume worker copy timed out");
            Sleep(5);
        }
        const auto multiResult=multiCopy.Job()->result();
        Check(!multiResult.code,multiResult.message.c_str());
        auto multiExports=MacBrowser::OpenWindows(multiTarget);
        const auto secondCopy=multiExports->List().at(0);
        char secondData=0;
        multiExports->Read(secondCopy.id,0,0,&secondData,1);
        Check(CopyProcess::lastProcess&&secondData=='B',"APFS copy worker reopened the wrong volume");
        fprintf(report,"PASS: same-named APFS volumes, per-volume case sensitivity, overlapping object IDs and export from the second volume.\n");
        const auto dmgPath=VolumeCoord::Wide(root+"apfs.dmg");
        Storage::Job convert;
        const auto wrapped=Dmg::Convert(path,dmgPath,Dmg::Output::CompressedDmg,convert);
        Check(!wrapped.code,wrapped.message.c_str());
        auto dmg=MacBrowser::OpenBrowserImage(dmgPath);
        Check(dmg&&dmg->Name()==u"APFS Fixture"&&!dmg->Writable(),"APFS GPT container did not reopen in compressed DMG");
        const auto dmgItem=dmg->List(dmg->List().at(0).id).at(0);
        Bytes fromDmg(bytes.size());
        dmg->Read(dmgItem.id,0,4095,fromDmg.data(),fromDmg.size());
        Check(fromDmg==bytes,"APFS file bytes differed through the compressed DMG reader");
        fprintf(report,"PASS: APFS inside a compressed DMG with a GPT partition table.\n");
        fprintf(report,"PASS: normal browser mount, UTF-8 names, sparse and fragmented data, worker export and read-only enforcement.\n");
        fprintf(report,"PASS: raw APFS, 4/64 KiB blocks, APFS GPT with 512/4096-byte sectors, geometry and checksum validation, duplicate volume refusal and normal open routing.\n");
    }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());result=1;}
    CopyProcess::enabled=false;
    fclose(report);
    return result;
}
#endif
