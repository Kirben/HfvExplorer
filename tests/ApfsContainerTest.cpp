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
    Check(Apfs::CreateCatalogDirectory(created,*start,2,u"New Folder",controlJob)==2&&reopen(created)==2,
          "APFS catalog transaction did not publish its checkpoint");
    for(unsigned step=1;step<=created.event;++step){
        CheckpointFaultImage attempt(original);
        attempt.failAt=step;
        attempt.fault=step==created.event||step==created.event-2?Fault::FailedFlush:Fault::TornWrite;
        Reject([&]{Apfs::CreateCatalogDirectory(attempt,*start,2,u"New Folder",controlJob);},
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
    Reject([&]{Apfs::CreateCatalogDirectory(created,*start,2,u"Stale",controlJob);},
           "A stale catalog mutation was accepted");
    Reject([&]{Apfs::CreateCatalogDirectory(created,*state,2,u"new folder",controlJob);},
           "A case-insensitive APFS name collision was accepted");
    Check(created.event==eventsAfter,"Rejected APFS catalog write modified the image");

    const auto openCheckpoint=[](const CheckpointFaultImage& disk){
        auto memory=std::make_unique<Memory>(disk.durable.size());
        memory->bytes=disk.durable;
        std::unique_ptr<Source> source=std::move(memory);
        auto mounted=Apfs::Open(source,"apfs-catalog-test");
        Check(mounted!=nullptr,"APFS test image could not be reopened");
        return mounted;
    };
    const auto listFolders=[&](const CheckpointFaultImage& disk){
        const auto mounted=openCheckpoint(disk);
        std::vector<std::u16string> names;
        for(const auto& entry:mounted->List()){
            Check(entry.directory&&mounted->List(entry.id).empty(),"Unexpected APFS directory contents");
            names.push_back(entry.name);
        }
        std::sort(names.begin(),names.end());
        return names;
    };
    CheckpointFaultImage reused(created.durable);
    Check(Apfs::CreateCatalogDirectory(reused,*state,2,u"Another",controlJob)==3&&reopen(reused)==3,
          "APFS catalog write could not reuse retired blocks");
    const auto oldMap=first->objectMap*4096;
    Check(!std::equal(original.begin()+oldMap,original.begin()+oldMap+4096,reused.durable.begin()+oldMap),
          "APFS catalog write did not reuse the initial checkpoint's object map block");
    for(unsigned step=1;step<=reused.event;++step){
        CheckpointFaultImage attempt(created.durable);
        attempt.failAt=step;
        attempt.fault=step==reused.event||step==reused.event-2?Fault::FailedFlush:Fault::TornWrite;
        Reject([&]{Apfs::CreateCatalogDirectory(attempt,*state,2,u"Another",controlJob);},
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
    Reject([&]{Apfs::CreateCatalogDirectory(liveBlockQueued,*state,2,u"Another",controlJob);},
           "APFS reclamation accepted a retired range overlapping live metadata");
    Check(!liveBlockQueued.event&&liveBlockQueued.durable==badQueue,"APFS reclamation wrote invalid allocation data");

    std::vector<std::u16string> expected{u"Another",u"New Folder"};
    Check(listFolders(reused)==expected,"APFS block reuse lost a previously created directory");
    for(uint64_t tx=4;tx<=13;++tx){
        std::u16string folderName=u"Folder ";
        folderName+=char16_t(u'A'+tx-4);
        const auto checkpoint=Apfs::Probe(reused);
        Check(checkpoint&&Apfs::CreateCatalogDirectory(reused,*checkpoint,2,folderName,controlJob)==tx&&reopen(reused)==tx,
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
    CheckpointFaultImage nested(created.durable);
    for(const uint64_t missing:{1,3,17}){
        Reject([&]{Apfs::CreateCatalogDirectory(nested,*state,missing,u"Child",controlJob);},
               "APFS accepted a reserved or missing folder as a destination");
    }
    Check(!nested.event&&nested.durable==created.durable,"Invalid APFS destinations changed the image");
    // The formatter assigns the first user folder inode 16. The browser
    // assigns separate, mount-local IDs for enumeration and lookup.
    constexpr uint64_t firstFolder=16,childFolder=17;
    Check(Apfs::CreateCatalogDirectory(nested,*state,firstFolder,u"New Folder",controlJob)==3,
          "APFS nested creation rejected a name used under a different parent");
    for(unsigned step=1;step<=nested.event;++step){
        CheckpointFaultImage attempt(created.durable);
        attempt.failAt=step;
        attempt.fault=step==nested.event||step==nested.event-2?Fault::FailedFlush:Fault::TornWrite;
        Reject([&]{Apfs::CreateCatalogDirectory(attempt,*state,firstFolder,u"New Folder",controlJob);},
               "Injected APFS nested directory interruption succeeded");
        Check(reopen(attempt)==2&&listFolders(attempt)==std::vector<std::u16string>{u"New Folder"},
              "Interrupted APFS nested directory creation changed its parent's listing");
    }
    const auto childCheckpoint=Apfs::Probe(nested);
    const auto nestedEvents=nested.event;
    Reject([&]{Apfs::CreateCatalogDirectory(nested,*childCheckpoint,firstFolder,u"new folder",controlJob);},
           "APFS accepted a case-insensitive duplicate in a nested folder");
    Check(nested.event==nestedEvents,"A duplicate APFS nested name changed the image");
    Check(Apfs::CreateCatalogDirectory(nested,*childCheckpoint,childFolder,u"Leaf",controlJob)==4,
          "APFS could not create a third-level directory");
    const auto nestedMounted=openCheckpoint(nested);
    MacBrowser::Entry top,child,leaf;
    Check(nestedMounted->List().size()==1&&nestedMounted->FindChild(2,u"New Folder",top)&&
          nestedMounted->List(top.id).size()==1&&nestedMounted->FindChild(top.id,u"New Folder",child)&&
          nestedMounted->List(child.id).size()==1&&nestedMounted->FindChild(child.id,u"Leaf",leaf)&&
          leaf.directory&&nestedMounted->List(leaf.id).empty(),"APFS nested catalog did not reopen correctly");
    {
        const auto path=VolumeCoord::Wide(root+"checkpoint-nested.img");
        auto image=CreateFormatOutput(path.c_str(),imageSize);
        image->Write(0,nested.durable.data(),nested.durable.size());
        image->Keep();
    }
    fprintf(report,"PASS: APFS nested catalog writes, parent validation, per-folder names, and interrupted nested creation.\n");
    const auto directoryState=Apfs::Probe(nested);
    CheckpointFaultImage files(nested.durable);
    Check(Apfs::CreateEmptyFile(files,*directoryState,childFolder,u"Empty.txt",controlJob)==5,
          "APFS could not create an empty file in a nested folder");
    for(unsigned step=1;step<=files.event;++step){
        CheckpointFaultImage attempt(nested.durable);
        attempt.failAt=step;
        attempt.fault=step==files.event||step==files.event-2?Fault::FailedFlush:Fault::TornWrite;
        Reject([&]{Apfs::CreateEmptyFile(attempt,*directoryState,childFolder,u"Empty.txt",controlJob);},
               "Injected APFS file creation interruption succeeded");
        const auto mounted=openCheckpoint(attempt);
        MacBrowser::Entry topFolder,containingFolder,oldLeaf;
        Check(reopen(attempt)==4&&mounted->FindChild(2,u"New Folder",topFolder)&&
              mounted->FindChild(topFolder.id,u"New Folder",containingFolder)&&
              mounted->List(containingFolder.id).size()==1&&mounted->FindChild(containingFolder.id,u"Leaf",oldLeaf),
              "Interrupted APFS file creation changed the preceding directory");
    }
    const auto withFile=Apfs::Probe(files);
    const auto fileEvents=files.event;
    constexpr uint64_t emptyFile=19;
    Reject([&]{Apfs::CreateCatalogDirectory(files,*withFile,emptyFile,u"Cannot nest",controlJob);},
           "An APFS regular file was accepted as a parent");
    Reject([&]{Apfs::CreateEmptyFile(files,*withFile,childFolder,u"leaf",controlJob);},
           "An APFS file replaced a directory with the same name");
    Reject([&]{Apfs::CreateCatalogDirectory(files,*withFile,childFolder,u"empty.txt",controlJob);},
           "An APFS directory replaced a file with the same name");
    Check(files.event==fileEvents,"An invalid APFS file operation changed the image");
    Check(Apfs::CreateCatalogDirectory(files,*withFile,childFolder,u"Sibling",controlJob)==6,
          "APFS could not edit a catalog containing an empty file");
    const auto withSibling=Apfs::Probe(files);
    Check(Apfs::CreateEmptyFile(files,*withSibling,2,u"Empty.txt",controlJob)==7,
          "APFS could not create an empty file with the same name in a different folder");
    const auto mountedFiles=openCheckpoint(files);
    MacBrowser::Entry nestedTop,nestedParent,file,otherFile,sibling;
    Check(mountedFiles->FindChild(2,u"New Folder",nestedTop)&&
          mountedFiles->FindChild(nestedTop.id,u"New Folder",nestedParent)&&
          mountedFiles->FindChild(nestedParent.id,u"Empty.txt",file)&&
          mountedFiles->FindChild(nestedParent.id,u"Sibling",sibling)&&sibling.directory&&
          mountedFiles->FindChild(2,u"Empty.txt",otherFile)&&file.id!=otherFile.id&&
          !file.directory&&!file.hardLink&&file.linkCount==1&&file.readError.empty()&&
          file.datesValid&&!file.forks[0].size&&!file.forks[1].size,
          "The mixed APFS catalog did not reopen with the expected empty-file metadata");
    {
        const auto path=VolumeCoord::Wide(root+"checkpoint-empty-files.img");
        auto image=CreateFormatOutput(path.c_str(),imageSize);
        image->Write(0,files.durable.data(),files.durable.size());
        image->Keep();
    }
    fprintf(report,"PASS: APFS empty files and mixed catalogs, cross-type name conflicts, and interrupted file creation.\n");
    const auto beforeData=Apfs::Probe(files);
    CheckpointFaultImage dataFiles(files.durable);
    Memory payload(9003);
    for(size_t i=0;i<payload.bytes.size();++i)payload.bytes[i]=uint8_t(i*31+(i>>9));
    Check(Apfs::CreateCatalogFile(dataFiles,*beforeData,childFolder,u"Data.bin",payload,controlJob)==8,
          "APFS could not create a multi-block data fork");
    for(unsigned step=1;step<=dataFiles.event;++step){
        CheckpointFaultImage attempt(files.durable);
        attempt.failAt=step;
        attempt.fault=step==dataFiles.event||step==dataFiles.event-2?Fault::FailedFlush:Fault::TornWrite;
        Reject([&]{Apfs::CreateCatalogFile(attempt,*beforeData,childFolder,u"Data.bin",payload,controlJob);},
               "Injected APFS data-write interruption succeeded");
        const auto mounted=openCheckpoint(attempt);
        MacBrowser::Entry topDir,parentDir,entry;
        Check(reopen(attempt)==7&&mounted->FindChild(2,u"New Folder",topDir)&&
              mounted->FindChild(topDir.id,u"New Folder",parentDir)&&
              mounted->FindChild(parentDir.id,u"Empty.txt",entry)&&
              !mounted->FindChild(parentDir.id,u"Data.bin",entry),
              "Interrupted APFS data write changed the preceding catalog");
    }
    CheckpointFaultImage outOfSpace(files.durable);
    Memory oversized(imageSize);
    Reject([&]{Apfs::CreateCatalogFile(outOfSpace,*beforeData,2,u"Too large",oversized,controlJob);},
           "APFS accepted a fork with no available extent");
    Check(!outOfSpace.event&&outOfSpace.durable==files.durable,"APFS wrote before rejecting an allocation failure");
    const auto afterData=Apfs::Probe(dataFiles);
    Check(Apfs::CreateCatalogDirectory(dataFiles,*afterData,2,u"After Data",controlJob)==9,
          "APFS could not edit a catalog containing allocated file data");
    const auto afterFolder=Apfs::Probe(dataFiles);
    constexpr uint64_t secondParent=23;
    Memory small(27);
    std::fill(small.bytes.begin(),small.bytes.end(),0xc7);
    {
        CheckpointFaultImage cancelled(dataFiles.durable);
        Storage::Job job;
        cancelled.onFlush=[&]{job.cancel();};
        bool stopped=false;
        try{Apfs::CreateCatalogFile(cancelled,*afterFolder,secondParent,u"Small.bin",small,job);}
        catch(const Storage::Cancelled&){stopped=true;}
        const auto mounted=openCheckpoint(cancelled);
        MacBrowser::Entry topDir,parentDir,entry,destination;
        Check(stopped&&reopen(cancelled)==9&&mounted->FindChild(2,u"After Data",destination)&&
              mounted->List(destination.id).empty()&&mounted->FindChild(2,u"New Folder",topDir)&&
              mounted->FindChild(topDir.id,u"New Folder",parentDir)&&mounted->FindChild(parentDir.id,u"Data.bin",entry),
              "APFS cancellation after the data flush changed the previous catalog");
        Bytes restored(payload.bytes.size());
        mounted->Read(entry.id,0,0,restored.data(),restored.size());
        Check(restored==payload.bytes,"Cancelled APFS file creation changed existing file data");
    }
    Check(Apfs::CreateCatalogFile(dataFiles,*afterFolder,secondParent,u"Small.bin",small,controlJob)==10,
          "APFS could not add a second physical extent reference");
    const auto withData=openCheckpoint(dataFiles);
    MacBrowser::Entry dataTop,dataParent,dataEntry,secondDir,smallEntry;
    Check(withData->FindChild(2,u"New Folder",dataTop)&&
          withData->FindChild(dataTop.id,u"New Folder",dataParent)&&
          withData->FindChild(dataParent.id,u"Data.bin",dataEntry)&&
          withData->FindChild(2,u"After Data",secondDir)&&withData->FindChild(secondDir.id,u"Small.bin",smallEntry),
          "APFS files with data did not reopen after subsequent writes");
    Check(!dataEntry.directory&&dataEntry.forks[0].size==payload.Size()&&!dataEntry.forks[1].size&&
          smallEntry.forks[0].size==small.Size(),"Incorrect APFS data-fork sizes");
    Bytes decoded(payload.bytes.size()),decodedSmall(small.bytes.size());
    withData->Read(dataEntry.id,0,0,decoded.data(),decoded.size());
    withData->Read(smallEntry.id,0,0,decodedSmall.data(),decodedSmall.size());
    Check(decoded==payload.bytes&&decodedSmall==small.bytes,"APFS extent write did not preserve file contents");
    {
        const auto path=VolumeCoord::Wide(root+"checkpoint-data-files.img");
        auto image=CreateFormatOutput(path.c_str(),imageSize);
        image->Write(0,dataFiles.durable.data(),dataFiles.durable.size());
        image->Keep();
    }
    fprintf(report,"PASS: APFS file data extents, partial final blocks, repeat allocation, insufficient space, and write interruption.\n");
    // Move Small.bin's one-block extent into the middle of the free space.
    // Update both trees and the raw bitmap so the fixture remains valid APFS.
    CheckpointFaultImage fragmentationSeed(original);
    Check(Apfs::CreateCatalogFile(fragmentationSeed,*start,2,u"Small.bin",small,controlJob)==2,
          "APFS could not create the fragmentation fixture's blocking file");
    auto fragmentedBase=fragmentationSeed.durable;
    const auto dataCheckpoint=Apfs::Probe(fragmentationSeed);
    const auto volumeId=dataCheckpoint->volumes.front();
    const Apfs::ObjectMap containerObjects(fragmentationSeed,*dataCheckpoint,dataCheckpoint->objectMap,dataCheckpoint->transaction);
    const auto volumeBlock=containerObjects.Lookup(volumeId);
    Check(volumeBlock.has_value(),"Missing APFS volume for fragmentation fixture");
    const auto volumeBytes=fragmentedBase.data()+*volumeBlock*4096;
    const auto catalogId=Apfs::Le64(volumeBytes+136);
    const Apfs::ObjectMap volumeObjects(fragmentationSeed,*dataCheckpoint,Apfs::Le64(volumeBytes+128),dataCheckpoint->transaction);
    const auto catalogBlock=volumeObjects.Lookup(catalogId);
    Check(catalogBlock.has_value(),"Missing APFS catalog for fragmentation fixture");
    const auto extentBlock=Apfs::Le64(volumeBytes+144);
    const auto nxData=fragmentedBase.data()+dataCheckpoint->checkpointBlock*4096;
    const auto dataMap=fragmentedBase.data()+(Apfs::Le64(nxData+112)+Apfs::Le32(nxData+136))*4096;
    const auto dataSpace=fragmentedBase.data()+Apfs::Le64(dataMap+112)*4096;
    const auto chunk=fragmentedBase.data()+Apfs::Le64(dataSpace+Apfs::Le32(dataSpace+80))*4096;
    auto bitmapData=fragmentedBase.data()+Apfs::Le64(chunk+64)*4096;
    constexpr uint64_t blocker=1024,smallId=16;
    uint64_t oldBlock=0;
    const auto updateTree=[&](uint64_t address,uint32_t flags,auto change){
        auto block=fragmentedBase.data()+address*4096;
        const Apfs::Node node(Bytes(block,block+4096),true,{});
        std::vector<Apfs::WriterDisk::Record> records;
        for(size_t i=0;i<node.Count();++i){
            const auto view=node.At(i);
            records.push_back({Bytes(view.key.data,view.key.data+view.key.size),
                               Bytes(view.value.data,view.value.data+view.value.size)});
            change(records.back());
        }
        const auto encoded=Apfs::WriterDisk::EncodeLeaf(records,flags);
        memcpy(block+32,encoded.data()+32,4096-32);
        Seal(fragmentedBase,address*4096,4096);
    };
    updateTree(*catalogBlock,0x40,[&](auto& record){
        if(Apfs::Le64(record.key.data())!=((uint64_t(8)<<60)|smallId))return;
        oldBlock=Apfs::Le64(record.value.data()+8);
        Little(record.value.data()+8,blocker,8);
    });
    bool referenceMoved=false;
    updateTree(extentBlock,0x50,[&](auto& record){
        if(Apfs::Le64(record.value.data()+8)!=smallId)return;
        Little(record.key.data(),(uint64_t(2)<<60)|blocker,8);
        referenceMoved=true;
    });
    Check(oldBlock&&oldBlock<blocker&&referenceMoved&&
          (bitmapData[oldBlock/8]&(1u<<(oldBlock%8)))&&!(bitmapData[blocker/8]&(1u<<(blocker%8))),
          "Fragmentation fixture could not move its blocking file");
    memcpy(fragmentedBase.data()+blocker*4096,fragmentedBase.data()+oldBlock*4096,4096);
    bitmapData[oldBlock/8]&=uint8_t(~(1u<<(oldBlock%8)));
    bitmapData[blocker/8]|=uint8_t(1u<<(blocker%8));

    // No run on either side of the blocker can cover these 1,100 blocks.
    Memory fragmentedPayload(1100*4096-37);
    for(size_t i=0;i<fragmentedPayload.bytes.size();++i)fragmentedPayload.bytes[i]=uint8_t((i>>11)+(i>>19)+i*13);
    CheckpointFaultImage fragmented(fragmentedBase);
    Check(Apfs::CreateCatalogFile(fragmented,*dataCheckpoint,2,u"Fragmented.bin",fragmentedPayload,controlJob)==3,
          "APFS could not use the free space on both sides of an allocated block");
    {
        CheckpointFaultImage interrupted(fragmentedBase);
        interrupted.failAt=1025; // Beyond the maximum length of any run in this fixture.
        interrupted.fault=Fault::TornWrite;
        Reject([&]{Apfs::CreateCatalogFile(interrupted,*dataCheckpoint,2,u"Fragmented.bin",fragmentedPayload,controlJob);},
               "Injected APFS interruption after crossing an extent boundary succeeded");
        const auto restored=openCheckpoint(interrupted);
        MacBrowser::Entry entry;
        Check(reopen(interrupted)==2&&!restored->FindChild(2,u"Fragmented.bin",entry),
              "APFS exposed a partial multi-extent file after interruption");
    }
    const auto fragmentedCheckpoint=Apfs::Probe(fragmented);
    Check(Apfs::CreateCatalogDirectory(fragmented,*fragmentedCheckpoint,2,u"After Fragment",controlJob)==4,
          "APFS could not validate an existing multi-extent file before a later commit");
    const auto fragmentedVolume=openCheckpoint(fragmented);
    MacBrowser::Entry split,pinFile;
    Check(fragmentedVolume->FindChild(2,u"Fragmented.bin",split)&&split.forks[0].size==fragmentedPayload.Size()&&
          fragmentedVolume->FindChild(2,u"Small.bin",pinFile),
          "APFS fragmented image did not reopen with both files");
    Bytes decodedFragments(fragmentedPayload.bytes.size()),decodedBlocker(small.bytes.size());
    fragmentedVolume->Read(split.id,0,0,decodedFragments.data(),decodedFragments.size());
    fragmentedVolume->Read(pinFile.id,0,0,decodedBlocker.data(),decodedBlocker.size());
    Check(decodedFragments==fragmentedPayload.bytes&&decodedBlocker==small.bytes,
          "APFS fragmented write changed file contents or overwrote the blocking extent");
    {
        const auto path=VolumeCoord::Wide(root+"checkpoint-fragmented-files.img");
        auto image=CreateFormatOutput(path.c_str(),imageSize);
        image->Write(0,fragmented.durable.data(),fragmented.durable.size());
        image->Keep();
    }
    fprintf(report,"PASS: APFS fragmented free-space allocation, cross-extent readback, and later catalog writes.\n");
    CheckpointFaultImage invalidName(original);
    Reject([&]{Apfs::CreateCatalogDirectory(invalidName,*start,2,u"Bad/Name",controlJob);},
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
    Reject([&]{Apfs::CreateCatalogDirectory(wrongAllocation,*start,2,u"Must not exist",controlJob);},
           "The APFS allocator trusted a free bit for a live metadata block");
    Check(!wrongAllocation.event&&wrongAllocation.durable==badBitmap,"Invalid APFS allocation wrote to the image");
    fprintf(report,"PASS: APFS allocation, repeated directory creation, retired block reuse, copy-on-write maps, and interrupted commits.\n");
}

class CancelAfterFlush final:public Source {
    Source& file;
    Storage::Job& job;
public:
    CancelAfterFlush(Source& file,Storage::Job& job):file(file),job(job){}
    uint64_t Size()const override{return file.Size();}
    bool Writable()const override{return true;}
    void Read(uint64_t at,void* data,size_t size)override{file.Read(at,data,size);}
    void Write(uint64_t at,const void* data,size_t size)override{file.Write(at,data,size);}
    void Flush()override{file.Flush();job.cancel();}
};

void MultiChunkWrites(const std::string& root,FILE* report)
{
    constexpr uint64_t chunkBytes=128ull*1024*1024;
    constexpr uint64_t imageSize=chunkBytes+4*1024*1024;
    constexpr uint64_t fileSize=chunkBytes+17;
    class PatternSource final:public Source {
    public:
        static uint8_t Byte(uint64_t offset){return uint8_t((offset>>20)+(offset>>13)+offset*7);}
        uint64_t Size()const override{return fileSize;}
        bool Writable()const override{return false;}
        void Read(uint64_t at,void* output,size_t size)override{
            Check(Disk::Range(at,size,fileSize),"Pattern read outside the APFS input");
            auto data=static_cast<uint8_t*>(output);
            for(size_t i=0;i<size;++i)data[i]=Byte(at+i);
        }
        void Write(uint64_t,const void*,size_t)override{throw Error("Cannot write APFS input");}
        void Flush()override{}
    } pattern;

    const auto path=VolumeCoord::Wide(root+"checkpoint-multichunk.img");
    {
        auto image=CreateFormatOutput(path.c_str(),imageSize);
        Storage::Job job;
        Apfs::Format(*image,u"Multi-chunk fixture",false,job);
        const auto initial=Apfs::Probe(*image);
        Check(initial&&Apfs::CreateCatalogFile(*image,*initial,2,u"Across chunks",pattern,job)==2,
              "APFS did not allocate file data beyond its first chunk");

        const auto checkpoint=Apfs::Probe(*image);
        Memory patternSlice(33);
        std::fill(patternSlice.bytes.begin(),patternSlice.bytes.end(),0xa5);
        {
            Check(checkpoint.has_value(),"Missing APFS checkpoint after writing across chunks");
            Storage::Job cancelled;
            CancelAfterFlush wrapper(*image,cancelled);
            bool stopped=false;
            try{Apfs::CreateCatalogFile(wrapper,*checkpoint,2,u"Cancelled",patternSlice,cancelled);}
            catch(const Storage::Cancelled&){stopped=true;}
            const auto recovered=Apfs::Probe(*image);
            Check(stopped&&recovered&&recovered->transaction==2,
                  "APFS exposed a checkpoint after cancellation following the multi-chunk bitmap flush");
        }
        Check(Apfs::CreateCatalogFile(*image,*checkpoint,2,u"Another",patternSlice,job)==3,
              "APFS could not allocate after reopening a materialized chunk");
        image->Keep();
    }
    const auto mounted=MacBrowser::OpenBrowserImage(path);
    MacBrowser::Entry large,small;
    Check(mounted&&mounted->List().size()==2&&mounted->FindChild(2,u"Across chunks",large)&&large.forks[0].size==fileSize&&
          mounted->FindChild(2,u"Another",small)&&small.forks[0].size==33,
          "APFS multi-chunk files did not reopen with correct sizes");
    Bytes buffer(512*1024);
    for(uint64_t at=0;at<fileSize;){
        const auto size=size_t((std::min)(uint64_t(buffer.size()),fileSize-at));
        mounted->Read(large.id,0,at,buffer.data(),size);
        for(size_t i=0;i<size;++i)
            if(buffer[i]!=PatternSource::Byte(at+i))throw Error("APFS multi-chunk file data changed");
        at+=size;
    }
    mounted->Read(small.id,0,0,buffer.data(),33);
    Check(std::all_of(buffer.begin(),buffer.begin()+33,[](uint8_t byte){return byte==0xa5;}),
          "APFS later write into a materialized chunk lost data");
    fprintf(report,"PASS: APFS multi-chunk allocation, bitmap publication, repeat allocation, and full readback beyond 128 MiB.\n");
}

void MultiCibWrites(const std::string& root,FILE* report)
{
    constexpr auto blockSize=Apfs::WriterDisk::Block;
    constexpr auto secondCibStart=Apfs::WriterDisk::ChunksPerCib*Apfs::WriterDisk::Chunk;
    constexpr auto imageSize=(secondCibStart+Apfs::WriterDisk::Chunk)*blockSize;
    const auto path=VolumeCoord::Wide(root+"checkpoint-multicib.img");
    const auto readCibs=[&](Source& image,const Apfs::Container& checkpoint){
        const auto nx=Disk::Read(image,checkpoint.checkpointBlock*blockSize,blockSize);
        const auto descriptor=Apfs::Le64(nx.data()+112)+Apfs::Le32(nx.data()+136);
        const auto map=Disk::Read(image,descriptor*blockSize,blockSize);
        const auto space=Disk::Read(image,Apfs::Le64(map.data()+112)*blockSize,blockSize);
        const auto offset=Apfs::Le32(space.data()+80);
        Check(Apfs::Le32(space.data()+64)==2&&Disk::Range(offset,16,space.size()),
              "Expected two directly addressed APFS chunk-info blocks");
        return std::array<uint64_t,2>{Apfs::Le64(space.data()+offset),Apfs::Le64(space.data()+offset+8)};
    };

    Memory payload(3*blockSize+11);
    for(size_t i=0;i<payload.bytes.size();++i)payload.bytes[i]=uint8_t(i*13+(i>>7));
    {
        auto image=CreateFormatOutput(path.c_str(),imageSize);
        const auto attributes=GetFileAttributesW(path.c_str());
        Check(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_SPARSE_FILE),
              "The multi-CIB APFS test requires a sparse image file");
        Storage::Job job;
        Apfs::Format(*image,u"Multi-CIB fixture",false,job);
        const auto initial=Apfs::Probe(*image);
        Check(initial.has_value(),"Missing initial APFS checkpoint on the multi-CIB fixture");
        const auto oldCibs=readCibs(*image,*initial);
        {
            Storage::Job cancelled;
            CancelAfterFlush wrapper(*image,cancelled);
            bool stopped=false;
            try{Apfs::CreateCatalogFile(wrapper,*initial,2,u"Second CIB",payload,cancelled);}
            catch(const Storage::Cancelled&){stopped=true;}
            const auto recovered=Apfs::Probe(*image);
            Check(stopped&&recovered&&recovered->transaction==1&&readCibs(*image,*recovered)==oldCibs,
                  "Cancelled APFS write changed the selected chunk-info blocks");
        }
        Check(Apfs::CreateCatalogFile(*image,*initial,2,u"Second CIB",payload,job)==2,
              "APFS could not write across two chunk-info blocks");
        const auto written=Apfs::Probe(*image);
        Check(written.has_value(),"Missing APFS checkpoint after the multi-CIB write");
        const auto newCibs=readCibs(*image,*written);
        Check(newCibs[0]!=oldCibs[0]&&newCibs[1]!=oldCibs[1],
              "APFS did not publish replacement CIBs for both modified regions");
        const auto second=Disk::Read(*image,newCibs[1]*blockSize,blockSize);
        const auto bitmap=Apfs::Le64(second.data()+64);
        Check(Apfs::Le64(second.data()+48)==secondCibStart&&bitmap&&
              Apfs::Le32(second.data()+56)-Apfs::Le32(second.data()+60)==4,
              "APFS did not materialize the later chunk's data bitmap");
        const auto raw=Disk::Read(*image,secondCibStart*blockSize,payload.bytes.size());
        Check(raw==payload.bytes,"APFS payload did not land in the chunk described by the second CIB");

        Check(Apfs::CreateCatalogDirectory(*image,*written,2,u"After write",job)==3,
              "APFS could not commit after reopening multiple live chunk-info blocks");
        const auto next=Apfs::Probe(*image);
        Check(next.has_value(),"Missing APFS checkpoint after the subsequent commit");
        const auto nextCibs=readCibs(*image,*next);
        Check(nextCibs[0]!=newCibs[0]&&nextCibs[1]==newCibs[1],
              "APFS replaced an untouched chunk-info block");
        image->Keep();
    }
    const auto mounted=MacBrowser::OpenBrowserImage(path);
    MacBrowser::Entry file,folder;
    Check(mounted&&mounted->FindChild(2,u"Second CIB",file)&&file.forks[0].size==payload.Size()&&
          mounted->FindChild(2,u"After write",folder)&&folder.directory,
          "The APFS browser did not reopen both multi-CIB commits");
    Bytes readback(payload.bytes.size());
    mounted->Read(file.id,0,0,readback.data(),readback.size());
    Check(readback==payload.bytes,"APFS file contents changed after rotating chunk-info blocks");
    fprintf(report,"PASS: APFS multiple chunk-info blocks, second-CIB allocation, and reuse of unchanged CIBs.\n");
}

std::u16string SplitFixtureName(size_t index)
{
    std::u16string name=u"Catalog entry ";
    name+=char16_t(u'A'+index/26);
    name+=char16_t(u'A'+index%26);
    name+=std::u16string(90,char16_t(u'a'+index%26));
    return name;
}

void SplitCatalogWrites(const std::string& root,FILE* report)
{
    const auto path=VolumeCoord::Wide(root+"checkpoint-split-catalog.img");
    auto image=CreateFormatOutput(path.c_str(),8*1024*1024);
    Storage::Job job;
    Apfs::Format(*image,u"Split catalog fixture",false,job);
    const auto initial=Disk::Read(*image,0,size_t(image->Size()));
    CheckpointFaultImage disk(initial);
    const auto catalogLevel=[](Source& source,const Apfs::Container& checkpoint){
        const auto volumeId=checkpoint.volumes.front();
        const Apfs::ObjectMap containerMap(source,checkpoint,checkpoint.objectMap,checkpoint.transaction);
        const auto volumeAddress=containerMap.Lookup(volumeId);
        Check(volumeAddress.has_value(),"Missing APFS test volume mapping");
        const auto volume=Apfs::ReadObject(source,checkpoint,*volumeAddress,volumeId,checkpoint.transaction,13);
        const auto rootId=Apfs::Le64(volume.data()+136);
        const Apfs::ObjectMap volumeMap(source,checkpoint,Apfs::Le64(volume.data()+128),checkpoint.transaction);
        const auto address=volumeMap.Lookup(rootId);
        Check(address.has_value(),"Missing APFS test catalog mapping");
        const Apfs::Node node(Apfs::ReadObject(source,checkpoint,*address,rootId,checkpoint.transaction,2,14),true,{});
        return node.Level();
    };
    const auto open=[](const CheckpointFaultImage& candidate){
        auto memory=std::make_unique<Memory>(candidate.durable.size());
        memory->bytes=candidate.durable;
        std::unique_ptr<Source> source=std::move(memory);
        return Apfs::Open(source,"apfs-split-catalog");
    };
    Memory payload(4096+11);
    for(size_t i=0;i<payload.bytes.size();++i)payload.bytes[i]=uint8_t((i>>8)+i*11);
    const auto add=[&](Source& target,const Apfs::Container& checkpoint,size_t index,const std::u16string& name){
        if(index%5==0)return Apfs::CreateCatalogFile(target,checkpoint,2,name,payload,job);
        return Apfs::CreateCatalogDirectory(target,checkpoint,2,name,job);
    };

    std::vector<std::u16string> names;
    bool split=false;
    for(size_t i=0;i<40;++i){
        auto name=SplitFixtureName(i);
        const auto checkpoint=Apfs::Probe(disk);
        Check(checkpoint.has_value(),"Missing APFS checkpoint before catalog insertion");
        Bytes previous;
        if(!split)previous=disk.durable;
        const auto firstEvent=disk.event;
        Check(add(disk,*checkpoint,i,name)==checkpoint->transaction+1,"APFS catalog insertion failed");
        const auto current=Apfs::Probe(disk);
        Check(current.has_value(),"Missing APFS checkpoint after catalog insertion");
        if(!split&&catalogLevel(disk,*current)){
            split=true;
            const auto events=disk.event-firstEvent;
            for(unsigned step=1;step<=events;++step){
                CheckpointFaultImage attempt(previous);
                attempt.failAt=step;
                attempt.fault=step==events||step==events-2?
                    CheckpointFaultImage::Fault::FailedFlush:CheckpointFaultImage::Fault::TornWrite;
                Reject([&]{add(attempt,*checkpoint,i,name);},"Injected APFS split interruption succeeded");
                Memory durable(attempt.durable.size());
                durable.bytes=attempt.durable;
                const auto selected=Apfs::Probe(durable);
                const auto old=open(attempt);
                MacBrowser::Entry entry;
                const auto oldLevel=selected?catalogLevel(durable,*selected):UINT_MAX;
                const auto oldEntries=old?old->List().size():SIZE_MAX;
                const auto appeared=old&&old->FindChild(2,name,entry);
                Check(selected&&selected->transaction==checkpoint->transaction&&oldLevel==0&&oldEntries==i&&!appeared,
                      ("Interrupted APFS catalog split at step "+std::to_string(step)+
                      " of "+std::to_string(events)+" selected transaction "+
                      std::to_string(selected?selected->transaction:0)+", expected "+
                      std::to_string(checkpoint->transaction)+", level "+std::to_string(oldLevel)+
                      ", "+std::to_string(oldEntries)+" records, expected "+std::to_string(i)).c_str());
            }
        }
        names.push_back(std::move(name));
    }
    const auto mounted=open(disk);
    Check(split&&mounted&&mounted->List().size()==names.size(),"The APFS split catalog did not reopen");
    for(size_t i=0;i<names.size();++i){
        MacBrowser::Entry entry;
        const bool isFile=i%5==0;
        Check(mounted->FindChild(2,names[i],entry)&&entry.directory!=isFile,"An APFS leaf lost a catalog entry");
        if(isFile){
            Bytes readback(payload.bytes.size());
            mounted->Read(entry.id,0,0,readback.data(),readback.size());
            Check(readback==payload.bytes,"APFS file data changed after a catalog split");
        }
    }
    image->Write(0,disk.durable.data(),disk.durable.size());
    image->Flush();
    image->Keep();
    fprintf(report,"PASS: APFS catalog growth past one leaf, cross-leaf lookup and data, and interrupted splits.\n");
}

void RenameCatalogWrites(const std::string& root,FILE* report)
{
    auto input=VolumeCoord::Wide(root+"checkpoint-split-catalog.img");
    if(const auto shared=_wgetenv(L"HFV_APFS_RENAME_BASE"))input=shared;
    auto image=OpenImage(input.c_str(),false);
    CheckpointFaultImage disk(Disk::Read(*image,0,size_t(image->Size())));
    Storage::Job job;
    const auto open=[](const CheckpointFaultImage& disk){
        auto memory=std::make_unique<Memory>(disk.durable.size());
        memory->bytes=disk.durable;
        std::unique_ptr<Source> source=std::move(memory);
        auto mounted=Apfs::Open(source,"apfs-rename-catalog");
        Check(bool(mounted),"Could not reopen the APFS rename fixture");
        return mounted;
    };
    const auto before=open(disk);
    const auto oldFile=SplitFixtureName(0),oldFolder=SplitFixtureName(1);
    constexpr uint64_t rootId=2,fileInode=16,folderInode=17;
    MacBrowser::Entry oldEntry;
    Check(before->FindChild(2,oldFile,oldEntry)&&!oldEntry.directory,"APFS rename fixture has no source file");
    Bytes contents(size_t(oldEntry.forks[0].size));
    before->Read(oldEntry.id,0,0,contents.data(),contents.size());
    const auto start=Apfs::Probe(disk);
    Check(start.has_value(),"No APFS rename checkpoint");
    Reject([&]{Apfs::RenameEntry(disk,*start,rootId,fileInode,oldFolder,job);},
           "APFS rename replaced a folder with a conflicting name");
    Reject([&]{Apfs::RenameEntry(disk,*start,folderInode,fileInode,u"Wrong parent",job);},
           "APFS rename accepted an entry from the wrong parent");
    Reject([&]{Apfs::RenameEntry(disk,*start,rootId,rootId,u"Root",job);},
           "APFS rename accepted the reserved root inode");
    Reject([&]{Apfs::RenameEntry(disk,*start,rootId,fileInode,u"Bad/Name",job);},
           "APFS rename accepted an invalid name");
    Check(!disk.event,"Rejected APFS rename wrote to the image");

    const auto original=disk.durable;
    const auto afterFile=Apfs::RenameEntry(disk,*start,rootId,fileInode,u"Renamed File",job);
    Check(afterFile==start->transaction+1,"APFS file rename did not publish a checkpoint");
    for(unsigned step=1;step<=disk.event;++step){
        CheckpointFaultImage interrupted(original);
        interrupted.failAt=step;
        interrupted.fault=step==disk.event||step==disk.event-2?
            CheckpointFaultImage::Fault::FailedFlush:CheckpointFaultImage::Fault::TornWrite;
        Reject([&]{Apfs::RenameEntry(interrupted,*start,rootId,fileInode,u"Renamed File",job);},
               "Injected APFS rename interruption succeeded");
        const auto previous=open(interrupted);
        MacBrowser::Entry entry;
        Check(!previous->FindChild(2,u"Renamed File",entry)&&previous->FindChild(2,oldFile,entry),
              "Interrupted APFS rename did not preserve the old name");
        Bytes bytes(contents.size());
        previous->Read(entry.id,0,0,bytes.data(),bytes.size());
        Check(bytes==contents,"Interrupted APFS rename changed the file data");
    }
    Reject([&]{Apfs::RenameEntry(disk,*start,rootId,fileInode,u"Stale",job);},
           "APFS rename accepted a stale checkpoint");
    const auto current=Apfs::Probe(disk);
    Check(current&&Apfs::RenameEntry(disk,*current,rootId,fileInode,u"renamed file",job)==afterFile+1,
          "APFS could not change only the case of a name");

    const auto renamed=Apfs::Probe(disk);
    Check(renamed&&Apfs::CreateEmptyFile(disk,*renamed,folderInode,u"Child",job)==afterFile+2,
          "Could not add a child to the APFS rename test folder");
    const auto withChild=Apfs::Probe(disk);
    Check(withChild&&Apfs::RenameEntry(disk,*withChild,rootId,folderInode,u"Renamed Folder",job)==afterFile+3,
          "APFS could not rename a populated folder");
    const auto mounted=open(disk);
    MacBrowser::Entry file,folder,child,unused;
    Check(mounted->List().size()==before->List().size()&&
          !mounted->FindChild(2,oldFile,unused)&&!mounted->FindChild(2,oldFolder,unused)&&
          mounted->FindChild(2,u"renamed file",file)&&file.name==u"renamed file"&&
          file.created==oldEntry.created&&file.modified==oldEntry.modified&&file.forks[0].size==contents.size()&&
          mounted->FindChild(2,u"Renamed Folder",folder)&&folder.directory&&
          mounted->FindChild(folder.id,u"Child",child)&&!child.directory,
          "APFS rename lost file metadata, kept an old name, or disconnected the folder contents");
    Bytes data(contents.size());
    mounted->Read(file.id,0,0,data.data(),data.size());
    Check(data==contents,"APFS rename changed a live file extent");

    {
        const auto output=VolumeCoord::Wide(root+"checkpoint-renamed-catalog.img");
        auto result=CreateFormatOutput(output.c_str(),disk.durable.size());
        result->Write(0,disk.durable.data(),disk.durable.size());
        result->Keep();
    }
    fprintf(report,"PASS: APFS file and populated-folder rename, case-only names, collisions, stale requests and interrupted commits.\n");
}

void DeleteCatalogWrites(const std::string& root,FILE* report)
{
    auto input=VolumeCoord::Wide(root+"checkpoint-fragmented-files.img");
    if(const auto shared=_wgetenv(L"HFV_APFS_DELETE_BASE"))input=shared;
    auto image=OpenImage(input.c_str(),false);
    CheckpointFaultImage disk(Disk::Read(*image,0,size_t(image->Size())));
    const auto open=[](const CheckpointFaultImage& candidate){
        auto memory=std::make_unique<Memory>(candidate.durable.size());
        memory->bytes=candidate.durable;
        std::unique_ptr<Source> source=std::move(memory);
        auto mounted=Apfs::Open(source,"apfs-delete-catalog");
        Check(bool(mounted),"Could not reopen the APFS delete fixture");
        return mounted;
    };
    const auto save=[&](const std::string& filename){
        const auto output=VolumeCoord::Wide(root+filename);
        auto result=CreateFormatOutput(output.c_str(),disk.durable.size());
        result->Write(0,disk.durable.data(),disk.durable.size());
        result->Keep();
    };
    constexpr uint64_t rootId=2,fileInode=17,folderInode=18,childInode=19;
    Storage::Job job;
    const auto before=open(disk);
    MacBrowser::Entry file,pin;
    Check(before->FindChild(2,u"Fragmented.bin",file)&&before->FindChild(2,u"Small.bin",pin),
          "APFS delete fixture is missing its fragmented or blocking file");
    Bytes contents(size_t(file.forks[0].size)),pinContents(size_t(pin.forks[0].size));
    before->Read(file.id,0,0,contents.data(),contents.size());
    before->Read(pin.id,0,0,pinContents.data(),pinContents.size());

    const auto start=Apfs::Probe(disk);
    Check(start.has_value(),"No APFS delete checkpoint");
    Check(Apfs::CreateEmptyFile(disk,*start,folderInode,u"Child",job)==start->transaction+1,
          "Could not populate the APFS delete test folder");
    const auto withChild=Apfs::Probe(disk);
    Check(withChild.has_value(),"No APFS checkpoint for the populated folder");
    const auto untouched=disk.event;
    Reject([&]{Apfs::DeleteEntry(disk,*withChild,rootId,folderInode,job);},"APFS deleted a nonempty folder");
    Reject([&]{Apfs::DeleteEntry(disk,*withChild,folderInode,fileInode,job);},"APFS deleted an entry from the wrong parent");
    Reject([&]{Apfs::DeleteEntry(disk,*withChild,rootId,rootId,job);},"APFS deleted a reserved inode");
    Reject([&]{Apfs::DeleteEntry(disk,*start,rootId,fileInode,job);},"APFS deleted an entry using a stale checkpoint");
    Check(disk.event==untouched,"Rejected APFS delete wrote to the image");

    Memory replacement(contents.size());
    for(size_t i=0;i<replacement.bytes.size();++i)replacement.bytes[i]=uint8_t(contents[i]^0x53);
    Reject([&]{Apfs::CreateCatalogFile(disk,*withChild,rootId,u"Replacement",replacement,job);},
           "APFS delete test did not exhaust free space before deletion");
    Reject([&]{Apfs::ReplaceFileData(disk,*withChild,rootId,fileInode,replacement,job);},
           "APFS data replacement reused live extents when the image was nearly full");
    Check(disk.event==untouched,"Rejected APFS allocation wrote to the image");

    const auto original=disk.durable;
    Check(Apfs::DeleteEntry(disk,*withChild,rootId,fileInode,job)==withChild->transaction+1,
          "APFS could not delete a fragmented file");
    const auto events=disk.event-untouched;
    for(unsigned step=1;step<=events;++step){
        CheckpointFaultImage interrupted(original);
        interrupted.failAt=step;
        interrupted.fault=step==events||step==events-2?
            CheckpointFaultImage::Fault::FailedFlush:CheckpointFaultImage::Fault::TornWrite;
        Reject([&]{Apfs::DeleteEntry(interrupted,*withChild,rootId,fileInode,job);},
               "Injected APFS delete interruption succeeded");
        const auto previous=open(interrupted);
        Check(previous->FindChild(2,u"Fragmented.bin",file),"Interrupted APFS delete lost the file");
        Bytes bytes(contents.size());
        previous->Read(file.id,0,0,bytes.data(),bytes.size());
        Check(bytes==contents,"Interrupted APFS delete changed the file data");
    }
    const auto deleted=open(disk);
    Check(!deleted->FindChild(2,u"Fragmented.bin",file)&&deleted->FindChild(2,u"Small.bin",pin),
          "APFS reopened with an incorrect listing after deletion");
    save("checkpoint-deleted-extents.img");

    const auto afterDelete=Apfs::Probe(disk);
    Check(afterDelete&&Apfs::CreateCatalogFile(disk,*afterDelete,rootId,u"Replacement",replacement,job)==afterDelete->transaction+1,
          "APFS did not reclaim the retired file extents on the next transaction");
    const auto afterReuse=Apfs::Probe(disk);
    Check(afterReuse&&Apfs::DeleteEntry(disk,*afterReuse,folderInode,childInode,job)==afterReuse->transaction+1,
          "APFS could not delete an empty file");
    const auto afterChild=Apfs::Probe(disk);
    Check(afterChild&&Apfs::DeleteEntry(disk,*afterChild,rootId,folderInode,job)==afterChild->transaction+1,
          "APFS could not delete an empty folder");
    const auto mounted=open(disk);
    Check(mounted->List().size()==2&&mounted->FindChild(2,u"Replacement",file)&&
          file.forks[0].size==replacement.Size()&&mounted->FindChild(2,u"Small.bin",pin),
          "APFS delete and reuse produced an incorrect catalog");
    Bytes data(replacement.bytes.size()),pinData(pinContents.size());
    mounted->Read(file.id,0,0,data.data(),data.size());
    mounted->Read(pin.id,0,0,pinData.data(),pinData.size());
    Check(data==replacement.bytes&&pinData==pinContents,"APFS extent reuse changed live file data");
    save("checkpoint-reused-file-extents.img");
    fprintf(report,"PASS: APFS file and empty-folder deletion, interrupted deletes, and deferred reuse of fragmented extents.\n");
}

void ReplaceCatalogData(const std::string& root,FILE* report)
{
    auto input=VolumeCoord::Wide(root+"checkpoint-renamed-catalog.img");
    if(const auto shared=_wgetenv(L"HFV_APFS_REPLACE_BASE"))input=shared;
    auto image=OpenImage(input.c_str(),false);
    CheckpointFaultImage disk(Disk::Read(*image,0,size_t(image->Size())));
    const auto open=[](const CheckpointFaultImage& candidate){
        auto memory=std::make_unique<Memory>(candidate.durable.size());
        memory->bytes=candidate.durable;
        std::unique_ptr<Source> source=std::move(memory);
        auto mounted=Apfs::Open(source,"apfs-replace-data");
        Check(bool(mounted),"Could not reopen the APFS replacement fixture");
        return mounted;
    };
    const auto save=[&](const std::string& filename){
        const auto output=VolumeCoord::Wide(root+filename);
        auto result=CreateFormatOutput(output.c_str(),disk.durable.size());
        result->Write(0,disk.durable.data(),disk.durable.size());
        result->Keep();
    };
    constexpr uint64_t rootId=2,fileInode=16,folderInode=17;
    const auto before=open(disk);
    MacBrowser::Entry original,neighbor;
    const auto neighborName=SplitFixtureName(5);
    Check(before->FindChild(2,u"renamed file",original)&&before->FindChild(2,neighborName,neighbor),
          "APFS replacement fixture is missing its files");
    Bytes oldData(size_t(original.forks[0].size)),neighborData(size_t(neighbor.forks[0].size));
    before->Read(original.id,0,0,oldData.data(),oldData.size());
    before->Read(neighbor.id,0,0,neighborData.data(),neighborData.size());

    Memory payload(3*4096-41);
    for(size_t i=0;i<payload.bytes.size();++i)payload.bytes[i]=uint8_t((i>>7)+i*17);
    const auto start=Apfs::Probe(disk);
    Check(start.has_value(),"No APFS replacement checkpoint");
    Storage::Job job;
    Reject([&]{Apfs::ReplaceFileData(disk,*start,rootId,folderInode,payload,job);},
           "APFS allowed replacing the data fork of a folder");
    Reject([&]{Apfs::ReplaceFileData(disk,*start,folderInode,fileInode,payload,job);},
           "APFS allowed replacing a file with the wrong parent");
    Check(!disk.event,"Rejected APFS replacement wrote to the image");
    const auto base=disk.durable;
    Check(Apfs::ReplaceFileData(disk,*start,rootId,fileInode,payload,job)==start->transaction+1,
          "APFS did not commit replacement file data");
    const auto events=disk.event;
    for(unsigned step=1;step<=events;++step){
        CheckpointFaultImage interrupted(base);
        interrupted.failAt=step;
        interrupted.fault=step==events||step==events-2?
            CheckpointFaultImage::Fault::FailedFlush:CheckpointFaultImage::Fault::TornWrite;
        Reject([&]{Apfs::ReplaceFileData(interrupted,*start,rootId,fileInode,payload,job);},
               "Injected APFS replacement interruption succeeded");
        const auto previous=open(interrupted);
        MacBrowser::Entry entry;
        Check(previous->FindChild(2,u"renamed file",entry)&&entry.forks[0].size==oldData.size(),
              "Interrupted APFS replacement changed the original file size");
        Bytes data(oldData.size());
        previous->Read(entry.id,0,0,data.data(),data.size());
        Check(data==oldData,"Interrupted APFS replacement overwrote existing data");
    }
    {
        CheckpointFaultImage cancelled(base);
        Storage::Job cancelledJob;
        cancelled.onFlush=[&]{cancelledJob.cancel();};
        bool stopped=false;
        try{Apfs::ReplaceFileData(cancelled,*start,rootId,fileInode,payload,cancelledJob);}
        catch(const Storage::Cancelled&){stopped=true;}
        const auto previous=open(cancelled);
        MacBrowser::Entry entry;
        Check(stopped&&previous->FindChild(2,u"renamed file",entry)&&entry.forks[0].size==oldData.size(),
              "Cancelled APFS replacement published the new fork");
    }
    const auto changed=open(disk);
    MacBrowser::Entry entry;
    Check(changed->FindChild(2,u"renamed file",entry)&&entry.name==original.name&&entry.created==original.created&&
          entry.linkCount==original.linkCount&&entry.forks[0].size==payload.Size(),
          "APFS replacement changed the file identity metadata");
    Bytes check(payload.bytes.size());
    changed->Read(entry.id,0,0,check.data(),check.size());
    Check(check==payload.bytes,"APFS replacement lost new file data");
    save("checkpoint-replaced-data.img");

    Reject([&]{Apfs::ReplaceFileData(disk,*start,rootId,fileInode,payload,job);},
           "APFS data replacement accepted a stale checkpoint");
    const auto afterWrite=Apfs::Probe(disk);
    Memory empty(0);
    Check(afterWrite&&Apfs::ReplaceFileData(disk,*afterWrite,rootId,fileInode,empty,job)==afterWrite->transaction+1,
          "APFS could not truncate a data fork to zero length");
    const auto truncated=open(disk);
    Check(truncated->FindChild(2,u"renamed file",entry)&&!entry.forks[0].size&&entry.created==original.created,
          "APFS truncate did not preserve the file");
    const auto afterTruncate=Apfs::Probe(disk);
    Check(afterTruncate&&Apfs::ReplaceFileData(disk,*afterTruncate,rootId,fileInode,payload,job)==afterTruncate->transaction+1,
          "APFS could not put data into the same inode after truncation");
    const auto reopened=open(disk);
    Check(reopened->List().size()==before->List().size()&&reopened->FindChild(2,u"renamed file",entry)&&
          reopened->FindChild(2,neighborName,neighbor),"APFS refill changed the directory listing");
    Bytes neighborCheck(neighborData.size());
    reopened->Read(entry.id,0,0,check.data(),check.size());
    reopened->Read(neighbor.id,0,0,neighborCheck.data(),neighborCheck.size());
    Check(check==payload.bytes&&neighborCheck==neighborData,"APFS refill changed file contents");
    save("checkpoint-refilled-data.img");
    fprintf(report,"PASS: APFS atomic data replacement, truncation and refill, stale and invalid destinations, and interrupted writes.\n");
}

void WritableNamespace(const std::string& root,FILE* report)
{
    auto baseline=VolumeCoord::Wide(root+"checkpoint-renamed-catalog.img");
    if(const auto shared=_wgetenv(L"HFV_APFS_REPLACE_BASE"))baseline=shared;
    auto original=OpenImage(baseline.c_str(),false);
    Reject([&]{Apfs::OpenWritable(original,"apfs-read-only-fixture");},
           "APFS enabled catalog edits through a read-only handle");
    const auto bytes=Disk::Read(*original,0,size_t(original->Size()));
    if(const auto reference=_wgetenv(L"HFV_APFS_TEST_IMAGE")){
        auto macOS=OpenImage(reference,false);
        std::unique_ptr<Source> writable=std::make_unique<CheckpointFaultImage>(
            Disk::Read(*macOS,0,size_t(macOS->Size())));
        Reject([&]{Apfs::OpenWritable(writable,"apfs-independent-fixture");},
               "APFS allowed writes on an independently formatted volume");
    }

    const auto path=VolumeCoord::Wide(root+"checkpoint-writable-namespace.img");
    {
        auto copy=CreateFormatOutput(path.c_str(),bytes.size());
        copy->Write(0,bytes.data(),bytes.size());
        copy->Flush();
        copy->Keep();
    }
    {
        auto source=OpenImage(path.c_str(),true);
        const auto key=VolumeCoord::ImageKey(path);
        auto mounted=Apfs::OpenWritable(source,key);
        Check(mounted&&mounted->Writable()&&!source,"An exclusive native APFS image did not mount writable");
        Reject([&]{OpenImage(path.c_str(),true);},"A second writer opened the mounted APFS image");
        const auto caps=mounted->Supports();
        Check(caps.writable&&caps.rename&&caps.remove&&caps.createFolders&&caps.create&&!caps.resourceForks&&
              !caps.CanEditAttributes()&&MacBrowser::ExecutionFor(*mounted).processLocal,
              "APFS advertised unsupported write or worker operations");

        MacBrowser::Entry target,folder,child,found;
        Check(mounted->FindChild(2,u"renamed file",target)&&mounted->FindChild(2,u"Renamed Folder",folder)&&
              mounted->FindChild(folder.id,u"Child",child),"APFS writable fixture did not list its existing entries");
        Bytes old(size_t(target.forks[0].size)),actual(old.size());
        mounted->Read(target.id,0,0,old.data(),old.size());
        {
            VolumeCoord::OperationLease lease({{key,VolumeCoord::Mode::Write}});
            mounted->Rename(target.id,2,u"Adapter file");
            const auto renamed=mounted->Stat(target.id);
            Check(renamed.name==u"Adapter file"&&renamed.identity==target.identity,
                  "APFS rename invalidated an existing browser row ID");
            const auto newFolder=mounted->MakeDirectory(2,u"Adapter folder");
            Check(mounted->Stat(newFolder).name==u"Adapter folder"&&mounted->List(newFolder).empty(),
                  "An APFS folder was unavailable after it was created");
            const auto nested=mounted->MakeDirectory(newFolder,u"Nested");
            Reject([&]{mounted->Remove(newFolder);},"The APFS adapter removed a populated folder");
            mounted->Remove(nested);
            mounted->Remove(newFolder);
            Reject([&]{mounted->Stat(nested);},"APFS retained a deleted row ID");
            const auto newId=mounted->MakeDirectory(2,u"Public folder");
            Check(newId!=newFolder&&newId!=nested,"APFS recycled a deleted browser row ID");
        }
        mounted->Read(target.id,0,0,actual.data(),actual.size());
        Check(actual==old&&mounted->FindChild(folder.id,u"Child",found)&&found.id==child.id&&
              mounted->Stat(folder.id).identity==folder.identity,
              "APFS refresh changed file bytes or invalidated an unrelated folder's selection");
    }
    {
        const auto reopened=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry entry;
        Check(reopened&&reopened->Writable()&&reopened->FreeBytes()>0&&
              reopened->FindChild(2,u"Adapter file",entry)&&reopened->FindChild(2,u"Public folder",entry)&&
              !reopened->FindChild(2,u"Adapter folder",entry),
              "The browser did not reopen the supported APFS namespace with write access");
    }
    {
        const auto readHandle=OpenImage(path.c_str(),false);
        const auto reopened=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry entry;
        Check(reopened&&!reopened->Writable()&&!reopened->Supports().createFolders&&
              reopened->FindChild(2,u"Public folder",entry),
              "APFS browser open did not fall back to read-only when a reader held the image");
    }

    const auto faultMount=[&](CheckpointFaultImage*& raw){
        auto fault=std::make_unique<CheckpointFaultImage>(bytes);
        raw=fault.get();
        std::unique_ptr<Source> source=std::move(fault);
        return Apfs::OpenWritable(source,"apfs-adapter-fault-fixture");
    };
    unsigned finalFlush=0;
    {
        CheckpointFaultImage* raw=nullptr;
        const auto control=faultMount(raw);
        control->MakeDirectory(2,u"Uncertain");
        finalFlush=raw->event;
    }
    {
        CheckpointFaultImage* raw=nullptr;
        const auto interrupted=faultMount(raw);
        raw->failAt=finalFlush;
        raw->fault=CheckpointFaultImage::Fault::FailedFlush;
        bool uncertain=false;
        try{interrupted->MakeDirectory(2,u"Uncertain");}
        catch(const Apfs::CheckpointPublicationUncertain&){uncertain=true;}
        Check(uncertain,"The APFS volume adapter hid an uncertain checkpoint");
        Reject([&]{interrupted->List();},"The APFS mount kept using a potentially buffered checkpoint");
        Reject([&]{interrupted->MakeDirectory(2,u"Another");},"An uncertain APFS mount accepted another write");
        std::unique_ptr<Source> durable=std::make_unique<CheckpointFaultImage>(raw->durable);
        const auto recovered=Apfs::OpenWritable(durable,"apfs-adapter-recovered");
        MacBrowser::Entry entry;
        Check(recovered&&!recovered->FindChild(2,u"Uncertain",entry),"APFS reopen left an unpublished folder visible");
    }
    {
        CheckpointFaultImage* raw=nullptr;
        const auto mounted=faultMount(raw);
        MacBrowser::Entry target;
        Check(mounted->FindChild(2,u"renamed file",target),"APFS transfer test could not find its overwrite destination");
        Bytes oldData(size_t(target.forks[0].size)),reopened(oldData.size());
        mounted->Read(target.id,0,0,oldData.data(),oldData.size());
        auto replacement=target;
        replacement.forks[0].size=8173;
        replacement.expectedDestinationIdentity=target.identity;
        Storage::Job cancelled;
        raw->onFlush=[&]{cancelled.cancel();};
        bool stopped=false;
        try{
            mounted->PutForTransfer(2,replacement,[](unsigned,uint64_t,void* buffer,size_t size){
                memset(buffer,0xab,size);
            },true,cancelled);
        }catch(const Storage::Cancelled&){stopped=true;}
        Check(stopped&&mounted->Stat(target.id).forks[0].size==oldData.size(),
              "APFS transfer did not honour cancellation after its first flush");
        mounted->Read(target.id,0,0,reopened.data(),reopened.size());
        Check(reopened==oldData,"Late APFS transfer cancellation changed the existing file");
        raw->onFlush={};
        bool readFailed=false;
        Reject([&]{
            mounted->Put(2,replacement,[&](unsigned,uint64_t at,void* buffer,size_t size){
                if(at){readFailed=true;throw Error("Injected APFS copy-source error");}
                memset(buffer,0xcd,size);
            },true);
        },"APFS transfer published a partial data fork when its source failed");
        mounted->Read(target.id,0,0,reopened.data(),reopened.size());
        Check(readFailed&&reopened==oldData,"APFS source read failure changed existing data");
        bool nestedRefused=false;
        Reject([&]{
            mounted->Put(2,replacement,[&](unsigned,uint64_t,void*,size_t){
                try{mounted->MakeDirectory(2,u"Nested write");}
                catch(const Error&){nestedRefused=true;throw;}
            },true);
        },"APFS transfer allowed a nested catalog write from its reader");
        MacBrowser::Entry unused;
        Check(nestedRefused&&!mounted->FindChild(2,u"Nested write",unused),"APFS nested write changed the catalog");
        for(const auto withResource:{false,true}){
            auto unsupported=replacement;
            if(withResource)unsupported.forks[1].size=1;
            else unsupported.version=1;
            const auto before=raw->event;
            Reject([&]{mounted->Put(2,unsupported,[](unsigned,uint64_t,void*,size_t){},true);},
                   "APFS transfer accepted metadata it cannot preserve");
            Check(raw->event==before,"APFS rejected metadata after starting to write");
        }
    }
    fprintf(report,"PASS: APFS writable shared-volume adapter, exclusive images, stable row IDs, namespace edits, uncertain flush recovery, and late transfer cancellation.\n");
}

void DirectoryHashCollisions(const std::string& root,FILE* report)
{
    const auto path=VolumeCoord::Wide(root+"checkpoint-hash-collision.img");
    const std::u16string first=u"Collision 1hvkudj",second=u"Collision 1ey4qx3";
    Check(Apfs::WriterDisk::DirectoryHash("Collision 1hvkudj",true)==
          Apfs::WriterDisk::DirectoryHash("Collision 1ey4qx3",true),"APFS fixture names no longer collide");
    {
        auto image=CreateFormatOutput(path.c_str(),8*1024*1024);
        Storage::Job job;
        Apfs::Format(*image,u"Hash collision",false,job);
        auto checkpoint=Apfs::Probe(*image);
        Apfs::CreateEmptyFile(*image,*checkpoint,2,first,job);
        checkpoint=Apfs::Probe(*image);
        Apfs::CreateCatalogDirectory(*image,*checkpoint,2,second,job);
        checkpoint=Apfs::Probe(*image);
        Reject([&]{Apfs::CreateEmptyFile(*image,*checkpoint,2,u"COLLISION 1HVKUDJ",job);},
               "APFS accepted a duplicate case-insensitive name in an equal-hash group");
        Reject([&]{Apfs::RenameEntry(*image,*checkpoint,2,17,first,job);},
               "APFS rename replaced a distinct entry with the same hash");
        Apfs::RenameEntry(*image,*checkpoint,2,17,u"COLLISION 1EY4QX3",job);
        image->Keep();
    }
    const auto opened=MacBrowser::OpenBrowserImage(path);
    MacBrowser::Entry file,folder;
    Check(opened&&opened->List().size()==2&&opened->FindChild(2,first,file)&&!file.directory&&
          opened->FindChild(2,second,folder)&&folder.directory&&folder.name==u"COLLISION 1EY4QX3",
          "APFS equal-hash directory records did not reopen as separate entries");
    fprintf(report,"PASS: distinct APFS names with the same hash, duplicate protection, and case-only rename.\n");
}

void FinderMetadataWrites(const std::string& root,FILE* report)
{
    const auto path=VolumeCoord::Wide(root+"apfs-finder-info.img");
    {
        auto image=CreateFormatOutput(path.c_str(),8*1024*1024);
        Storage::Job job;
        Apfs::Format(*image,u"Finder metadata",false,job);
        image->Keep();
    }

    MacBrowser::Entry file,folder;
    file.name=u"Document";
    std::copy_n("TEXTttxt",8,file.finder.begin());
    file.forks[0].size=5;
    folder.name=u"Folder";
    folder.directory=true;
    folder.finder[9]=0x08;
    const auto contents=[](unsigned fork,uint64_t offset,void* data,size_t bytes){
        Check(!fork&&offset<=5&&bytes<=5-offset,"Unexpected Finder fixture read");
        memcpy(data,"abcde"+offset,bytes);
    };
    {
        const auto mounted=MacBrowser::OpenBrowserImage(path);
        Check(mounted&&mounted->Writable(),"Could not mount Finder fixture writable");
        VolumeCoord::OperationLease lease({{mounted->ResourceKey(),VolumeCoord::Mode::Write}});
        const auto parent=mounted->MakeDirectoryWithAttributes(2,folder);
        const auto id=mounted->Put(parent,file,contents,false);
        mounted->Rename(parent,2,u"Renamed Folder");
        mounted->Rename(id,parent,u"Renamed Document");
        Check(mounted->Stat(parent).finder==folder.finder&&mounted->Stat(id).finder==file.finder,
              "APFS rename lost Finder information");
        file.name=u"Renamed Document";
        file.finder[9]=0x40;
        Check(mounted->Put(parent,file,contents,true)==id&&mounted->Stat(id).finder==file.finder,
              "APFS overwrite lost Finder information or the destination identity");

        auto temporary=file;
        temporary.name=u"Temporary";
        temporary.forks[0].size=0;
        const auto temporaryId=mounted->Put(parent,temporary,{},false);
        auto cleared=temporary;
        cleared.finder={};
        mounted->Put(parent,cleared,{},true);
        Check(mounted->Stat(temporaryId).finder==cleared.finder,"APFS retained stale Finder information");
        mounted->Remove(temporaryId);
        temporary.name=u"Removed with metadata";
        mounted->Remove(mounted->Put(parent,temporary,{},false));

        const auto destination=mounted->MakeDirectory(2,u"Copy destination");
        Storage::Job job;
        MacBrowser::Entry copiedFolder,copiedFile;
        Check(MacBrowser::Copy(*mounted,parent,*mounted,destination,job)&&
              mounted->FindChild(destination,u"Renamed Folder",copiedFolder)&&copiedFolder.finder==folder.finder&&
              mounted->FindChild(copiedFolder.id,file.name,copiedFile)&&copiedFile.finder==file.finder,
              "Recursive APFS copy lost Finder information");
    }
    {
        const auto mounted=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry foundFolder,foundFile;
        Check(mounted&&mounted->FindChild(2,u"Renamed Folder",foundFolder)&&foundFolder.finder==folder.finder&&
              mounted->FindChild(foundFolder.id,file.name,foundFile)&&foundFile.finder==file.finder&&
              mounted->List(foundFolder.id).size()==1,"APFS Finder information did not survive reopening");
        std::array<char,5> data{};
        mounted->Read(foundFile.id,0,0,data.data(),data.size());
        Check(std::string(data.begin(),data.end())=="abcde","APFS Finder information changed the data fork");
    }
    auto source=OpenImage(path.c_str(),false);
    Bytes bytes(size_t(source->Size()));
    source->Read(0,bytes.data(),bytes.size());
    CheckpointFaultImage fault(bytes);
    fault.fault=CheckpointFaultImage::Fault::TornWrite;
    fault.failAt=1;
    const auto previous=Apfs::Probe(fault)->transaction;
    Storage::Job job;
    Reject([&]{Apfs::CreateCatalogDirectory(fault,*Apfs::Probe(fault),2,u"Interrupted",job,{},&folder.finder);},
           "APFS Finder metadata test did not inject a write failure");
    CheckpointFaultImage reopened(fault.durable);
    Check(Apfs::Probe(reopened)->transaction==previous,"APFS published an interrupted Finder metadata write");
    fprintf(report,"PASS: APFS Finder information for files and folders, rename, overwrite, clearing, and interrupted writes.\n");
}

void UnicodeCatalogNames(const std::string& root,FILE* report)
{
    for(const bool sensitive:{false,true}){
        const auto path=VolumeCoord::Wide(root+(sensitive?"apfs-unicode-sensitive.img":"apfs-unicode.img"));
        const auto spelling=u"Caf\u00e9 \U0001f600";
        const auto decomposed=u"Cafe\u0301 \U0001f600";
        const auto uppercase=u"CAF\u00c9 \U0001f600";
        const auto renamed=u"\uac01 \u65e5\u672c";
        {
            auto image=CreateFormatOutput(path.c_str(),8*1024*1024);
            Storage::Job job;
            Apfs::Format(*image,u"Unicode names",sensitive,job);
            Apfs::CreateEmptyFile(*image,*Apfs::Probe(*image),2,spelling,job);
            const auto checkpoint=Apfs::Probe(*image);
            Reject([&]{Apfs::CreateCatalogDirectory(*image,*checkpoint,2,decomposed,job);},
                   "APFS accepted canonically equivalent names");
            if(sensitive)Apfs::CreateEmptyFile(*image,*checkpoint,2,uppercase,job);
            else Reject([&]{Apfs::CreateEmptyFile(*image,*checkpoint,2,uppercase,job);},
                        "APFS accepted canonically equivalent names with different case");
            Reject([&]{Apfs::CreateEmptyFile(*image,*Apfs::Probe(*image),2,
                                            std::u16string(128,u'\u00e9'),job);},
                   "APFS accepted a name exceeding the UTF-8 byte limit");
            Reject([&]{Apfs::CreateEmptyFile(*image,*Apfs::Probe(*image),2,u"\U0001FAE0",job);},
                   "APFS accepted an unassigned Unicode 9.0 character");
            Apfs::CreateCatalogDirectory(*image,*Apfs::Probe(*image),2,u"\u1e0b\u0323",job);
            image->Keep();
        }
        {
            const auto opened=MacBrowser::OpenBrowserImage(path);
            Check(bool(opened),"Could not open the APFS Unicode fixture");
            opened->ValidateName(spelling);
            Reject([&]{opened->ValidateName(std::u16string(1,char16_t(0xd800)));},
                   "The APFS browser accepted an unpaired surrogate");
            MacBrowser::Entry file,alias,folder;
            Check(opened->Supports().unicodeNames&&opened->FindChild(2,decomposed,file)&&
                  file.name==spelling&&opened->FindChild(2,uppercase,alias)&&
                  ((file.id!=alias.id)==sensitive)&&
                  opened->FindChild(2,u"d\u0323\u0307",folder)&&folder.directory,
                  "APFS Unicode name lookup lost spelling, case mode or combining-mark order");
        }
        auto output=HfsPlus::OpenImage(path.c_str(),true);
        Storage::Job job;
        Apfs::RenameEntry(*output,*Apfs::Probe(*output),2,16,renamed,job);
        output.reset();
        const auto reopened=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry file;
        Check(reopened&&reopened->FindChild(2,u"\u1100\u1161\u11a8 \u65e5\u672c",file)&&file.name==renamed&&
              !reopened->FindChild(2,decomposed,file),"APFS Unicode rename did not survive reopening");
    }
    fprintf(report,"PASS: APFS UTF-8 names, normalization, case modes, invalid names and Unicode rename.\n");
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
        MultiChunkWrites(root,report);
        MultiCibWrites(root,report);
        SplitCatalogWrites(root,report);
        RenameCatalogWrites(root,report);
        DeleteCatalogWrites(root,report);
        ReplaceCatalogData(root,report);
        WritableNamespace(root,report);
        DirectoryHashCollisions(root,report);
        UnicodeCatalogNames(root,report);
        FinderMetadataWrites(root,report);
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
