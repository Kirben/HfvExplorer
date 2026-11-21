// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "ApfsContainer.h"
#include "ApfsCatalogWrite.h"
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

void DataStreamAllocation(FILE* report)
{
    Memory data(4097),empty(0),resource(8193);
    std::vector<Apfs::DataStreamWrite> streams{{data,data.Size()},{empty,empty.Size()},{resource,resource.Size()}};
    const std::vector<Apfs::DataRun> runs{{20,3},{42,2}};
    Apfs::AssignDataRuns(streams,runs);
    Check(streams[0].runs.size()==1&&streams[0].runs[0].block==20&&streams[0].runs[0].count==2&&
          streams[1].runs.empty()&&streams[2].runs.size()==2&&
          streams[2].runs[0].block==22&&streams[2].runs[0].count==1&&
          streams[2].runs[1].block==42&&streams[2].runs[1].count==2,
          "APFS streams shared a partial block or lost a fragmented extent");
    Reject([&]{Apfs::AssignDataRuns(streams,{{20,4}});},"APFS accepted too few blocks for its streams");
    Reject([&]{Apfs::AssignDataRuns(streams,{{20,6}});},"APFS accepted surplus blocks for its streams");
    fprintf(report,"PASS: APFS stream allocation splits runs at independently padded stream boundaries.\n");
}

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
    invalid=original;
    // The reader accepts distinct scopes. The writer's lookup uses only the ID.
    Little(invalid.data()+4096+96,first->volumes.front(),8);
    memcpy(invalid.data()+4096+104,invalid.data()+4096+64,8);
    Seal(invalid,4096,4096);
    CheckpointFaultImage duplicate(invalid);
    bool repeated=false;
    try{Apfs::CommitCheckpoint(duplicate,*first,controlJob);}
    catch(const Error& error){repeated=std::string(error.what())=="Repeated APFS checkpoint object ID";}
    Check(repeated&&!duplicate.event&&duplicate.durable==invalid,
          "Repeated APFS checkpoint object IDs did not fail before writing");
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

void WritableHardLinks(const std::string& root,FILE* report)
{
    const auto baseline=VolumeCoord::Wide(root+"checkpoint-renamed-catalog.img");
    const auto original=OpenImage(baseline.c_str(),false);
    const auto bytes=Disk::Read(*original,0,size_t(original->Size()));
    {
        CheckpointFaultImage interrupted(bytes);
        const auto before=Apfs::Probe(interrupted);
        Check(before.has_value(),"Missing APFS checkpoint for hard-link cancellation");
        Storage::Job job;
        interrupted.onFlush=[&]{job.cancel();};
        bool cancelled=false;
        try{Apfs::CreateCatalogHardLink(interrupted,*before,2,16,u"Cancelled alias",job);}
        catch(const Storage::Cancelled&){cancelled=true;}
        const auto after=Apfs::Probe(interrupted);
        Check(cancelled&&after&&after->transaction==before->transaction,
              "Cancelled APFS hard-link creation published a checkpoint");
    }
    const auto path=VolumeCoord::Wide(root+"apfs-unlinked-hard-links.img");
    {
        auto copy=CreateFormatOutput(path.c_str(),bytes.size());
        copy->Write(0,bytes.data(),bytes.size());
        copy->Flush();
        copy->Keep();
    }
    {
        auto source=OpenImage(path.c_str(),true);
        auto volume=Apfs::OpenWritable(source,VolumeCoord::ImageKey(path));
        MacBrowser::Entry target,folder,alias;
        Check(volume->Supports().CanCreateHardLinks()&&volume->FindChild(2,u"renamed file",target)&&
              volume->FindChild(2,u"Renamed Folder",folder),"Missing APFS hard-link fixture");
        const auto id=volume->MakeHardLink(target.id,folder.id,u"Nested alias");
        Check(volume->FindChild(folder.id,u"Nested alias",alias)&&alias.id==id&&alias.id!=target.id&&
              alias.linkCount==2&&volume->Stat(target.id).linkCount==2&&
              alias.resolvedId==volume->Stat(target.id).resolvedId,"APFS cross-folder alias identity differs");
        const auto second=volume->MakeHardLink(id,2,u"Second alias");
        Check(volume->Stat(second).linkCount==3&&volume->Stat(second).resolvedId==alias.resolvedId,
              "APFS cannot extend a hard-link group through another alias");
        Reject([&]{volume->MakeHardLink(id,2,u"Second alias");},"APFS accepted a duplicate link name");
        Reject([&]{volume->Rename(target.id,2,u"Second alias");},"APFS renamed over another alias of the same inode");
        volume->Rename(id,folder.id,u"Renamed nested alias");
        volume->Rename(second,2,u"Renamed second alias");
        volume->Rename(target.id,2,u"Renamed primary");
        Check(volume->Stat(id).identity==alias.identity&&volume->Stat(target.id).identity==target.identity&&
              volume->Stat(id).linkCount==3&&volume->Stat(second).linkCount==3&&
              !volume->FindChild(folder.id,u"Nested alias",alias)&&
              !volume->FindChild(2,u"Second alias",alias)&&!volume->FindChild(2,u"renamed file",alias),
              "APFS hard-link rename changed identity or left an old name");
        volume->Remove(target.id);
        Reject([&]{volume->Stat(target.id);},"Removed APFS name still resolved");
        Check(!volume->FindChild(2,u"Renamed primary",alias)&&volume->Stat(id).linkCount==2,
              "APFS unlink lost the other names");
        volume->Remove(second);
        Check(volume->FindChild(folder.id,u"Renamed nested alias",alias)&&alias.id==id&&
              alias.linkCount==1&&!alias.hardLink,"APFS did not promote the last file name");
        volume->MakeDirectory(2,u"Unrelated edit");
    }
    {
        const auto volume=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry target,alias,second,folder,unrelated;
        Check(!volume->FindChild(2,u"Renamed primary",target)&&!volume->FindChild(2,u"Renamed second alias",second)&&
              volume->FindChild(2,u"Renamed Folder",folder)&&volume->FindChild(folder.id,u"Renamed nested alias",alias)&&
              volume->FindChild(2,u"Unrelated edit",unrelated)&&alias.linkCount==1&&!alias.hardLink,
              "APFS unlink did not survive reopen");
        const auto before=MacBrowser::OpenBrowserImage(baseline);
        Check(before->FindChild(2,u"renamed file",target)&&target.forks[0].size>0&&
              alias.forks[0].size==target.forks[0].size,"APFS unlink changed the file size");
        Bytes expected(size_t(target.forks[0].size)),actual(expected.size());
        before->Read(target.id,0,0,expected.data(),expected.size());
        volume->Read(alias.id,0,0,actual.data(),actual.size());
        Check(actual==expected,"APFS alias did not share the original file data");
    }
    {
        const auto survivor=OpenImage(path.c_str(),false);
        CheckpointFaultImage disk(Disk::Read(*survivor,0,size_t(survivor->Size())));
        const auto checkpoint=Apfs::Probe(disk);
        Check(checkpoint.has_value(),"Missing APFS checkpoint after unlink");
        Storage::Job job;
        Apfs::DeleteEntry(disk,*checkpoint,17,16,job);
        auto memory=std::make_unique<Memory>(disk.durable.size());
        memory->bytes=std::move(disk.durable);
        std::unique_ptr<Source> source=std::move(memory);
        const auto volume=Apfs::Open(source,"apfs-last-link-deleted");
        MacBrowser::Entry folder,file;
        Check(volume&&volume->FindChild(2,u"Renamed Folder",folder)&&
              !volume->FindChild(folder.id,u"Renamed nested alias",file),
              "APFS failed to delete the final hard-link name");
    }
    fprintf(report,"PASS: APFS hard-link creation, rename, primary promotion, reopen and final deletion.\n");
}

void SharedInodeEdits(const std::string& root,FILE* report)
{
    const auto baseline=VolumeCoord::Wide(root+"checkpoint-renamed-catalog.img");
    const auto path=VolumeCoord::Wide(root+"apfs-shared-inode-edits.img");
    const auto original=OpenImage(baseline.c_str(),false);
    const auto bytes=Disk::Read(*original,0,size_t(original->Size()));
    {
        auto copy=CreateFormatOutput(path.c_str(),bytes.size());
        copy->Write(0,bytes.data(),bytes.size());
        copy->Flush();
        copy->Keep();
    }
    const auto reader=[](unsigned fork,uint64_t at,void* data,size_t size){
        memset(data,fork?'R':'D',size);
    };
    const auto verify=[&](MacBrowser::Volume& volume,const MacBrowser::Entry& file){
        Check(file.linkCount==2&&file.forks[0].size==4500&&file.forks[1].size==5100&&file.finder[0]=='H'&&
              volume.GetAttributes(file.id)==Attributes{{u"user.alias",{3,1,4}}},"APFS aliases disagree on shared file metadata");
        for(unsigned fork=0;fork<2;++fork){
            Bytes actual(size_t(file.forks[fork].size)),expected(actual.size());
            reader(fork,0,expected.data(),expected.size());
            volume.Read(file.id,fork,0,actual.data(),actual.size());
            Check(actual==expected,"APFS alias did not read the updated shared fork");
        }
    };
    {
        auto source=OpenImage(path.c_str(),true);
        const auto volume=Apfs::OpenWritable(source,VolumeCoord::ImageKey(path));
        MacBrowser::Entry original,folder;
        Check(volume&&volume->FindChild(2,u"renamed file",original)&&volume->FindChild(2,u"Renamed Folder",folder),
              "Missing APFS shared inode fixture");
        const auto alias=volume->MakeHardLink(original.id,folder.id,u"Shared alias");
        auto edit=volume->Stat(alias);
        edit.hardLink=false;
        edit.forks[0].size=4500;
        edit.forks[1].size=0;
        Storage::Job job;
        Check(volume->PutForTransfer(folder.id,edit,reader,true,job)==alias,"APFS overwrite replaced the alias identity");
        edit=volume->Stat(alias);
        edit.forks[1].size=5100;
        volume->PutResourceReplacement(alias,edit,reader,{});
        volume->SetAttributes(alias,{{u"user.alias",{3,1,4}}});
        edit=volume->Stat(alias);
        edit.finder[0]='H';
        volume->SetBasicMetadata(alias,edit);
        const auto primary=volume->Stat(original.id);
        const auto secondary=volume->Stat(alias);
        Check(secondary.resolvedId==primary.resolvedId&&secondary.id!=primary.id&&
              primary.identity==original.identity&&secondary.identity==edit.identity,"APFS shared edits changed alias identities");
        verify(*volume,primary);
        verify(*volume,secondary);
    }
    const auto reopened=MacBrowser::OpenBrowserImage(path);
    MacBrowser::Entry originalEntry,folder,alias;
    Check(reopened&&reopened->FindChild(2,u"renamed file",originalEntry)&&reopened->FindChild(2,u"Renamed Folder",folder)&&
          reopened->FindChild(folder.id,u"Shared alias",alias),"APFS shared edits lost a name on reopen");
    verify(*reopened,originalEntry);
    verify(*reopened,alias);
    fprintf(report,"PASS: APFS shared inode data, resource fork, attributes, Finder metadata, cache refresh and reopen.\n");
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
        Check(caps.writable&&caps.rename&&caps.remove&&caps.createFolders&&caps.create&&caps.resourceForks&&
              caps.CanEditAttributes()&&MacBrowser::ExecutionFor(*mounted).processLocal,
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
            if(withResource)unsupported.forks[1].size=UINT64_MAX;
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

void DeepCatalogGrowth(const std::string& root,FILE* report)
{
    const auto path=VolumeCoord::Wide(root+"apfs-large-object-map.img");
    {
        auto disk=CreateFormatOutput(path.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Deep catalog",false,job);
        disk->Keep();
    }
    MacBrowser::Entry entry;
    entry.name=u"Many long attributes";
    for(unsigned index=0;index<2300;++index){
        auto name=u"user."+MacBrowser::U16(std::to_wstring(index));
        name.resize(127,u'x');
        entry.attributes.emplace(std::move(name),Bytes(3804,uint8_t(index)));
    }
    {
        const auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&volume->Writable(),"Could not mount APFS deep catalog image");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        volume->Put(2,entry,{},false);
    }
    {
        auto disk=OpenImage(path.c_str(),false);
        const auto checkpoint=Apfs::Probe(*disk);
        const Apfs::ObjectMap containerMap(*disk,*checkpoint,checkpoint->objectMap,checkpoint->transaction);
        const auto volumeId=checkpoint->volumes.front();
        const auto volumeBlock=containerMap.Lookup(volumeId);
        Check(volumeBlock.has_value(),"Missing deep APFS test volume");
        const auto volume=Apfs::ReadObject(*disk,*checkpoint,*volumeBlock,volumeId,checkpoint->transaction,13);
        const auto treeId=Apfs::Le64(volume.data()+136);
        const auto mapAddress=Apfs::Le64(volume.data()+128);
        const auto mapHeader=Apfs::ReadObject(*disk,*checkpoint,mapAddress,mapAddress,checkpoint->transaction,11,0,true);
        const auto mapRootAddress=Apfs::Le64(mapHeader.data()+48);
        const Apfs::Node mapRoot(Apfs::ReadObject(*disk,*checkpoint,mapRootAddress,mapRootAddress,checkpoint->transaction,2,11,true),true,{});
        Check(mapRoot.Level()==1&&mapRoot.Count()>1,"APFS object map did not grow past one leaf");
        const Apfs::ObjectMap volumeMap(*disk,*checkpoint,mapAddress,checkpoint->transaction);
        const auto treeBlock=volumeMap.Lookup(treeId);
        Check(treeBlock.has_value(),"Missing APFS test catalog");
        const Apfs::Node tree(Apfs::ReadObject(*disk,*checkpoint,*treeBlock,treeId,checkpoint->transaction,2,14),true,{});
        Check(tree.Level()==2,"Long APFS keys did not grow the catalog beyond two levels");
    }
    {
        const auto volume=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry loaded;
        Check(volume&&volume->Writable()&&volume->FindChild(2,entry.name,loaded),"Could not reopen a deep APFS catalog writable");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        Check(volume->GetAttributes(loaded.id)==entry.attributes,"Deep APFS catalog lost long keys or stream data");
        volume->Rename(loaded.id,2,u"Renamed in a deep catalog");
        Check(volume->Stat(loaded.id).name==u"Renamed in a deep catalog","APFS deep catalog rename failed");
    }
    fprintf(report,"PASS: APFS multi-leaf object map and three-level catalog with 2300 long keys, exact readback and later rename.\n");
}

void ExtentReferenceGrowth(const std::string& root,FILE* report)
{
    const auto path=VolumeCoord::Wide(root+"apfs-extent-tree.img");
    {
        auto disk=CreateFormatOutput(path.c_str(),8*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Extent reference tree",false,job);
        disk->Keep();
    }
    const auto level=[&]{
        auto disk=OpenImage(path.c_str(),false);
        const auto checkpoint=Apfs::Probe(*disk);
        const Apfs::ObjectMap map(*disk,*checkpoint,checkpoint->objectMap,checkpoint->transaction);
        const auto oid=checkpoint->volumes.front();
        const auto address=map.Lookup(oid);
        Check(address.has_value(),"Missing APFS test volume");
        const auto volume=Apfs::ReadObject(*disk,*checkpoint,*address,oid,checkpoint->transaction,13);
        const auto extent=Apfs::Le64(volume.data()+144);
        const Apfs::Node rootNode(Apfs::ReadObject(*disk,*checkpoint,extent,extent,checkpoint->transaction,2,15,true),true,{});
        return rootNode.Level();
    };
    MacBrowser::Entry entry;
    entry.name=u"Many streams";
    for(unsigned index=0;index<135;++index){
        const auto name=u"user.stream."+MacBrowser::U16(std::to_wstring(index));
        entry.attributes.emplace(name,Bytes(4001,uint8_t(index)));
    }
    {
        const auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&volume->Writable(),"Could not mount APFS extent test image");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        volume->Put(2,entry,{},false);
    }
    Check(level()==1,"APFS extent references did not grow past one leaf");
    {
        const auto volume=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry loaded;
        Check(volume&&volume->Writable()&&volume->FindChild(2,entry.name,loaded),
              "Could not reopen APFS with an extent reference index");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        Check(volume->GetAttributes(loaded.id)==entry.attributes,"Multi-leaf APFS extent references lost stream data");
        volume->Rename(loaded.id,2,u"Renamed streams");
        volume->SetAttributes(loaded.id,{{u"user.small",{7}}});
    }
    Check(level()==0,"APFS extent references did not shrink after stream removal");
    {
        const auto volume=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry loaded;
        Check(volume&&volume->Writable()&&volume->FindChild(2,u"Renamed streams",loaded),
              "Could not reopen compacted APFS extent references");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        volume->SetAttributes(loaded.id,entry.attributes);
    }
    Check(level()==1,"APFS extent reference tree did not regrow after reclaiming streams");
    {
        auto image=OpenImage(path.c_str(),false);
        Memory invalid(size_t(image->Size()));
        invalid.bytes=Disk::Read(*image,0,invalid.bytes.size());
        const auto checkpoint=Apfs::Probe(invalid);
        const auto oid=checkpoint->volumes.front();
        const auto address=Apfs::ObjectMap(invalid,*checkpoint,checkpoint->objectMap,checkpoint->transaction).Lookup(oid);
        const auto volume=Apfs::ReadObject(invalid,*checkpoint,*address,oid,checkpoint->transaction,13);
        const auto extent=Apfs::Le64(volume.data()+144);
        const Apfs::Node index(Apfs::ReadObject(invalid,*checkpoint,extent,extent,checkpoint->transaction,2,15,true),true,{});
        const auto leaf=Apfs::Le64(index.At(0).value.data)*checkpoint->blockSize;
        auto* node=invalid.bytes.data()+leaf;
        const auto table=56+Apfs::Le16(node+40);
        const auto value=checkpoint->blockSize-Apfs::Le16(node+table+4);
        Little(node+value,uint64_t(1)<<60,8); // Valid extent kind, zero blocks.
        Seal(invalid.bytes,leaf,checkpoint->blockSize);
        bool rejected=false;
        try{Apfs::InspectCatalogLayout(invalid,*checkpoint);}
        catch(const Error& e){rejected=std::string(e.what())=="Invalid or overlapping APFS physical extent range";}
        Check(rejected,"APFS accepted a zero-length physical extent");
    }
    fprintf(report,"PASS: APFS extent-tree growth, reopen, shrink, regrowth and invalid range rejection.\n");
}

void FinderMetadataWrites(const std::string& root,FILE* report)
{
    const auto path=VolumeCoord::Wide(root+"apfs-compressed-copy.img");
    {
        auto image=CreateFormatOutput(path.c_str(),8*1024*1024);
        Storage::Job job;
        Apfs::Format(*image,u"Finder metadata",false,job);
        image->Keep();
    }

    MacBrowser::Entry file,folder;
    file.name=u"Document";
    file.attributesKnown=true;
    file.attributes={{u"user.sample",{1,2,3}},{u"user.\u00e9",HfsPlus::Bytes{}},{u"user.\U0001f600",{0xff,0}}};
    std::copy_n("TEXTttxt",8,file.finder.begin());
    file.forks[0].size=5;
    file.forks[1].size=9003;
    folder.name=u"Folder";
    folder.attributes={{u"user.folder",Bytes(5001,0xa7)}};
    file.attributes.emplace(u"user.large",Bytes(13019,0x71));
    file.attributes.emplace(u"user.second",Bytes(3804,0xf5));
    Bytes acl(68);
    Disk::P32(acl.data(),0x012cc16d);
    Disk::P32(acl.data()+36,1);
    for(unsigned i=0;i<16;++i)acl[44+i]=uint8_t(i+1);
    Disk::P32(acl.data()+60,1);
    Disk::P32(acl.data()+64,2);
    file.attributes.emplace(u"com.apple.system.Security",acl);
    folder.attributes.emplace(u"com.apple.system.Security",acl);
    folder.directory=true;
    folder.finder[9]=0x08;
    const auto contents=[](unsigned fork,uint64_t offset,void* data,size_t bytes){
        if(fork==1){
            Check(offset<=9003&&bytes<=9003-offset,"Unexpected APFS resource read");
            memset(data,'x',bytes);
            return;
        }
        const std::string_view content="abcde";
        Check(fork<2&&offset<=content.size()&&bytes<=content.size()-offset,"Unexpected APFS fixture read");
        memcpy(data,content.data()+offset,bytes);
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
        temporary.forks[1].size=0;
        const auto temporaryId=mounted->Put(parent,temporary,{},false);
        auto cleared=temporary;
        cleared.finder={};
        cleared.attributes.clear();
        mounted->Put(parent,cleared,{},true);
        Check(mounted->Stat(temporaryId).finder==cleared.finder,"APFS retained stale Finder information");
        Check(mounted->GetAttributes(temporaryId).empty(),"APFS overwrite retained stale attributes");
        mounted->Remove(temporaryId);
        temporary.name=u"Removed with metadata";
        mounted->Remove(mounted->Put(parent,temporary,{},false));

        const auto destination=mounted->MakeDirectory(2,u"Copy destination");
        Storage::Job job;
        MacBrowser::Entry copiedFolder,copiedFile;
        auto edited=file.attributes;
        edited.erase(u"user.sample");
        edited.erase(u"user.large");
        edited[u"user.second"]=Bytes(8165,0xa3);
        edited.erase(u"com.apple.system.Security");
        edited[u"user.new"]={9,8,7};
        mounted->SetAttributes(id,edited);
        Check(mounted->Stat(id).forks[1].size==9003,"Editing APFS attributes cleared the resource fork");
        const auto original=mounted->Stat(id);
        auto replacement=original;
        replacement.forks[1].size=2;
        mounted->PutResourceReplacement(id,replacement,[](unsigned fork,uint64_t at,void* bytes,size_t size){
            Check(fork==1&&at==0&&size==2,"Unexpected APFS resource replacement read");
            memcpy(bytes,"RS",size);
        },{});
        std::array<char,2> resource{};
        mounted->Read(id,1,0,resource.data(),resource.size());
        Check(std::string(resource.begin(),resource.end())=="RS"&&mounted->GetAttributes(id)==edited&&
              mounted->Stat(id).forks[0].size==5,"APFS resource replacement changed ordinary attributes or the data fork");
        replacement=mounted->Stat(id);
        replacement.forks[1].size=0;
        mounted->PutResourceReplacement(id,replacement,{},{});
        Check(!mounted->Stat(id).forks[1].size,"APFS resource removal did not clear the fork");
        replacement=mounted->Stat(id);
        replacement.forks[1].size=9003;
        mounted->PutResourceReplacement(id,replacement,contents,{});
        Check(mounted->GetAttributes(id)==edited&&mounted->Stat(id).finder==file.finder&&
              mounted->Stat(id).forks[0].size==5,"APFS attribute editing lost other metadata or file data");
        mounted->SetAttributes(id,file.attributes);
        const auto makePayload=[](unsigned fork,uint64_t,void* bytes,size_t size){
            Check(fork==0,"Unexpected resource read from an uncompressed fixture");
            memset(bytes,'A',size);
        };
        MacBrowser::Entry large;
        large.name=u"Compressed fixture";
        large.forks[0].size=150003;
        large.finder=file.finder;
        large.attributes=file.attributes;
        const auto compressedId=mounted->Put(2,large,makePayload,false);
        using Mode=HfsPlus::Volume::CompressionMode;
        mounted->SetCompressionMethod(compressedId,Mode::Zlib,{});
        Check(mounted->Stat(compressedId).compressed&&mounted->Stat(compressedId).forks[0].size==150003&&
              !mounted->Stat(compressedId).forks[1].size&&mounted->GetAttributes(compressedId)==large.attributes,
              "APFS compression changed logical sizes or ordinary attributes");
        std::array<char,25> tail{};
        mounted->Read(compressedId,0,150003-tail.size(),tail.data(),tail.size());
        Check(std::string(tail.begin(),tail.end())==std::string(tail.size(),'A'),"APFS compressed resource readback differed");
        mounted->SetCompressionMethod(compressedId,Mode::Uncompressed,{});
        Check(!mounted->Stat(compressedId).compressed&&!mounted->Stat(compressedId).forks[1].size,
              "APFS decompression left compression metadata behind");
        mounted->SetCompressionMethod(compressedId,Mode::Zlib,{});
        Reject([&]{mounted->SetCompressionMethod(id,Mode::StoredInline,{});},
               "APFS compressed a file with a user resource fork");
        Check(!mounted->Stat(id).compressed&&mounted->Stat(id).forks[1].size==9003,
              "Rejected APFS compression changed the file or its user resource fork");
        auto inlineFile=file;
        inlineFile.name=u"Inline compressed fixture";
        inlineFile.forks[1].size=0;
        const auto inlineId=mounted->Put(2,inlineFile,contents,false);
        mounted->SetCompressionMethod(inlineId,Mode::StoredInline,{});
        const auto copies=mounted->MakeDirectory(2,u"Compressed copies");
        Storage::Job copyJob;
        MacBrowser::Entry copied;
        Check(MacBrowser::Copy(*mounted,compressedId,*mounted,copies,copyJob)&&
              mounted->FindChild(copies,large.name,copied)&&copied.compressed&&
              copied.forks[0].size==large.forks[0].size&&mounted->GetAttributes(copied.id)==large.attributes,
              "Copying a compressed APFS file lost compression, size or metadata");
        mounted->Read(copied.id,0,150003-tail.size(),tail.data(),tail.size());
        Check(std::string(tail.begin(),tail.end())==std::string(tail.size(),'A'),"Compressed APFS copy changed file data");
        auto overwriteTarget=file;
        overwriteTarget.name=inlineFile.name;
        const auto targetId=mounted->Put(copies,overwriteTarget,contents,false);
        auto compressedSource=mounted->Stat(inlineId);
        compressedSource.attributes=mounted->GetAttributes(inlineId);
        compressedSource.attributesKnown=true;
        const auto readCompressed=[&](unsigned fork,uint64_t offset,void* bytes,size_t size){
            mounted->Read(inlineId,fork,offset,bytes,size);
        };
        Check(mounted->Put(copies,compressedSource,readCompressed,true)==targetId&&
              mounted->Stat(targetId).compressed&&!mounted->Stat(targetId).forks[1].size,
              "Compressed APFS overwrite kept a stale resource fork or lost compression or destination identity");
        mounted->Read(targetId,0,0,tail.data(),5);
        Check(std::string(tail.data(),5)=="abcde","Compressed APFS overwrite changed file contents");
        auto malformed=file.attributes;
        malformed[u"com.apple.system.Security"]=Bytes(67);
        Reject([&]{mounted->SetAttributes(id,malformed);},"APFS accepted a malformed ACL");
        Check(mounted->GetAttributes(id)==file.attributes,"Rejected APFS ACL changed existing attributes");
        Check(MacBrowser::Copy(*mounted,parent,*mounted,destination,job)&&
              mounted->FindChild(destination,u"Renamed Folder",copiedFolder)&&copiedFolder.finder==folder.finder&&
              mounted->FindChild(copiedFolder.id,file.name,copiedFile)&&copiedFile.finder==file.finder,
              "Recursive APFS copy lost Finder information");
        Check(mounted->GetAttributes(copiedFolder.id)==folder.attributes&&
              mounted->GetAttributes(copiedFile.id)==file.attributes,
              "APFS recursive copy lost embedded attributes");
        auto oversized=file;
        oversized.name=u"Oversized attribute";
        oversized.attributes={{std::u16string(128,u'x'),Bytes(3804)}};
        Reject([&]{mounted->Put(parent,oversized,contents,false);},"APFS accepted an oversized attribute name");
    }
    {
        const auto mounted=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry foundFolder,foundFile;
        Check(mounted&&mounted->FindChild(2,u"Renamed Folder",foundFolder)&&foundFolder.finder==folder.finder&&
              mounted->FindChild(foundFolder.id,file.name,foundFile)&&foundFile.finder==file.finder&&
              mounted->List(foundFolder.id).size()==1,"APFS Finder information did not survive reopening");
        Check(mounted->GetAttributes(foundFolder.id)==folder.attributes&&
              mounted->GetAttributes(foundFile.id)==file.attributes,
              "APFS embedded and streamed folder or file attributes did not survive reopening");
        std::array<char,5> data{};
        mounted->Read(foundFile.id,0,0,data.data(),data.size());
        Check(std::string(data.begin(),data.end())=="abcde","APFS Finder information changed the data fork");
        MacBrowser::Entry compressed;
        Check(mounted->FindChild(2,u"Compressed fixture",compressed)&&compressed.compressed,
              "Compressed APFS file did not survive reopening");
        std::array<char,17> compressedBytes{};
        mounted->Read(compressed.id,0,65529,compressedBytes.data(),compressedBytes.size());
        Check(std::string(compressedBytes.begin(),compressedBytes.end())==std::string(compressedBytes.size(),'A'),
              "Reopened APFS compression did not read across units");
        std::array<char,6> fork{};
        mounted->Read(foundFile.id,1,8997,fork.data(),fork.size());
        Check(std::string(fork.begin(),fork.end())=="xxxxxx","APFS streamed resource fork did not survive reopening");
    }
    {
        const auto mounted=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry parent,current;
        Check(mounted&&mounted->Supports().CanEditBasicMetadata()&&
              mounted->FindChild(2,u"Renamed Folder",parent)&&mounted->FindChild(parent.id,file.name,current),
              "Could not open APFS Properties fixture");
        const auto submit=[&](const MacBrowser::Entry& original,const MacBrowser::Entry& desired){
            const auto item=FileOperations::Location::Mounted(mounted,original.id);
            auto operation=FileOperations::Submit(FileOperations::EditBasicMetadata{item,original,desired});
            Check(WaitForSingleObject(operation.Job()->event(),15000)==WAIT_OBJECT_0,"APFS Properties timed out");
            return operation.Job()->result();
        };
        auto desired=current;
        desired.finder[31]=0x5a;
        desired.modified=5000000123ULL;
        const auto saved=submit(current,desired);
        Check(!saved.code,saved.message.c_str());
        const auto fresh=mounted->Stat(current.id);
        Check(fresh.finder==desired.finder&&fresh.identity==current.identity&&
              fresh.created==current.created&&fresh.modified==desired.modified,
              "APFS Properties did not update only the requested metadata");
        auto stale=current;
        stale.finder[31]=0x55;
        Check(submit(current,stale).code&&mounted->Stat(current.id).finder==desired.finder,
              "A stale APFS Properties edit overwrote fresh Finder information");
        auto invalidDate=fresh;
        invalidDate.created=UINT64_MAX;
        Check(submit(fresh,invalidDate).code&&mounted->Stat(current.id).created==fresh.created,
              "APFS Properties accepted an out-of-range date");
        auto forbidden=fresh;
        forbidden.name=u"Unexpected rename";
        Check(submit(fresh,forbidden).code&&mounted->Stat(current.id).name==current.name,
              "APFS Finder editor unexpectedly changed the file name");
        auto desiredFolder=parent;
        desiredFolder.finder={};
        desiredFolder.created=1000000000;
        const auto cleared=submit(parent,desiredFolder);
        Check(!cleared.code&&mounted->Stat(parent.id).finder==desiredFolder.finder&&
              mounted->Stat(parent.id).created==desiredFolder.created&&mounted->Stat(parent.id).modified==parent.modified,
              "APFS folder Properties did not clear Finder information");
        auto lockedFolder=mounted->Stat(parent.id);
        lockedFolder.protectedFile=true;
        Check(!submit(mounted->Stat(parent.id),lockedFolder).code&&mounted->Stat(parent.id).protectedFile,
              "APFS folder did not lock");
        {
            VolumeCoord::OperationLease lease({{mounted->ResourceKey(),VolumeCoord::Mode::Write}});
            Reject([&]{mounted->MakeDirectory(parent.id,u"Blocked");},"APFS created a child in a locked folder");
            Reject([&]{mounted->Rename(parent.id,2,u"Locked folder rename");},"APFS renamed a locked folder");
            Reject([&]{mounted->ValidateRemoval(current.id);},"APFS cut preflight ignored a locked parent");
            Reject([&]{mounted->Rename(current.id,parent.id,u"Locked child rename");},"APFS renamed a child in a locked folder");
            Reject([&]{mounted->Remove(current.id);},"APFS deleted from a locked folder");
            Reject([&]{mounted->Put(parent.id,file,contents,true);},"APFS overwrote in a locked folder");
            Check(mounted->List(parent.id).size()==1,"Rejected operations changed a locked APFS folder");
        }
        auto unlockedFolder=mounted->Stat(parent.id);
        unlockedFolder.protectedFile=false;
        Check(!submit(lockedFolder,unlockedFolder).code&&!mounted->Stat(parent.id).protectedFile,
              "APFS folder did not unlock");
        {
            VolumeCoord::OperationLease lease({{mounted->ResourceKey(),VolumeCoord::Mode::Write}});
            mounted->Remove(mounted->MakeDirectory(parent.id,u"Unlocked folder child"));
        }
        auto locked=fresh;
        locked.protectedFile=true;
        Check(!submit(fresh,locked).code&&mounted->Stat(fresh.id).protectedFile,"APFS file did not lock");
        {
            VolumeCoord::OperationLease lease({{mounted->ResourceKey(),VolumeCoord::Mode::Write}});
            Reject([&]{mounted->ValidateRemoval(fresh.id);},"APFS cut preflight accepted a locked file");
            Reject([&]{mounted->Rename(fresh.id,parent.id,u"Locked rename");},"APFS renamed a locked file");
            Reject([&]{mounted->Remove(fresh.id);},"APFS deleted a locked file");
            Reject([&]{mounted->Put(parent.id,file,contents,true);},"APFS overwrote a locked file");
        }
        const auto persisted=mounted->Stat(fresh.id);
        auto stillLocked=persisted;
        stillLocked.finder[2]^=1;
        Check(submit(persisted,stillLocked).code,"APFS modified Finder information on a locked file");
        auto unlocked=persisted;
        unlocked.protectedFile=false;
        Check(!submit(persisted,unlocked).code&&!mounted->Stat(fresh.id).protectedFile,"APFS file did not unlock");
        Check(!submit(unlocked,locked).code&&mounted->Stat(fresh.id).protectedFile,"APFS file did not relock");
        {
            VolumeCoord::OperationLease lease({{mounted->ResourceKey(),VolumeCoord::Mode::Write}});
            const auto copiedParent=mounted->MakeDirectory(2,u"Locked file copies");
            Storage::Job job;
            MacBrowser::Entry copy;
            Check(MacBrowser::Copy(*mounted,fresh.id,*mounted,copiedParent,job)&&
                  mounted->FindChild(copiedParent,file.name,copy)&&copy.protectedFile&&copy.finder==locked.finder,
                  "Ordinary APFS copy lost the source lock or Finder information");
            auto replacement=locked;
            replacement.name=u"Replacement";
            const auto id=mounted->Put(copiedParent,replacement,contents,false);
            Check(mounted->Stat(id).protectedFile,"New APFS import lost the source lock");
            auto target=file;
            target.name=u"Overwritten";
            target.forks[0].size=0;
            target.forks[1].size=0;
            const auto overwriteId=mounted->Put(copiedParent,target,{},false);
            replacement.name=target.name;
            Check(mounted->Put(copiedParent,replacement,contents,true)==overwriteId&&
                  mounted->Stat(overwriteId).protectedFile&&mounted->Stat(overwriteId).forks[0].size==5,
                  "APFS overwrite did not publish its contents and lock together");
        }
        {
            VolumeCoord::OperationLease lease({{mounted->ResourceKey(),VolumeCoord::Mode::Write}});
            Reject([&]{mounted->SetAttributes(fresh.id,{});},"APFS edited attributes on a locked file");
        }
        const auto unlockedParent=mounted->Stat(parent.id);
        auto lockedParent=unlockedParent;
        lockedParent.protectedFile=true;
        Check(!submit(unlockedParent,lockedParent).code,"APFS folder did not relock");
        VolumeCoord::OperationLease lease({{mounted->ResourceKey(),VolumeCoord::Mode::Write}});
        const auto destination=mounted->MakeDirectory(2,u"Folder copies");
        Storage::Job folderCopy;
        MacBrowser::Entry copiedFolder,copiedChild;
        Check(MacBrowser::Copy(*mounted,parent.id,*mounted,destination,folderCopy)&&
              mounted->FindChild(destination,lockedParent.name,copiedFolder)&&copiedFolder.protectedFile&&
              copiedFolder.created==lockedParent.created&&copiedFolder.modified==lockedParent.modified&&
              mounted->FindChild(copiedFolder.id,file.name,copiedChild)&&copiedChild.protectedFile&&
              copiedChild.forks[0].size==5,"APFS recursive copy did not preserve locks, dates and contents");
        const auto mergeParent=mounted->MakeDirectory(2,u"Folder merges");
        const auto existing=mounted->MakeDirectory(mergeParent,lockedParent.name);
        Storage::Job merge;
        Check(MacBrowser::Copy(*mounted,parent.id,*mounted,mergeParent,merge)&&
              !mounted->Stat(existing).protectedFile&&mounted->FindChild(existing,file.name,copiedChild),
              "Merging a locked source folder changed the destination lock");
    }
    {
        const auto mounted=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry parent,current;
        Check(mounted&&mounted->FindChild(2,u"Renamed Folder",parent)&&parent.finder==Apfs::FinderBytes{}&&parent.protectedFile&&
              mounted->FindChild(parent.id,file.name,current)&&current.finder[31]==0x5a&&current.modified==5000000123ULL&&current.protectedFile,
              "APFS Properties did not survive reopening");
        std::array<char,5> data{};
        mounted->Read(current.id,0,0,data.data(),data.size());
        Check(std::string(data.begin(),data.end())=="abcde","Finder Properties changed the APFS data fork");
    }
    auto source=OpenImage(path.c_str(),false);
    Bytes bytes(size_t(source->Size()));
    source->Read(0,bytes.data(),bytes.size());
    CheckpointFaultImage fault(bytes);
    fault.fault=CheckpointFaultImage::Fault::TornWrite;
    fault.failAt=1;
    const auto previous=Apfs::Probe(fault)->transaction;
    Storage::Job job;
    CheckpointFaultImage input(Bytes{1,2,3});
    Reject([&]{Apfs::CreateCatalogFile(fault,*Apfs::Probe(fault),2,u"Interrupted",input,job,{},&file.finder,true);},
           "APFS Finder metadata test did not inject a write failure");
    CheckpointFaultImage reopened(fault.durable);
    Check(Apfs::Probe(reopened)->transaction==previous,"APFS published an interrupted locked file import");
    source.reset();
    {
        const auto mounted=MacBrowser::OpenBrowserImage(path);
        MacBrowser::Entry lockedFolder,partial;
        Check(mounted&&mounted->FindChild(2,u"Renamed Folder",lockedFolder),"Missing locked APFS source folder");
        auto image=std::make_unique<CheckpointFaultImage>(bytes);
        auto* raw=image.get();
        std::unique_ptr<Source> targetImage=std::move(image);
        const auto target=Apfs::OpenWritable(targetImage,"apfs-cancelled-folder-copy");
        Check(bool(target),"Could not open the APFS cancellation fixture");
        VolumeCoord::OperationLease lease({{mounted->ResourceKey(),VolumeCoord::Mode::Read},
                                           {target->ResourceKey(),VolumeCoord::Mode::Write}});
        const auto destination=target->MakeDirectory(2,u"Cancelled destination");
        Storage::Job stopped;
        raw->onFlush=[&]{stopped.cancel();};
        bool cancelled=false;
        try{MacBrowser::Copy(*mounted,lockedFolder.id,*target,destination,stopped);}
        catch(const Storage::Cancelled&){cancelled=true;}
        Check(cancelled&&target->FindChild(destination,lockedFolder.name,partial)&&!partial.protectedFile&&
              target->List(partial.id).empty(),"APFS locked a folder whose copy was cancelled");
    }
    fprintf(report,"PASS: APFS Finder information for files and folders, rename, overwrite, clearing, timestamp edits, stale Properties edits, file and folder locks, locked copies and overwrites, and interrupted import.\n");
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
Memory BrowseFixture(bool attributes=false,bool links=false,bool compressed=false,Symlink symlink=Symlink::None,bool badSiblingMap=false)
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
    const unsigned records=8+(attributes?3:0)+(links?13:0)+(compressed?1:0)-(symlink!=Symlink::None?2:0);
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
        const bool hasData=!directory&&symlink==Symlink::None;
        Bytes value(hasData?140:92);
        Little(value.data()+8,id,8);Little(value.data()+56,linked?2:1,4);
        Little(value.data()+80,directory?0040755:symlink!=Symlink::None?0120777:0100644,2);
        if(hasData){
            Little(value.data()+92,1,2);Little(value.data()+94,44,2);
            value[96]=8;Little(value.data()+98,40,2);
            Little(value.data()+100,length,8);
        }
        record(id,3,{},std::move(value));
    };
    auto drec=[&](uint64_t directory,uint64_t id,const char* name,uint64_t sibling=0){
        const auto len=strlen(name)+1;
        Bytes key(4+len),value(sibling?34:18);
        if(sibling){
            Little(value.data()+18,1,2);Little(value.data()+20,8,2);
            value[22]=1;Little(value.data()+24,8,2);Little(value.data()+26,sibling,8);
        }
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
    inode(320,true,0);drec(320,330,"Long \xc3\xa9.txt",links?350:0);
    if(links){drec(320,330,"Alias A",351);drec(320,340,"Alias B",352);drec(320,340,"Other",353);}
    auto siblingLink=[&](uint64_t inode,uint64_t sibling,const char* name){
        const auto length=strlen(name)+1;
        Bytes key(8),value(10+length);
        Little(key.data(),sibling,8);Little(value.data(),320,8);
        Little(value.data()+8,length,2);memcpy(value.data()+10,name,length);
        record(inode,5,std::move(key),std::move(value));
    };
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
    if(links){
        siblingLink(330,350,"Long \xc3\xa9.txt");
        siblingLink(330,351,"Alias A");
    }
    if(symlink==Symlink::None){extent(0,40);extent(size,0);extent(2*size,42);}
    if(attributes){
        extent(0,43,331);
        memcpy(disk.bytes.data()+43*size,"resource",8);
    }
    if(links){
        inode(340,false,1,true);
        siblingLink(340,352,"Alias B");siblingLink(340,353,"Other");
        extent(0,42,340);
        for(uint64_t sibling=350;sibling<=353;++sibling){
            Bytes value(8);
            Little(value.data(),sibling<352&&!(badSiblingMap&&sibling==351)?330:340,8);
            record(sibling,12,{},std::move(value));
        }
    }
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

Memory Gpt(const Memory& raw,unsigned sector,bool readOnly=false)
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
    if(readOnly)Little(table.data()+48,1ULL<<60,8);
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
void ExternalResourceForkWrites(MacBrowser::Volume& volume,uint32_t folder)
{
    MacBrowser::Entry file;
    file.name=u"Separate resource fork";
    file.forks[0].size=8197;
    file.finder[0]='R';
    file.datesValid=true;
    file.created=file.modified=3700000456;
    file.attributes={{u"user.kept",Bytes(4301,0x54)}};
    const Bytes data(8197,0x62),embedded(701,0x46),streamed(10193,0x39);
    const auto id=volume.Put(folder,file,[&](unsigned fork,uint64_t offset,void* bytes,size_t count){
        Check(!fork,"Unexpected resource fork in APFS test input");
        memcpy(bytes,data.data()+offset,count);
    },false);
    const auto original=volume.Stat(id);
    const auto verify=[&](const Bytes& resource,uint64_t modified){
        const auto entry=volume.Stat(id);
        Check(entry.identity==original.identity&&entry.created==original.created&&entry.modified==modified&&
              entry.finder==file.finder&&volume.GetAttributes(id)==file.attributes&&
              entry.forks[0].size==data.size()&&entry.forks[1].size==resource.size(),
              "APFS resource replacement changed unrelated metadata");
        Bytes actual(data.size());
        volume.Read(id,0,0,actual.data(),actual.size());
        Check(actual==data,"APFS resource replacement changed the data fork");
        if(!resource.empty()){
            actual.resize(resource.size());
            volume.Read(id,1,0,actual.data(),actual.size());
            Check(actual==resource,"APFS resource fork did not read back exactly");
        }
    };
    const auto replace=[&](const Bytes& bytes,uint64_t modified,bool cancel=false){
        auto entry=volume.Stat(id);
        entry.forks[1].size=bytes.size();
        entry.modified=modified;
        Storage::Job job;
        volume.PutResourceReplacementForJob(id,entry,[&](unsigned fork,uint64_t offset,void* dest,size_t count){
            Check(fork==1,"APFS resource replacement attempted to rewrite the data fork");
            memcpy(dest,bytes.data()+offset,count);
            if(cancel)job.cancel();
        },{},job);
    };
    replace(embedded,file.modified+1);
    verify(embedded,file.modified+1);
    bool cancelled=false;
    try{replace(streamed,file.modified+2,true);}catch(const Storage::Cancelled&){cancelled=true;}
    Check(cancelled,"APFS did not cancel resource stream replacement");
    verify(embedded,file.modified+1);
    replace(streamed,file.modified+3);
    verify(streamed,file.modified+3);
    replace(embedded,file.modified+4);
    verify(embedded,file.modified+4);
    replace(streamed,file.modified+5);
    replace({},file.modified+6);
    verify({},file.modified+6);
    replace(streamed,file.modified+7);
    const auto beforeAttributes=volume.Stat(id);
    const auto acl=[](unsigned count){
        Bytes bytes(44+24*count);
        Disk::P32(bytes.data(),0x012cc16d);
        Disk::P32(bytes.data()+36,count);
        for(unsigned i=0;i<count;++i){
            bytes[44+24*i]=uint8_t(i+1);
            Disk::P32(bytes.data()+60+24*i,1);
            Disk::P32(bytes.data()+64+24*i,2);
        }
        return bytes;
    };
    auto attributes=file.attributes;
    attributes[u"user.added"]=Bytes{1,2,3};
    attributes[u"com.apple.system.Security"]=acl(1);
    volume.SetAttributes(id,attributes);
    Check(volume.GetAttributes(id)==attributes,"APFS did not add ordinary attributes and ACL");
    auto replacement=attributes;
    replacement[u"user.added"]=Bytes(9503,0x28);
    replacement.erase(u"user.kept");
    replacement[u"com.apple.system.Security"]=acl(128);
    Storage::Job cancelledAttributes;
    cancelledAttributes.cancel();
    bool stopped=false;
    try{volume.SetAttributesForJob(id,replacement,cancelledAttributes);}catch(const Storage::Cancelled&){stopped=true;}
    Check(stopped&&volume.GetAttributes(id)==attributes,"Cancelled APFS edit changed attributes or ACL");
    volume.SetAttributes(id,replacement);
    Check(volume.GetAttributes(id)==replacement,"APFS did not replace streamed attributes and ACL");
    auto malformed=replacement;
    malformed[u"com.apple.system.Security"]=Bytes(67);
    Reject([&]{volume.SetAttributes(id,malformed);},"APFS accepted a malformed ACL");
    Check(volume.GetAttributes(id)==replacement,"Rejected APFS ACL changed existing attributes");
    volume.SetAttributes(id,{});
    const auto after=volume.Stat(id);
    Check(volume.GetAttributes(id).empty()&&after.identity==beforeAttributes.identity&&
          after.created==beforeAttributes.created&&after.modified==beforeAttributes.modified&&
          after.finder==beforeAttributes.finder&&after.forks[0].size==data.size()&&after.forks[1].size==streamed.size(),
          "APFS attribute edits changed unrelated metadata");
    for(unsigned fork:{0u,1u}){
        const auto& expected=fork?streamed:data;
        Bytes actual(expected.size());
        volume.Read(id,fork,0,actual.data(),actual.size());
        Check(actual==expected,"APFS attribute edits changed a file fork");
    }
}

void ExternalCaseSensitiveWrites(const std::string& root,FILE* report,const wchar_t* fixture)
{
    const auto path=VolumeCoord::Wide(root+(fixture?"apfs-macos-sensitive-writes.img":"apfs-external-sensitive-writes.img"));
    Storage::Job job;
    {
        auto original=fixture?OpenImage(fixture,false):nullptr;
        auto image=CreateFormatOutput(path.c_str(),original?original->Size():32*1024*1024);
        if(original){
            auto bytes=Disk::Read(*original,0,size_t(original->Size()));
            image->Write(0,bytes.data(),bytes.size());
        }else{
            Apfs::Format(*image,u"Case-sensitive external writer test",true,job);
            const auto checkpoint=Apfs::Probe(*image);
            Check(checkpoint&&checkpoint->volumes.size()==1,"Missing case-sensitive test volume");
            Apfs::ObjectMap objects(*image,*checkpoint,checkpoint->objectMap,checkpoint->transaction);
            const auto block=objects.Lookup(checkpoint->volumes.front());
            Check(block.has_value(),"Missing case-sensitive volume superblock");
            const auto offset=*block*checkpoint->blockSize;
            auto volume=Disk::Read(*image,offset,checkpoint->blockSize);
            // Exercise the compatibility writer even without a separately mastered image.
            memset(volume.data()+272,0,32);
            memcpy(volume.data()+272,"Case test",9);
            Seal(volume,0,checkpoint->blockSize);
            image->Write(offset,volume.data(),volume.size());
        }
        image->Flush();
        image->Keep();
    }
    const auto expect=[](MacBrowser::Volume& volume,uint32_t folder,const std::u16string& name,uint8_t value){
        MacBrowser::Entry entry;
        Check(volume.FindChild(folder,name,entry)&&entry.name==name&&entry.forks[0].size==8197,
              "Case-sensitive APFS lookup returned the wrong file");
        Bytes data(8197);
        volume.Read(entry.id,0,0,data.data(),data.size());
        Check(data==Bytes(data.size(),value),"Case-sensitive APFS write changed the wrong file");
        return entry.id;
    };
    {
        auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&volume->CaseSensitive()&&volume->Supports().writable&&!volume->Writable(),
              "Case-sensitive APFS did not select the compatibility writer");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        const auto folder=volume->MakeDirectory(2,u"Case compatibility test");
        const auto put=[&](const std::u16string& name,uint8_t value,bool overwrite=false){
            MacBrowser::Entry entry;
            entry.name=name;
            entry.forks[0].size=8197;
            const auto reader=[value](unsigned,uint64_t,void* bytes,size_t count){memset(bytes,value,count);};
            return volume->Put(folder,entry,reader,overwrite);
        };
        const auto upper=put(u"Report",0x31);
        const auto lower=put(u"report",0x62);
        Check(upper!=lower,"Case-sensitive APFS files shared an identity");
        Reject([&]{volume->Rename(upper,folder,u"report");},"APFS rename accepted an exact collision");
        volume->Rename(upper,folder,u"REPORT");
        expect(*volume,folder,u"REPORT",0x31);
        expect(*volume,folder,u"report",0x62);
        put(u"report",0x53,true);
        expect(*volume,folder,u"REPORT",0x31);
        volume->Remove(expect(*volume,folder,u"report",0x53));
        MacBrowser::Entry removed;
        Check(!volume->FindChild(folder,u"report",removed),"APFS case-sensitive delete retained the target");
        ExternalResourceForkWrites(*volume,folder);
        put(u"caf\u00e9",0x44);
        Reject([&]{put(u"cafe\u0301",0x22);},"APFS accepted a normalization-equivalent name");
    }
    auto reopened=MacBrowser::OpenBrowserImage(path);
    MacBrowser::Entry folder,missing;
    Check(reopened&&reopened->FindChild(2,u"Case compatibility test",folder)&&
          !reopened->FindChild(folder.id,u"Report",missing)&&!reopened->FindChild(folder.id,u"report",missing),
          "Case-sensitive APFS edits did not survive reopen");
    expect(*reopened,folder.id,u"REPORT",0x31);
    expect(*reopened,folder.id,u"caf\u00e9",0x44);
    fprintf(report,"PASS: %s case-sensitive APFS compatibility writer, collisions, rename, overwrite, delete and exact readback.\n",
            fixture?"macOS-created":"generated");
}
void ExternalMultiVolumeWrites(const std::string& root,FILE* report,const wchar_t* fixture,size_t targetIndex,bool mixedCase=false)
{
    const auto path=VolumeCoord::Wide(root+(mixedCase?"apfs-mixed-case-writes-":"apfs-multi-volume-writes-")+std::to_string(targetIndex+1)+".img");
    const auto siblingIndex=1-targetIndex;
    auto input=OpenImage(fixture,false);
    const auto initial=Apfs::Probe(*input);
    Check(initial&&initial->volumes.size()==2&&!initial->offset,"Expected two raw APFS volumes");
    for(const auto oid:initial->volumes)Apfs::InspectFreeQueueAllocation(*input,*initial,oid);
    const Apfs::ObjectMap originalMap(*input,*initial,initial->objectMap,initial->transaction);
    const auto siblingBlock=originalMap.Lookup(initial->volumes[siblingIndex]);
    Check(siblingBlock.has_value(),"Missing sibling APFS volume mapping");
    const auto siblingSuper=Disk::Read(*input,*siblingBlock*initial->blockSize,initial->blockSize);
    {
        auto output=CreateFormatOutput(path.c_str(),input->Size());
        Bytes buffer(1024*1024);
        for(uint64_t offset=0;offset<input->Size();offset+=buffer.size()){
            const auto size=size_t((std::min)(uint64_t(buffer.size()),input->Size()-offset));
            input->Read(offset,buffer.data(),size);
            output->Write(offset,buffer.data(),size);
        }
        output->Flush();
        output->Keep();
    }
    std::u16string targetName,siblingName;
    MacBrowser::Entry originalSibling;
    HfsPlus::Attributes siblingAttributes;
    std::array<Bytes,2> siblingForks;
    const auto readSibling=[&](MacBrowser::Volume& volume){
        MacBrowser::Entry sibling,folder,file;
        Check(volume.FindChild(2,siblingName,sibling),"APFS sibling volume disappeared");
        const bool found=volume.FindChild(sibling.id,u"Fixture",folder)?
                         volume.FindChild(folder.id,u"same.txt",file):volume.FindChild(sibling.id,u"small.txt",file);
        Check(found,"APFS sibling fixture disappeared");
        return file;
    };
    {
        auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&volume->Supports().writable,"APFS multi-volume fixture opened read-only");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        auto roots=volume->List(2);
        Check(roots.size()==2,"APFS multi-volume image did not expose both volume roots");
        targetName=roots[targetIndex].name;
        siblingName=roots[siblingIndex].name;
        originalSibling=readSibling(*volume);
        siblingAttributes=volume->GetAttributes(originalSibling.id);
        for(unsigned fork:{0u,1u}){
            auto& bytes=siblingForks[fork];
            bytes.resize(size_t(originalSibling.forks[fork].size));
            if(!bytes.empty())volume->Read(originalSibling.id,fork,0,bytes.data(),bytes.size());
        }
        const auto folder=volume->MakeDirectory(roots[targetIndex].id,u"HFV multi-volume edits");
        MacBrowser::Entry file;
        file.name=u"Remaining.bin";
        file.forks[0].size=9307;
        const auto put=[&](unsigned char value,bool replace,Storage::Job& job,bool cancel=false){
            return volume->PutForTransfer(folder,file,[&](unsigned,uint64_t offset,void* bytes,size_t count){
                memset(bytes,value,count);
                if(cancel&&offset)job.cancel(); // Leave at least one staged block behind the old checkpoint.
            },replace,job);
        };
        Storage::Job job;
        const auto id=put(0x34,false,job);
        Storage::Job cancelled;
        bool stopped=false;
        try{put(0x68,true,cancelled,true);}catch(const Storage::Cancelled&){stopped=true;}
        Bytes actual(9307);
        volume->Read(id,0,0,actual.data(),actual.size());
        Check(stopped&&actual==Bytes(actual.size(),0x34),"Cancelled APFS overwrite changed the original bytes");
        put(0x56,true,job);
        file.name=u"Deleted.bin";
        volume->Remove(put(0x90,false,job));
        if(mixedCase){
            constexpr size_t sensitiveIndex=1;
            const bool sensitive=targetIndex==sensitiveIndex;
            file.name=u"Report";
            const auto upper=put(0x31,false,job);
            file.name=u"report";
            if(sensitive){
                const auto lower=put(0x62,false,job);
                Check(upper!=lower,"APFS case-sensitive files shared an identity");
                Reject([&]{volume->Rename(upper,folder,u"report");},"APFS case-sensitive rename accepted an exact collision");
                Check(put(0x28,true,job)==lower,"APFS overwrite chose the wrong case-sensitive name");
                volume->Read(upper,0,0,actual.data(),actual.size());
                Check(actual==Bytes(actual.size(),0x31),"APFS overwrote a case-distinct sibling");
                volume->Rename(upper,folder,u"REPORT");
            }else{
                Reject([&]{put(0x62,false,job);},"APFS accepted a case-only duplicate in an insensitive volume");
                Check(put(0x28,true,job)==upper,"APFS did not fold case for overwrite");
                volume->Rename(upper,folder,u"REPORT");
            }
            MacBrowser::Entry upperEntry,lowerEntry;
            Check(volume->FindChild(folder,u"REPORT",upperEntry)&&volume->FindChild(folder,u"report",lowerEntry)&&
                  (upperEntry.id!=lowerEntry.id)==sensitive,"APFS lookup disagreed with its destination case mode");

            auto memory=std::make_unique<CheckpointFaultImage>(Bytes(16*1024*1024));
            Apfs::Format(*memory,u"Nested test source",true,job);
            std::unique_ptr<Source> input=std::move(memory);
            auto source=Apfs::OpenWritable(input,"apfs-mixed-case-nested-source");
            const auto tree=source->MakeDirectory(2,u"Nested collision test");
            const auto child=source->MakeDirectory(tree,u"Inside");
            MacBrowser::Entry name;
            name.name=u"Draft";
            source->Put(child,name,{},false);
            name.name=u"draft";
            source->Put(child,name,{},false);
            MacBrowser::Entry copied;
            if(sensitive){
                Check(MacBrowser::Copy(*source,tree,*volume,folder,job)&&
                      volume->FindChild(folder,u"Nested collision test",copied)&&volume->List(copied.id).size()==1&&
                      volume->List(volume->List(copied.id).front().id).size()==2,
                      "APFS case-sensitive copy lost a nested case-distinct name");
            }else{
                Reject([&]{MacBrowser::Copy(*source,tree,*volume,folder,job);},
                       "APFS insensitive copy accepted nested case-colliding names");
                Check(!volume->FindChild(folder,u"Nested collision test",copied),"APFS copy created a folder before detecting a nested collision");
            }
        }
        const auto same=readSibling(*volume);
        Check(same.id==originalSibling.id&&same.identity==originalSibling.identity,
              "APFS edit invalidated the sibling volume or entry identity");
    }
    {
        auto output=OpenImage(path.c_str(),false);
        const auto final=Apfs::Probe(*output);
        Check(final&&final->volumes==initial->volumes,"APFS edits changed the container volume list");
        const Apfs::ObjectMap finalMap(*output,*final,final->objectMap,final->transaction);
        Check(finalMap.Lookup(initial->volumes[siblingIndex])==siblingBlock&&
              Disk::Read(*output,*siblingBlock*final->blockSize,final->blockSize)==siblingSuper,
              "APFS edits changed the sibling volume mapping or superblock");
    }
    auto volume=MacBrowser::OpenBrowserImage(path);
    MacBrowser::Entry target,folder,remaining,deleted;
    Check(volume&&volume->FindChild(2,targetName,target)&&volume->FindChild(target.id,u"HFV multi-volume edits",folder)&&
          volume->FindChild(folder.id,u"Remaining.bin",remaining)&&!volume->FindChild(folder.id,u"Deleted.bin",deleted),
          "APFS writes did not survive reopening");
    Bytes actual(9307);
    volume->Read(remaining.id,0,0,actual.data(),actual.size());
    Check(actual==Bytes(actual.size(),0x56),"Overwritten APFS bytes did not survive reopening");
    const auto same=readSibling(*volume);
    Check(same.finder==originalSibling.finder&&same.created==originalSibling.created&&
          same.modified==originalSibling.modified&&volume->GetAttributes(same.id)==siblingAttributes,
          "APFS operation changed sibling file metadata");
    for(unsigned fork:{0u,1u}){
        const auto& expected=siblingForks[fork];
        Check(same.forks[fork].size==expected.size(),"APFS operation changed a sibling fork size");
        actual.resize(expected.size());
        if(!actual.empty())volume->Read(same.id,fork,0,actual.data(),actual.size());
        Check(actual==expected,"APFS operation changed a sibling fork");
    }
    if(mixedCase){
        MacBrowser::Entry upper,lower;
        const bool sensitive=targetIndex==1;
        Check(volume->FindChild(folder.id,u"REPORT",upper)&&upper.name==u"REPORT"&&
              volume->FindChild(folder.id,u"report",lower)&&(upper.id!=lower.id)==sensitive,
              "APFS case-mode edits did not survive reopening");
        volume->Read(lower.id,0,0,actual.data(),actual.size());
        Check(actual==Bytes(actual.size(),0x28),"APFS case-mode overwrite did not survive reopening");
        fprintf(report,"PASS: mixed-case APFS volume %zu uses its own naming and recursive copy rules.\n",targetIndex+1);
    }
    fprintf(report,"PASS: APFS volume %zu create, overwrite, delete, cancellation, and unchanged sibling mapping, forks and metadata.\n",
            targetIndex+1);
}

void SnapshotLiveWrites(const std::wstring& path,FILE* report,const char16_t* directory,const char16_t* name,bool withClones=false)
{
    uint64_t originalTransaction=0,originalCheckpoint=0;
    {
        auto raw=OpenImage(path.c_str(),false);
        const auto checkpoint=Apfs::Probe(*raw);
        Check(checkpoint.has_value(),"Missing APFS snapshot checkpoint");
        originalTransaction=checkpoint->transaction;
        originalCheckpoint=checkpoint->checkpointBlock;
        Apfs::InspectFreeQueueAllocation(*raw,*checkpoint);
    }
    MacBrowser::Entry live,snapshot;
    const auto roots=[&](MacBrowser::Volume& volume){
        const auto entries=volume.List(2);
        Check(entries.size()==2,"The APFS snapshot or live volume is missing");
        live=entries.front();
        snapshot=entries.back();
        Check(snapshot.name.find(u" [Snapshot: ")!=snapshot.name.npos,"Missing APFS snapshot view");
    };
    const auto locate=[&](MacBrowser::Volume& volume,uint32_t root,const char16_t* fileName){
        MacBrowser::Entry folder,file;
        Check(volume.FindChild(root,directory,folder)&&volume.FindChild(folder.id,fileName,file),
              "Missing APFS snapshot verification file");
        return file;
    };
    const auto bytes=[&](MacBrowser::Volume& volume,const MacBrowser::Entry& entry){
        Bytes data(size_t(entry.forks[0].size));
        if(!data.empty())volume.Read(entry.id,0,0,data.data(),data.size());
        return data;
    };
    const auto overwrite=[](MacBrowser::Volume& volume,const MacBrowser::Entry& original,Storage::Job& job,bool cancel){
        auto replacement=original;
        replacement.forks[0].size=9301;
        return volume.PutForTransfer(original.parent,replacement,[&](unsigned,uint64_t offset,void* data,size_t size){
            memset(data,0x73,size);
            if(cancel&&offset)job.cancel();
        },true,job);
    };
    std::array<Bytes,4> clones;
    const auto readClones=[&](MacBrowser::Volume& volume,bool renamed=false){
        std::array<Bytes,4> state;
        if(withClones){
            size_t index=0;
            for(const auto root:{live.id,snapshot.id})
                for(const auto file:{u"binary.bin",u"clone.bin"})
                    state[index++]=bytes(volume,locate(volume,root,
                        renamed&&root==live.id&&file==std::u16string_view(u"clone.bin")?u"renamed-clone.bin":file));
        }
        return state;
    };
    Bytes historical,historicalDeleted;
    {
        auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&volume->Supports().writable,"Supported APFS snapshot image opened read-only");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        roots(*volume);
        historical=bytes(*volume,locate(*volume,snapshot.id,name));
        clones=readClones(*volume);
        if(withClones){
            Check(!clones[0].empty()&&clones[0]!=clones[1],"The APFS clone fixture must contain distinct data");
            historicalDeleted=bytes(*volume,locate(*volume,snapshot.id,u"vanished"));
            Reject([&]{volume->Remove(locate(*volume,live.id,u"clone.bin").id);},
                   "APFS removed a file with shared extents");
        }
        const auto original=locate(*volume,live.id,name);
        const auto originalBytes=bytes(*volume,original);
        const auto historicalEntry=locate(*volume,snapshot.id,name);
        Storage::Job preflight;
        Check(!MacBrowser::Snapshot(*volume,original.id,preflight,true).empty(),
              "APFS move preflight rejected the live file");
        Check(!MacBrowser::Snapshot(*volume,historicalEntry.id,preflight,false).empty(),
              "APFS copy preflight rejected the snapshot file");
        Reject([&]{MacBrowser::Snapshot(*volume,historicalEntry.id,preflight,true);},
               "APFS move preflight accepted a read-only snapshot file");
        Reject([&]{volume->MakeDirectory(snapshot.id,u"No mutation");},"APFS wrote into a snapshot");
        Storage::Job cancelled;
        bool stopped=false;
        try{overwrite(*volume,original,cancelled,true);}catch(const Storage::Cancelled&){stopped=true;}
        Check(stopped&&bytes(*volume,locate(*volume,live.id,name))==originalBytes&&
              bytes(*volume,locate(*volume,snapshot.id,name))==historical,"Cancelled APFS write changed the live file or snapshot");
    }
    {
        auto raw=OpenImage(path.c_str(),false);
        const auto checkpoint=Apfs::Probe(*raw);
        Check(checkpoint&&checkpoint->transaction==originalTransaction&&checkpoint->checkpointBlock==originalCheckpoint,
              "Cancelled APFS live write published a checkpoint");
    }
    {
        auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&volume->Supports().writable,"Could not reopen the APFS live volume for writing");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        roots(*volume);
        const auto original=locate(*volume,live.id,name);
        Storage::Job written;
        const auto id=overwrite(*volume,original,written,false);
        Check(bytes(*volume,volume->Stat(id))==Bytes(9301,0x73),"APFS live overwrite changed replacement data");
        Check(bytes(*volume,locate(*volume,snapshot.id,name))==historical,"APFS live overwrite changed snapshot data");
        const auto deletedName=withClones?u"vanished":name;
        const auto deleted=withClones?locate(*volume,live.id,deletedName):volume->Stat(id);
        volume->Remove(deleted.id);
        MacBrowser::Entry missing;
        Check(!volume->FindChild(deleted.parent,deletedName,missing),"APFS live deletion retained the file");
        Check(bytes(*volume,locate(*volume,snapshot.id,name))==historical,"APFS live deletion changed snapshot data");
        Check(readClones(*volume)==clones,"APFS live edits changed a clone");
        if(withClones){
            const auto alias=locate(*volume,live.id,u"clone.bin");
            volume->Rename(alias.id,alias.parent,u"renamed-clone.bin");
            Check(!volume->FindChild(alias.parent,u"clone.bin",missing)&&readClones(*volume,true)==clones,
                  "Renaming the live APFS clone changed clone data or the snapshot");
        }
    }
    auto reopened=MacBrowser::OpenBrowserImage(path);
    Check(reopened!=nullptr,"Could not reopen APFS snapshot image");
    roots(*reopened);
    Check(bytes(*reopened,locate(*reopened,snapshot.id,name))==historical,"Reopened APFS snapshot changed original bytes");
    Check(readClones(*reopened,withClones)==clones,"Reopening APFS changed a clone");
    if(withClones){
        Check(bytes(*reopened,locate(*reopened,snapshot.id,u"vanished"))==historicalDeleted&&
              bytes(*reopened,locate(*reopened,live.id,name))==Bytes(9301,0x73),
              "Reopening APFS changed the overwritten live file or the deleted file in the snapshot");
    }
    fprintf(report,"PASS: APFS snapshot preserved through live overwrite, deletion, cancellation and reopening.\n");
    if(withClones)fprintf(report,"PASS: macOS APFS shared extents and both clone files survived live writes.\n");
}

void ExternalSpaceLimitWrites(const std::string& root,FILE* report,const wchar_t* fixture)
{
    constexpr uint64_t MiB=1024*1024;
    const auto path=VolumeCoord::Wide(root+"apfs-macos-volume-space-edits.img");
    Storage::Job conversion;
    const auto result=Dmg::Convert(fixture,path,Dmg::Output::RawImage,conversion);
    Check(!result.code,result.message.c_str());
    {
        auto volume=MacBrowser::OpenBrowserImage(path);
        Check(volume&&volume->Supports().writable,"APFS quota and reserve image opened read-only");
        VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
        for(const auto name:{u"Limited",u"Empty"}){
            MacBrowser::Entry selected,folder,pressure;
            Check(volume->FindChild(2,name,selected)&&volume->FindChild(selected.id,u"Fixture",folder)&&
                  volume->FindChild(folder.id,u"pressure",pressure),"Missing macOS APFS pressure directory");
            const auto files=volume->List(pressure.id);
            Check(files.size()>=2,"Missing macOS APFS pressure files");
            volume->Remove(files[0].id);
            volume->Remove(files[1].id);
        }
    }
    class Payload final:public Source {
        uint64_t size;
        Storage::Job* cancel;
    public:
        unsigned reads=0;
        Payload(uint64_t length,Storage::Job* job=nullptr):size(length),cancel(job){}
        uint64_t Size()const override{return size;}
        bool Writable()const override{return false;}
        void Flush()override{}
        void Write(uint64_t,const void*,size_t)override{throw Error("Unexpected APFS test input write");}
        void Read(uint64_t offset,void* dest,size_t count)override{
            ++reads;
            memset(dest,0x5d,count);
            if(cancel&&offset)cancel->cancel();
        }
    };
    {
        auto output=OpenImage(path.c_str(),true);
        const auto checkpoint=[&]{auto value=Apfs::Probe(*output);Check(value.has_value(),"Missing APFS quota checkpoint");return *value;};
        const auto target=[&](const std::string& name){
            auto current=checkpoint();
            const Apfs::ObjectMap map(*output,current,current.objectMap,current.transaction);
            for(const auto oid:current.volumes){
                const auto block=map.Lookup(oid);
                Check(block.has_value(),"Missing APFS quota volume mapping");
                const auto super=Apfs::ReadObject(*output,current,*block,oid,current.transaction,13);
                const auto label=reinterpret_cast<const char*>(super.data()+704);
                if(label[name.size()]||memcmp(label,name.data(),name.size()))continue;
                current.writeVolume=oid;
                return current;
            }
            throw Error("Missing APFS quota volume");
        };
        const auto unchanged=[&](const Apfs::Container& before){
            const auto after=checkpoint();
            Check(after.transaction==before.transaction&&after.checkpointBlock==before.checkpointBlock,
                  "A rejected or cancelled APFS space-limited write published a checkpoint");
        };
        const auto create=[&](const Apfs::Container& selected,const std::u16string& name,Payload& data,Storage::Job& job){
            return Apfs::CreateCatalogFile(*output,selected,2,name,data,job,{},nullptr,false,nullptr,nullptr,nullptr,true);
        };
        const auto reject=[&](const std::string& volume,uint64_t bytes,const char* reason){
            const auto before=target(volume);
            Payload payload(bytes);
            Storage::Job job;
            bool rejected=false;
            try{create(before,u"Must not be created",payload,job);}
            catch(const Error& failure){Check(strstr(failure.what(),reason)!=nullptr,failure.what());rejected=true;}
            Check(rejected&&!payload.reads,"APFS space limit rejection read or wrote file data");
            unchanged(before);
        };
        reject("Limited",9*MiB,"quota would be exceeded");
        reject("Empty",64*MiB,"reserved for another volume");
        Storage::Job job;
        Payload small(16384);
        create(target("Limited"),u"Within quota",small,job);
        Payload ownReserve(40*MiB);
        create(target("Reserved"),u"Within reserve",ownReserve,job);
        const auto before=target("Limited");
        Storage::Job cancel;
        Payload cancelled(16384,&cancel);
        bool stopped=false;
        try{create(before,u"Must not be created",cancelled,cancel);}catch(const Storage::Cancelled&){stopped=true;}
        Check(stopped&&cancelled.reads>=2,"APFS quota cancellation did not reach staging");
        unchanged(before);
    }
    auto volume=MacBrowser::OpenBrowserImage(path);
    for(const auto& target:std::array<std::pair<std::u16string,const char16_t*>,2>{{
            {u"Limited",u"Within quota"},{u"Reserved",u"Within reserve"}}}){
        MacBrowser::Entry selected,file,missing;
        Check(volume&&volume->FindChild(2,target.first,selected)&&volume->FindChild(selected.id,target.second,file)&&
              !volume->FindChild(selected.id,u"Must not be created",missing),"APFS space-limited writes did not reopen correctly");
        Bytes data(4096);
        volume->Read(file.id,0,file.forks[0].size-data.size(),data.data(),data.size());
        Check(data==Bytes(data.size(),0x5d),"APFS space-limited write read back different bytes");
    }
    for(const auto& expected:std::array<std::pair<const char16_t*,const char*>,3>{{
            {u"Limited","APFS quota: 8388608 bytes"},
            {u"Reserved","APFS reserve: 1577058304 bytes"},
            {u"Empty",""}}}){
        MacBrowser::Entry rootEntry;
        Check(volume->FindChild(2,expected.first,rootEntry)&&volume->PropertiesDetails(rootEntry.id)==expected.second,
              "APFS volume Properties did not report the macOS quota or reserve");
    }
    fprintf(report,"PASS: macOS APFS volume quota, sibling reserve, own reserve, cancellation, and unchanged rejected checkpoints.\n");
}

void WritableGptImages(const std::string& root,FILE* report,const wchar_t* macFixture=nullptr)
{
    const auto baselinePath=macFixture?std::wstring(macFixture):VolumeCoord::Wide(root+"checkpoint-before.img");
    auto baseline=OpenImage(baselinePath.c_str(),false);
    Memory raw(size_t(baseline->Size()));
    baseline->Read(0,raw.bytes.data(),raw.bytes.size());
    constexpr size_t start=1024*1024;
    for(const unsigned sector:{512u,4096u}){
        for(const bool readOnly:{false,true}){
            const auto disk=Gpt(raw,sector,readOnly);
            const auto suffix=(macFixture?"macos-":"")+std::to_string(sector)+".img";
            const auto path=VolumeCoord::Wide(root+(readOnly?"apfs-gpt-readonly-":"apfs-gpt-writable-")+suffix);
            {
                auto file=CreateFormatOutput(path.c_str(),disk.bytes.size());
                file->Write(0,disk.bytes.data(),disk.bytes.size());
                file->Flush();
                file->Keep();
            }
            {
                auto volume=MacBrowser::OpenBrowserImage(path);
                Check(volume&&volume->Supports().writable==!readOnly,"APFS GPT partition has the wrong write capability");
                if(!readOnly){
                    VolumeCoord::OperationLease lease({{volume->ResourceKey(),VolumeCoord::Mode::Write}});
                    const auto folder=volume->MakeDirectory(2,u"Partition test");
                    MacBrowser::Entry file;
                    file.name=u"Payload";
                    file.forks[0].size=8011;
                    volume->Put(folder,file,[](unsigned fork,uint64_t,void* output,size_t count){
                        Check(fork==0,"Unexpected APFS GPT test fork");
                        memset(output,0x6a,count);
                    },false);
                    if(macFixture)ExternalResourceForkWrites(*volume,folder);
                    volume->Rename(folder,2,u"Edited partition");
                }
            }
            auto file=OpenImage(path.c_str(),false);
            const auto result=Disk::Read(*file,0,disk.bytes.size());
            file.reset();
            Check(std::equal(result.begin(),result.begin()+start,disk.bytes.begin())&&
                  std::equal(result.begin()+start+raw.bytes.size(),result.end(),disk.bytes.begin()+start+raw.bytes.size()),
                  "APFS wrote outside the GPT partition");
            if(readOnly)Check(result==disk.bytes,"Opening read-only APFS GPT media changed it");
            else{
                auto volume=MacBrowser::OpenBrowserImage(path);
                MacBrowser::Entry folder,payload;
                std::array<uint8_t,9> tail{};
                Check(volume&&volume->Supports().writable&&volume->FindChild(2,u"Edited partition",folder)&&
                      volume->FindChild(folder.id,u"Payload",payload),"APFS edits inside GPT did not survive reopening");
                volume->Read(payload.id,0,8002,tail.data(),tail.size());
                Check(std::all_of(tail.begin(),tail.end(),[](uint8_t value){return value==0x6a;}),
                      "APFS GPT readback changed file contents");
                const auto slice=VolumeCoord::Wide(root+"apfs-gpt-slice-"+suffix);
                auto output=CreateFormatOutput(slice.c_str(),raw.bytes.size());
                output->Write(0,result.data()+start,raw.bytes.size());
                output->Flush();
                output->Keep();
            }
        }
    }
    fprintf(report,"PASS: %s APFS writes in GPT slices, 512/4096 sector geometry, read-only flags, unchanged partition tables and file readback.\n",
            macFixture?"macOS-created":"HFVExplorer-created");
    if(macFixture)fprintf(report,"PASS: macOS APFS resource forks, embedded/streamed attributes, ACL edits, cancellation, and data and metadata preservation.\n");
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
        ObjectMapFixture(mapped);
        auto index=mapped.bytes.data()+13*4096;
        Little(index+36,2,4);Little(index+42,8,2);Little(index+44,32,2);
        Little(index+60,16,2);Little(index+62,16,2);
        Little(index+64,1024,8);Little(index+72,1,8);
        Little(index+80,1029,8);Little(index+88,6,8);
        Little(index+4096-56,15,8);
        Seal(mapped.bytes,13*4096,4096);
        Reject([&]{map.Lookup(1029);},"APFS object map accepted a leaf crossing its next separator");
        fprintf(report,"PASS: APFS object-map index, object versions, tombstones, child ranges and cycles.\n");
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
        DataStreamAllocation(report);
        CheckpointWrites(root,report);
        WritableGptImages(root,report);
        if(const auto fixture=_wgetenv(L"HFV_APFS_MULTI_VOLUME_FIXTURE")){
            std::wstring input=fixture;
            if(input.size()>=4&&input.compare(input.size()-4,4,L".dmg")==0){
                const auto extracted=VolumeCoord::Wide(root+"apfs-multi-volume-source.img");
                Storage::Job conversion;
                const auto result=Dmg::Convert(input,extracted,Dmg::Output::RawImage,conversion);
                Check(!result.code,result.message.c_str());
                input=extracted;
            }
            for(size_t target=0;target<2;++target)
                ExternalMultiVolumeWrites(root,report,input.c_str(),target);
        }
        if(const auto mixed=_wgetenv(L"HFV_APFS_MIXED_CASE_FIXTURE")){
            for(size_t target=0;target<2;++target)
                ExternalMultiVolumeWrites(root,report,mixed,target,true);
        }
        if(const auto quota=_wgetenv(L"HFV_APFS_SPACE_LIMIT_FIXTURE"))
            ExternalSpaceLimitWrites(root,report,quota);
        if(const auto snapshot=_wgetenv(L"HFV_APFS_APPLE_SNAPSHOT_WRITE_FIXTURE")){
            const auto path=VolumeCoord::Wide(root+"apple-snapshot-live-edits.img");
            Check(CopyFileW(snapshot,path.c_str(),TRUE)!=FALSE,"Could not copy APFS snapshot writer fixture");
            SnapshotLiveWrites(path,report,u"Fixture",u"entry-0");
        }
        if(const auto native=_wgetenv(L"HFV_APFS_NATIVE_SHARED_SNAPSHOT")){
            const auto diskPath=VolumeCoord::Wide(root+"apfs-native-shared-source-disk.img");
            const auto path=VolumeCoord::Wide(root+"apfs-native-shared-snapshot-edits.img");
            Storage::Job conversion;
            const auto result=Dmg::Convert(native,diskPath,Dmg::Output::RawImage,conversion);
            Check(!result.code,result.message.c_str());
            {
                const auto disk=OpenImage(diskPath.c_str(),false);
                const auto container=Apfs::Probe(*disk);
                Check(container.has_value(),"Missing APFS native snapshot container");
                const auto size=container->blockCount*container->blockSize;
                auto output=CreateFormatOutput(path.c_str(),size);
                Bytes buffer(1024*1024);
                for(uint64_t offset=0;offset<size;offset+=buffer.size()){
                    const auto count=size_t((std::min)(uint64_t(buffer.size()),size-offset));
                    disk->Read(container->offset+offset,buffer.data(),count);
                    output->Write(offset,buffer.data(),count);
                }
                output->Flush();
                output->Keep();
            }
            SnapshotLiveWrites(path,report,u"Fixture",u"generation.txt",true);
        }
        ExternalCaseSensitiveWrites(root,report,nullptr);
        if(const auto fixture=_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE"))
            ExternalCaseSensitiveWrites(root,report,fixture);
        MultiChunkWrites(root,report);
        MultiCibWrites(root,report);
        SplitCatalogWrites(root,report);
        RenameCatalogWrites(root,report);
        DeleteCatalogWrites(root,report);
        ReplaceCatalogData(root,report);
        WritableNamespace(root,report);
        WritableHardLinks(root,report);
        SharedInodeEdits(root,report);
        DirectoryHashCollisions(root,report);
        UnicodeCatalogNames(root,report);
        FinderMetadataWrites(root,report);
        ExtentReferenceGrowth(root,report);
        DeepCatalogGrowth(root,report);
        if(const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE")){
            auto readOnlySource=OpenImage(fixture,false);
            const auto authentic=Apfs::Open(readOnlySource,"apfs-macos-read-only-reference");
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
            WritableGptImages(root,report,fixture);
            const auto raw=OpenImage(fixture,false);
            const auto before=Apfs::Probe(*raw);
            Check(before.has_value(),"Missing macOS-generated APFS checkpoint");
            const auto layout=Apfs::InspectCatalogLayout(*raw,*before);
            Check(layout.catalogRecords>800&&layout.extentReferences>100,
                  "The write planner did not validate the macOS-generated catalog and extent tree");
            const auto free=Apfs::InspectAllocationFreeBytes(*raw,*before);
            Check(free>0&&free<raw->Size()&&free==Apfs::WritableFreeBytes(*raw,*before),
                  "APFS macOS allocation bitmap totals did not match the space manager");
            Check(Apfs::InspectFreeQueueAllocation(*raw,*before)>free,
                  "APFS macOS free queues did not release blocks in the allocation dry run");
            const auto image=Disk::Read(*raw,0,size_t(raw->Size()));
            {
                CheckpointFaultImage interrupted(image);
                Storage::Job job;
                interrupted.onFlush=[&]{job.cancel();};
                bool cancelled=false;
                try{Apfs::CommitCheckpoint(interrupted,*before,job);}
                catch(const Storage::Cancelled&){cancelled=true;}
                const auto preserved=Apfs::Probe(interrupted);
                Check(cancelled&&preserved&&preserved->transaction==before->transaction,
                      "Cancelled publication changed a macOS-generated APFS checkpoint");
            }
            constexpr uint64_t editedTime=3730000123,createdTime=3700000456;
            const auto publishedPath=VolumeCoord::Wide(root+"apfs-macos-compressed-rename.img");
            {
                auto output=CreateFormatOutput(publishedPath.c_str(),image.size());
                output->Write(0,image.data(),image.size());
                output->Flush();
                Storage::Job job;
                const auto xid=Apfs::CommitCheckpoint(*output,*before,job);
                const auto after=Apfs::Probe(*output);
                Check(after&&after->transaction==xid&&xid>before->transaction&&after->uuid==before->uuid,
                      "macOS-generated APFS checkpoint was not published");
                Reject([&]{Apfs::SetExternalCatalogProperties(*output,*after,2,u"missing.txt",{std::nullopt,editedTime},job);},
                       "APFS accepted a missing timestamp target");
                Check(Apfs::SetExternalCatalogProperties(*output,*after,2,u"small.txt",{std::nullopt,editedTime},job)>xid,
                      "APFS did not publish the macOS modification time edit");
                const auto modified=Apfs::Probe(*output);
                Check(modified.has_value(),"APFS modification time checkpoint could not be reopened");
                CheckpointFaultImage interrupted(Disk::Read(*output,0,size_t(output->Size())));
                Storage::Job cancel;
                interrupted.onFlush=[&]{cancel.cancel();};
                bool cancelled=false;
                try{Apfs::SetExternalCatalogProperties(interrupted,*modified,2,u"small.txt",{createdTime,{}},cancel);}
                catch(const Storage::Cancelled&){cancelled=true;}
                const auto retained=Apfs::Probe(interrupted);
                Check(cancelled&&retained&&retained->transaction==modified->transaction,
                      "Cancelled APFS file timestamp edit published a checkpoint");
                Check(Apfs::SetExternalCatalogProperties(*output,*modified,2,u"small.txt",{createdTime,{}},job)>modified->transaction,
                      "APFS did not publish the macOS creation time edit");
                const auto openCurrent=[&]{
                    const auto current=Apfs::Probe(*output);
                    Check(current.has_value(),"Missing APFS Properties checkpoint");
                    return *current;
                };
                constexpr auto file=u"small.txt";
                Apfs::SetExternalCatalogProperties(*output,openCurrent(),2,file,{},job,true);
                const auto locked=openCurrent();
                Reject([&]{Apfs::SetExternalCatalogProperties(*output,locked,2,file,{createdTime+1,{}},job);},
                       "APFS accepted a date edit on a locked file");
                Check(openCurrent().transaction==locked.transaction,"Rejected APFS lock edit changed the checkpoint");
                Apfs::SetExternalCatalogProperties(*output,locked,2,file,{},job,false);
                const auto beforeRename=openCurrent();
                Reject([&]{Apfs::RenameExternalCatalogEntry(*output,beforeRename,2,file,u"HARD_A.TXT",job);},
                       "APFS accepted a case-insensitive rename collision");
                Check(openCurrent().transaction==beforeRename.transaction,"Rejected APFS rename changed the checkpoint");
                constexpr auto longer=u"Longer caf\u00e9 filename.txt";
                Apfs::RenameExternalCatalogEntry(*output,beforeRename,2,file,longer,job);
                Apfs::RenameExternalCatalogEntry(*output,openCurrent(),2,longer,u"x",job);
                Apfs::SetExternalCatalogProperties(*output,openCurrent(),2,u"x",{createdTime,{}},job,true);
                constexpr uint64_t manyInode=24; // Directory ID on the macOS-generated fixture.
                Apfs::RenameExternalCatalogEntry(*output,openCurrent(),manyInode,u"file_0222.txt",u"Renamed 222.txt",job);
                Apfs::RenameExternalCatalogEntry(*output,openCurrent(),2,u"many",u"Renamed collection",job);
                Apfs::RenameExternalCatalogEntry(*output,openCurrent(),2,u"link.txt",u"Renamed symbolic link",job);
                {
                    const auto checkpoint=openCurrent();
                    CheckpointFaultImage interruptedLink(Disk::Read(*output,0,size_t(output->Size())));
                    Storage::Job cancelLink;
                    interruptedLink.onFlush=[&]{cancelLink.cancel();};
                    constexpr uint64_t symlinkInode=425; // Inode of link.txt in the macOS fixture.
                    bool cancelled=false;
                    try{
                        Apfs::ReplaceCatalogSymlink(interruptedLink,checkpoint,2,symlinkInode,
                                                    Bytes{'n','o','n','e',0},cancelLink,{},{},false,{},true);
                    }catch(const Storage::Cancelled&){cancelled=true;}
                    const auto retained=Apfs::Probe(interruptedLink);
                    Check(cancelled&&retained&&retained->transaction==checkpoint.transaction,
                          "Cancelled APFS symbolic-link overwrite published a checkpoint");
                    Storage::Job cancelDelete;
                    interruptedLink.onFlush=[&]{cancelDelete.cancel();};
                    bool deletionCancelled=false;
                    try{Apfs::DeleteEntry(interruptedLink,checkpoint,2,symlinkInode,cancelDelete,0,true);}
                    catch(const Storage::Cancelled&){deletionCancelled=true;}
                    const auto afterCancellation=Apfs::Probe(interruptedLink);
                    Check(deletionCancelled&&afterCancellation&&afterCancellation->transaction==checkpoint.transaction,
                          "Cancelled APFS symbolic-link deletion published a checkpoint");
                    auto memory=std::make_unique<Memory>(interruptedLink.durable.size());
                    memory->bytes=std::move(interruptedLink.durable);
                    std::unique_ptr<Source> source=std::move(memory);
                    const auto reopened=Apfs::Open(source,"apfs-cancelled-symlink-overwrite");
                    MacBrowser::Entry original;
                    Check(reopened&&reopened->FindChild(2,u"Renamed symbolic link",original)&&
                          reopened->SymbolicLinkTarget(original.id)==authentic->SymbolicLinkTarget(symlink.id),
                          "Cancelled APFS symbolic-link deletion or overwrite changed the original target");
                    Reject([&]{reopened->Read(original.id,0,0,nullptr,0);},
                           "Cancelled APFS copy followed the original symbolic-link target");
                }
                Apfs::SetExternalCatalogProperties(*output,openCurrent(),2,u"Renamed collection",{createdTime,{}},job,true);
                const auto folderLocked=openCurrent();
                Reject([&]{Apfs::RenameExternalCatalogEntry(*output,folderLocked,manyInode,u"Renamed 222.txt",u"Blocked",job);},
                       "APFS accepted a rename inside a locked folder");
                Check(openCurrent().transaction==folderLocked.transaction,"Rejected APFS rename wrote a checkpoint");
                Apfs::SetExternalCatalogProperties(*output,openCurrent(),2,u"Renamed collection",{},job,false);
                Apfs::SetExternalCatalogProperties(*output,openCurrent(),2,u"hard_b.txt",{createdTime,editedTime},job,true);
                const auto linkLocked=openCurrent();
                Reject([&]{Apfs::SetExternalCatalogProperties(*output,linkLocked,2,u"hard_a.txt",{{},editedTime+1},job);},
                       "Editing another APFS alias bypassed the shared lock");
                Check(openCurrent().transaction==linkLocked.transaction,"Rejected APFS hard-link edit wrote a checkpoint");
                Memory payload(9181);
                for(size_t i=0;i<payload.bytes.size();++i)payload.bytes[i]=uint8_t((i*13)%251);
                {
                    const auto checkpoint=openCurrent();
                    CheckpointFaultImage interruptedCompression(Disk::Read(*output,0,size_t(output->Size())));
                    Storage::Job cancelCompressedRename;
                    interruptedCompression.onFlush=[&]{cancelCompressedRename.cancel();};
                    bool renameCancelled=false;
                    try{
                        Apfs::RenameExternalCatalogEntry(interruptedCompression,checkpoint,2,compressed.name,u"Cancelled compressed rename",cancelCompressedRename);
                    }catch(const Storage::Cancelled&){renameCancelled=true;}
                    const auto afterRenameCancellation=Apfs::Probe(interruptedCompression);
                    Check(renameCancelled&&afterRenameCancellation&&afterRenameCancellation->transaction==checkpoint.transaction,
                          "Cancelled compressed APFS rename published a checkpoint");
                    Storage::Job cancelAttributes;
                    interruptedCompression.onFlush=[&]{cancelAttributes.cancel();};
                    bool attributesCancelled=false;
                    try{
                        Apfs::SetCatalogAttributes(interruptedCompression,checkpoint,2,430,
                            {{u"user.cancelled",Bytes(8001,0x42)}},cancelAttributes,true);
                    }catch(const Storage::Cancelled&){attributesCancelled=true;}
                    const auto afterAttributeCancellation=Apfs::Probe(interruptedCompression);
                    Check(attributesCancelled&&afterAttributeCancellation&&afterAttributeCancellation->transaction==checkpoint.transaction,
                          "Cancelled APFS streamed attribute edit published a checkpoint");
                    Storage::Job cancelCompressedDelete;
                    interruptedCompression.onFlush=[&]{cancelCompressedDelete.cancel();};
                    constexpr uint64_t compressedInode=430; // Inode of decmpfs.txt in the macOS fixture.
                    bool cancelled=false;
                    try{Apfs::DeleteEntry(interruptedCompression,checkpoint,2,compressedInode,cancelCompressedDelete,0,true);}
                    catch(const Storage::Cancelled&){cancelled=true;}
                    const auto after=Apfs::Probe(interruptedCompression);
                    Check(cancelled&&after&&after->transaction==checkpoint.transaction,
                          "Cancelled compressed APFS deletion published a checkpoint");
                    auto memory=std::make_unique<Memory>(interruptedCompression.durable.size());
                    memory->bytes=std::move(interruptedCompression.durable);
                    std::unique_ptr<Source> source=std::move(memory);
                    const auto reopened=Apfs::Open(source,"apfs-cancelled-compressed-delete");
                    MacBrowser::Entry original;
                    Check(reopened&&reopened->FindChild(2,compressed.name,original)&&original.compressed&&
                          original.forks[0].size==decoded.size()&&original.finder==compressed.finder&&
                          original.modified==compressed.modified&&original.created==compressed.created,
                          "Cancelled APFS deletion changed compressed file metadata");
                    Bytes actual(decoded.size());
                    reopened->Read(original.id,0,0,actual.data(),actual.size());
                    Check(actual==decoded,"Cancelled APFS deletion changed the compressed file bytes");
                }
                const auto beforeCreate=openCurrent();
                const auto volumeOid=beforeCreate.volumes.front();
                const Apfs::ObjectMap map(*output,beforeCreate,beforeCreate.objectMap,beforeCreate.transaction);
                const auto volumeBlock=map.Lookup(volumeOid);
                Check(volumeBlock.has_value(),"Missing APFS volume for cancellation test");
                const auto volume=Apfs::ReadObject(*output,beforeCreate,*volumeBlock,volumeOid,beforeCreate.transaction,13);
                const auto newInode=Apfs::Le64(volume.data()+176);
                Apfs::CreateCatalogFile(*output,beforeCreate,2,u"Created data.bin",payload,job,{},
                                        nullptr,false,nullptr,nullptr,nullptr,true);
                const auto beforeDelete=openCurrent();
                {
                    CheckpointFaultImage unlinkDisk(Disk::Read(*output,0,size_t(output->Size())));
                    Apfs::CreateCatalogHardLink(unlinkDisk,beforeDelete,2,newInode,u"Cancelled unlink",job,0,true);
                    const auto linked=Apfs::Probe(unlinkDisk);
                    Check(linked.has_value(),"Missing APFS hard-link checkpoint");
                    Storage::Job cancelUnlink;
                    unlinkDisk.onFlush=[&]{cancelUnlink.cancel();};
                    bool cancelled=false;
                    try{Apfs::DeleteEntry(unlinkDisk,*linked,2,newInode,cancelUnlink,newInode+2,true);}
                    catch(const Storage::Cancelled&){cancelled=true;}
                    const auto retained=Apfs::Probe(unlinkDisk);
                    Check(cancelled&&retained&&retained->transaction==linked->transaction,
                          "Cancelled APFS unlink published a checkpoint");
                    auto memory=std::make_unique<Memory>(unlinkDisk.durable.size());
                    memory->bytes=std::move(unlinkDisk.durable);
                    std::unique_ptr<Source> source=std::move(memory);
                    const auto reopened=Apfs::Open(source,"apfs-cancelled-unlink");
                    MacBrowser::Entry original,alias;
                    Check(reopened&&reopened->FindChild(2,u"Created data.bin",original)&&
                          reopened->FindChild(2,u"Cancelled unlink",alias)&&original.linkCount==2&&
                          alias.linkCount==2&&original.resolvedId==alias.resolvedId,
                          "Cancelled APFS unlink lost an alias");
                }
                {
                    CheckpointFaultImage overwriteDisk(Disk::Read(*output,0,size_t(output->Size())));
                    Memory data(4411),resource(5801);
                    std::fill(data.bytes.begin(),data.bytes.end(),0x25);
                    std::fill(resource.bytes.begin(),resource.bytes.end(),0x31);
                    const Apfs::FinderBytes finder{0x15};
                    const Attributes attributes{{u"user.victim",{2,3,5}}};
                    Apfs::CreateCatalogFile(overwriteDisk,beforeDelete,2,u"Retained victim",data,job,
                                            Apfs::FileDates{createdTime,editedTime},&finder,false,&attributes,&resource,nullptr,true);
                    const auto checkpoint=Apfs::Probe(overwriteDisk);
                    Check(checkpoint.has_value(),"Missing APFS hard-link overwrite checkpoint");
                    Storage::Job cancelLink;
                    overwriteDisk.onFlush=[&]{cancelLink.cancel();};
                    bool cancelled=false;
                    try{
                        Apfs::CreateCatalogHardLink(overwriteDisk,*checkpoint,2,newInode,u"Retained victim",
                                                    cancelLink,newInode+1,true);
                    }catch(const Storage::Cancelled&){cancelled=true;}
                    const auto current=Apfs::Probe(overwriteDisk);
                    Check(cancelled&&current&&current->transaction==checkpoint->transaction,
                          "Cancelled APFS hard-link overwrite published a checkpoint");
                    auto memory=std::make_unique<Memory>(overwriteDisk.durable.size());
                    memory->bytes=std::move(overwriteDisk.durable);
                    std::unique_ptr<Source> source=std::move(memory);
                    const auto reopened=Apfs::Open(source,"apfs-cancelled-hard-link-overwrite");
                    MacBrowser::Entry victim;
                    Check(reopened&&reopened->FindChild(2,u"Retained victim",victim)&&!victim.hardLink&&
                          victim.forks[0].size==data.Size()&&victim.forks[1].size==resource.Size()&&
                          victim.finder==finder&&victim.created==createdTime&&victim.modified==editedTime&&
                          reopened->GetAttributes(victim.id)==attributes,
                          "Cancelled APFS hard-link overwrite changed the victim metadata");
                    Bytes keptData(size_t(data.Size())),keptResource(size_t(resource.Size()));
                    reopened->Read(victim.id,0,0,keptData.data(),keptData.size());
                    reopened->Read(victim.id,1,0,keptResource.data(),keptResource.size());
                    Check(keptData==data.bytes&&keptResource==resource.bytes,
                          "Cancelled APFS hard-link overwrite changed victim forks");
                }
                CheckpointFaultImage deletion(Disk::Read(*output,0,size_t(output->Size())));
                Storage::Job deletionJob;
                deletion.onFlush=[&]{deletionJob.cancel();};
                bool deletionCancelled=false;
                try{Apfs::DeleteEntry(deletion,beforeDelete,2,newInode,deletionJob,0,true);}
                catch(const Storage::Cancelled&){deletionCancelled=true;}
                const auto preserved=Apfs::Probe(deletion);
                Check(deletionCancelled&&preserved&&preserved->transaction==beforeDelete.transaction,
                      "Cancelled APFS deletion changed the durable checkpoint");
                auto memory=std::make_unique<Memory>(deletion.durable.size());
                memory->bytes=std::move(deletion.durable);
                std::unique_ptr<Source> retainedSource=std::move(memory);
                const auto retainedVolume=Apfs::Open(retainedSource,"apfs-cancelled-deletion");
                MacBrowser::Entry retainedFile;
                Check(retainedVolume&&retainedVolume->FindChild(2,u"Created data.bin",retainedFile)&&
                      retainedFile.forks[0].size==payload.Size(),"Cancelled APFS deletion lost its file");
                Bytes retainedBytes(payload.bytes.size());
                retainedVolume->Read(retainedFile.id,0,0,retainedBytes.data(),retainedBytes.size());
                Check(retainedBytes==payload.bytes,"Cancelled APFS deletion changed file bytes");
                const auto treeInode=newInode+1;
                Apfs::CreateCatalogDirectory(*output,openCurrent(),2,u"Cancel tree",job,{},nullptr,nullptr,true);
                Apfs::CreateCatalogFile(*output,openCurrent(),treeInode,u"Kept child.bin",payload,job,{},
                                        nullptr,false,nullptr,nullptr,nullptr,true);
                const auto beforeTreeDelete=openCurrent();
                CheckpointFaultImage treeDelete(Disk::Read(*output,0,size_t(output->Size())));
                Storage::Job treeJob;
                treeDelete.onFlush=[&]{treeJob.cancel();};
                bool treeCancelled=false;
                try{Apfs::DeleteEntry(treeDelete,beforeTreeDelete,2,treeInode,treeJob,0,true,true);}
                catch(const Storage::Cancelled&){treeCancelled=true;}
                const auto previous=Apfs::Probe(treeDelete);
                Check(treeCancelled&&previous&&previous->transaction==beforeTreeDelete.transaction,
                      "Cancelled recursive APFS deletion published a checkpoint");
                auto oldTree=std::make_unique<Memory>(treeDelete.durable.size());
                oldTree->bytes=std::move(treeDelete.durable);
                std::unique_ptr<Source> oldTreeSource=std::move(oldTree);
                const auto mountedTree=Apfs::Open(oldTreeSource,"apfs-cancelled-tree");
                MacBrowser::Entry tree,kept;
                Check(mountedTree&&mountedTree->FindChild(2,u"Cancel tree",tree)&&
                      mountedTree->FindChild(tree.id,u"Kept child.bin",kept),
                      "Cancelled recursive APFS deletion lost a descendant");
                mountedTree->Read(kept.id,0,0,retainedBytes.data(),retainedBytes.size());
                Check(retainedBytes==payload.bytes,"Cancelled recursive APFS deletion changed child bytes");
                output->Keep();
            }
            const auto published=MacBrowser::OpenBrowserImage(publishedPath);
            MacBrowser::Entry publishedA,publishedB,publishedFolder,publishedFile;
            Check(published&&published->FindChild(2,u"hard_a.txt",publishedA)&&
                  published->FindChild(2,u"hard_b.txt",publishedB)&&publishedA.resolvedId==publishedB.resolvedId&&
                  publishedA.protectedFile&&publishedB.protectedFile&&publishedA.created==createdTime&&publishedB.created==createdTime&&
                  publishedA.modified==editedTime&&publishedB.modified==editedTime&&
                   !published->FindChild(2,u"many",publishedFolder)&&
                  published->FindChild(2,u"Renamed collection",publishedFolder)&&published->List(publishedFolder.id).size()==400&&
                  publishedFolder.modified!=many.modified&&publishedFolder.created==createdTime&&!publishedFolder.protectedFile&&
                  !published->FindChild(publishedFolder.id,u"file_0222.txt",publishedFile)&&
                  published->FindChild(publishedFolder.id,u"Renamed 222.txt",publishedFile),
                  "Publishing a checkpoint changed the macOS-generated catalog");
            MacBrowser::Entry imported;
            Check(published->FindChild(2,u"Created data.bin",imported)&&imported.forks[0].size==9181,
                  "Created APFS file did not survive reopen");
            Bytes importedBytes(9181);
            published->Read(imported.id,0,0,importedBytes.data(),importedBytes.size());
            for(size_t i=0;i<importedBytes.size();++i)
                Check(importedBytes[i]==(i*13)%251,"Created APFS file readback differs");
            const auto caps=published->Supports();
            Check(caps.rename&&caps.writable&&!published->Writable()&&caps.create&&caps.createFolders&&
                  caps.remove&&caps.CanEditBasicMetadata()&&caps.finderMetadata&&caps.CanEditAttributes()&&
                  caps.CanEditCompression()&&caps.CanCreateHardLinks(),
                  "External APFS mount exposed operations beyond rename");
            const auto importPath=VolumeCoord::Wide(root+"apfs-import-source.img");
            {
                auto image=CreateFormatOutput(importPath.c_str(),8*1024*1024);
                Storage::Job job;
                Apfs::Format(*image,u"Import source",false,job);
                image->Keep();
            }
            const auto importSource=MacBrowser::OpenBrowserImage(importPath);
            Check(importSource&&importSource->Writable(),"Could not mount APFS import source");
            MacBrowser::Entry toCopy;
            toCopy.name=u"Imported forks";
            toCopy.forks[0].size=4301;
            toCopy.forks[1].size=5701;
            toCopy.datesValid=true;
            toCopy.created=createdTime;
            toCopy.modified=editedTime;
            toCopy.finder[0]='F';
            toCopy.attributes={{u"user.imported",Bytes(4200,0x33)}};
            const auto importId=importSource->Put(2,toCopy,[](unsigned fork,uint64_t offset,void* data,size_t length){
                memset(data,fork?0x7b:0x6a,length);
            },false);
            const FileOperations::OrdinaryCopy request{{FileOperations::Location::Mounted(importSource,importId)},
                                                      FileOperations::Location::Mounted(published,2)};
            const auto runImport=[&]{
                auto copy=FileOperations::Submit(request);
                const auto start=GetTickCount();
                while(!copy.Job()->done()){
                    Check(GetTickCount()-start<15000,"External APFS import timed out");
                    Sleep(5);
                }
                return copy.Job()->result();
            };
            const auto compressionFolder=published->MakeDirectory(2,u"Compressed copies");
            Storage::Job compressionCopy;
            Check(MacBrowser::Copy(*authentic,compressed.id,*published,compressionFolder,compressionCopy),
                  "Copying a macOS compressed file into APFS failed");
            MacBrowser::Entry copiedCompressed;
            Check(published->FindChild(compressionFolder,compressed.name,copiedCompressed)&&copiedCompressed.compressed&&
                  copiedCompressed.forks[0].size==decoded.size()&&!copiedCompressed.forks[1].size,
                  "APFS copy did not retain resource-backed compression");
            Bytes actualCompressed(decoded.size());
            published->Read(copiedCompressed.id,0,0,actualCompressed.data(),actualCompressed.size());
            Check(actualCompressed==decoded,"Copied APFS compressed file differed byte for byte");
            auto compressedEdit=copiedCompressed;
            const auto originalAttributes=published->GetAttributes(copiedCompressed.id);
            compressedEdit.attributes={{u"user.compressed",{13,21,34}}};
            compressedEdit.attributesKnown=true;
            compressedEdit.finder[0]='Z';
            compressedEdit.datesValid=true;
            compressedEdit.created=createdTime;
            compressedEdit.modified=editedTime;
            Bytes replacementBytes(150123);
            for(size_t i=0;i<replacementBytes.size();++i)replacementBytes[i]=uint8_t((i*17)%251);
            compressedEdit.forks[0].size=replacementBytes.size();
            const auto readReplacement=[&](unsigned fork,uint64_t offset,void* data,size_t length){
                Check(!fork&&offset<=replacementBytes.size()&&length<=replacementBytes.size()-offset,
                      "Invalid compressed replacement read");
                memcpy(data,replacementBytes.data()+offset,length);
            };
            Storage::Job cancelCompression;
            unsigned firstReads=0;
            bool compressionCancelled=false;
            try{
                published->PutForTransfer(compressionFolder,compressedEdit,
                    [&](unsigned fork,uint64_t offset,void* data,size_t length){
                        readReplacement(fork,offset,data,length);
                        if(!offset&&++firstReads==2)cancelCompression.cancel();
                    },true,cancelCompression);
            }catch(const Storage::Cancelled&){compressionCancelled=true;}
            const auto oldCompressed=published->Stat(copiedCompressed.id);
            Check(compressionCancelled&&oldCompressed.compressed&&oldCompressed.identity==copiedCompressed.identity&&
                  oldCompressed.forks[0].size==decoded.size()&&oldCompressed.finder==copiedCompressed.finder&&
                  oldCompressed.created==copiedCompressed.created&&oldCompressed.modified==copiedCompressed.modified&&
                  published->GetAttributes(oldCompressed.id)==originalAttributes,
                  "Cancelled APFS compressed overwrite changed original metadata");
            published->Read(oldCompressed.id,0,0,actualCompressed.data(),actualCompressed.size());
            Check(actualCompressed==decoded,"Cancelled APFS compressed overwrite changed original bytes");
            Storage::Job replaceCompression;
            const auto compressedId=published->PutForTransfer(compressionFolder,compressedEdit,readReplacement,true,replaceCompression);
            const auto newCompressed=published->Stat(compressedId);
            Check(compressedId==copiedCompressed.id&&newCompressed.identity==copiedCompressed.identity&&newCompressed.compressed&&
                  newCompressed.forks[0].size==replacementBytes.size()&&!newCompressed.forks[1].size&&
                  newCompressed.finder==compressedEdit.finder&&newCompressed.created==createdTime&&newCompressed.modified==editedTime&&
                  published->GetAttributes(compressedId)==compressedEdit.attributes,
                  "Completed APFS overwrite lost compression, identity or metadata");
            actualCompressed.resize(replacementBytes.size());
            published->Read(compressedId,0,0,actualCompressed.data(),actualCompressed.size());
            Check(actualCompressed==replacementBytes,"Completed APFS compressed overwrite changed replacement bytes");
            MacBrowser::Entry shortCompressed;
            shortCompressed.name=u"Inline compressed";
            shortCompressed.forks[0].size=1300;
            shortCompressed.compressed=true;
            shortCompressed.finder[0]='T';
            shortCompressed.created=createdTime;
            shortCompressed.modified=editedTime;
            shortCompressed.datesValid=true;
            shortCompressed.attributes={{u"user.rename",Bytes(4301,0x52)}};
            const auto shortId=importSource->Put(2,shortCompressed,[](unsigned,uint64_t,void* bytes,size_t size){
                memset(bytes,0x42,size);
            },false);
            Storage::Job inlineCopy;
            Check(MacBrowser::Copy(*importSource,shortId,*published,2,inlineCopy)&&
                  published->FindChild(2,shortCompressed.name,shortCompressed)&&shortCompressed.compressed,
                  "APFS copy did not retain inline compression");
            Bytes actualInline(1300);
            published->Read(shortCompressed.id,0,0,actualInline.data(),actualInline.size());
            Check(actualInline==Bytes(1300,0x42),"Copied inline APFS compression changed file contents");
            MacBrowser::Entry transform;
            transform.name=u"In-place compression";
            transform.forks[0].size=replacementBytes.size();
            transform.finder[0]='C';
            transform.datesValid=true;
            transform.created=createdTime;
            transform.modified=editedTime;
            transform.attributes={{u"user.compression",Bytes(4301,0x24)}};
            const auto transformId=published->Put(2,transform,readReplacement,false);
            const auto beforeTransform=published->Stat(transformId);
            const auto verifyTransform=[&](bool compressed){
                const auto current=published->Stat(transformId);
                Check(current.identity==beforeTransform.identity&&current.compressed==compressed&&
                      current.forks[0].size==replacementBytes.size()&&!current.forks[1].size&&
                      current.finder==transform.finder&&current.created==createdTime&&current.modified==editedTime&&
                      published->GetAttributes(transformId)==transform.attributes,
                      "In-place APFS compression changed file metadata");
                Bytes actual(replacementBytes.size());
                published->Read(transformId,0,0,actual.data(),actual.size());
                Check(actual==replacementBytes,"In-place APFS compression changed file bytes");
            };
            using CompressionMode=HfsPlus::Volume::CompressionMode;
            Storage::Job cancelEncode;
            unsigned fullReports=0;
            bool encodeCancelled=false;
            try{
                published->SetCompressionMethodForJob(transformId,CompressionMode::Zlib,
                    [&](uint64_t done,uint64_t total){
                        if(done==total&&++fullReports==3)cancelEncode.cancel();
                    },cancelEncode);
            }catch(const Storage::Cancelled&){encodeCancelled=true;}
            Check(encodeCancelled,"In-place APFS compression did not cancel during staging");
            verifyTransform(false);
            const auto changeCompression=[&](CompressionMode method){
                auto operation=FileOperations::Submit(FileOperations::ChangeCompression{
                    FileOperations::Location::Mounted(published,transformId),published->Stat(transformId),
                    transform.attributes,method});
                const auto start=GetTickCount();
                while(!operation.Job()->done()){
                    Check(GetTickCount()-start<15000,"In-place APFS compression job timed out");
                    Sleep(5);
                }
                const auto result=operation.Job()->result();
                Check(!result.code,result.message.c_str());
            };
            changeCompression(CompressionMode::Zlib);
            verifyTransform(true);
            Storage::Job cancelDecode;
            bool decodeCancelled=false;
            try{
                published->SetCompressionMethodForJob(transformId,CompressionMode::Uncompressed,
                    [&](uint64_t done,uint64_t){if(done>4096)cancelDecode.cancel();},cancelDecode);
            }catch(const Storage::Cancelled&){decodeCancelled=true;}
            Check(decodeCancelled,"In-place APFS decompression did not cancel during staging");
            verifyTransform(true);
            changeCompression(CompressionMode::Uncompressed);
            verifyTransform(false);
            fprintf(report,"PASS: macOS-generated APFS in-place compression and decompression, exact bytes, metadata, and cancellation during staging.\n");
            for(const auto method:{CompressionMode::Lzvn,CompressionMode::Lzfse,
                                  CompressionMode::Lzbitmap,CompressionMode::Stored}){
                changeCompression(method);
                verifyTransform(true);
                changeCompression(CompressionMode::Uncompressed);
                verifyTransform(false);
            }
            fprintf(report,"PASS: APFS LZVN, LZFSE, LZBITMAP, and stored resource compression round trips on a macOS-generated image.\n");
            const auto importedCopy=runImport();
            Check(!importedCopy.code,importedCopy.message.c_str());
            MacBrowser::Entry copied;
            Check(published->FindChild(2,toCopy.name,copied)&&copied.created==toCopy.created&&
                  copied.modified==toCopy.modified&&copied.finder==toCopy.finder&&
                  published->GetAttributes(copied.id)==toCopy.attributes,"APFS import lost file metadata");
            for(unsigned fork=0;fork<2;++fork){
                Check(copied.forks[fork].size==toCopy.forks[fork].size,"APFS import lost a fork");
                Bytes actual(size_t(copied.forks[fork].size));
                published->Read(copied.id,fork,0,actual.data(),actual.size());
                Check(actual==Bytes(actual.size(),fork?0x7b:0x6a),"APFS imported fork differs");
            }
            MacBrowser::Entry folderMetadata;
            folderMetadata.name=u"Folder import";
            folderMetadata.directory=true;
            folderMetadata.datesValid=true;
            folderMetadata.created=createdTime;
            folderMetadata.modified=editedTime;
            folderMetadata.finder[0]=0x24;
            folderMetadata.attributes={{u"user.folder",Bytes(4300,0x71)}};
            const auto folderId=importSource->MakeDirectoryWithAttributes(2,folderMetadata);
            const auto nestedId=importSource->MakeDirectory(folderId,u"Nested");
            const auto writePayload=[](unsigned fork,uint64_t,void* data,size_t length){
                memset(data,fork?0x7b:0x6a,length);
            };
            importSource->Put(nestedId,toCopy,writePayload,false);
            importSource->FinalizeDirectory(folderId,folderMetadata,importSource->Stat(folderId),false);
            auto folderCopy=FileOperations::Submit(FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(importSource,folderId)},FileOperations::Location::Mounted(published,2)});
            const auto folderStart=GetTickCount();
            while(!folderCopy.Job()->done()){
                Check(GetTickCount()-folderStart<15000,"External APFS recursive import timed out");
                Sleep(5);
            }
            const auto folderResult=folderCopy.Job()->result();
            Check(!folderResult.code,folderResult.message.c_str());
            MacBrowser::Entry copiedFolder,nested,copiedChild;
            Check(published->FindChild(2,folderMetadata.name,copiedFolder)&&
                  copiedFolder.created==folderMetadata.created&&copiedFolder.modified==folderMetadata.modified&&
                  copiedFolder.finder==folderMetadata.finder&&published->GetAttributes(copiedFolder.id)==folderMetadata.attributes&&
                  published->FindChild(copiedFolder.id,u"Nested",nested)&&
                  published->FindChild(nested.id,toCopy.name,copiedChild),"APFS recursive copy lost folder metadata or children");

            Storage::Job cancelFolderCopy;
            cancelFolderCopy.transport([&](const Storage::Progress& progress){
                if(progress.fileBytes>toCopy.forks[0].size)cancelFolderCopy.cancel();
            },[](const std::string&){return Storage::ConflictChoice::Replace;});
            bool cancelledTree=false;
            try{MacBrowser::Copy(*importSource,folderId,*published,2,cancelFolderCopy);}
            catch(const Storage::Cancelled&){cancelledTree=true;}
            const auto retainedFolder=published->Stat(copiedFolder.id);
            Check(cancelledTree&&retainedFolder.created==copiedFolder.created&&retainedFolder.modified==copiedFolder.modified&&
                  retainedFolder.finder==copiedFolder.finder&&published->GetAttributes(copiedFolder.id)==folderMetadata.attributes,
                  "Cancelled recursive APFS copy changed destination folder metadata");
            for(unsigned fork=0;fork<2;++fork){
                Check(published->Stat(copiedChild.id).forks[fork].size==toCopy.forks[fork].size,
                      "Recursive APFS copy lost a child fork");
                Bytes actual(size_t(toCopy.forks[fork].size));
                published->Read(copiedChild.id,fork,0,actual.data(),actual.size());
                Check(actual==Bytes(actual.size(),fork?0x7b:0x6a),"Recursive APFS copy changed child fork bytes");
            }
            MacBrowser::Entry symlinkToCopy;
            symlinkToCopy.name=u"Dangling link";
            symlinkToCopy.datesValid=true;
            symlinkToCopy.created=createdTime;
            symlinkToCopy.modified=editedTime;
            const Bytes dangling{'.','.','/','m','i','s','s','i','n','g',0};
            Storage::Job createLink;
            importSource->PutSymbolicLink(2,symlinkToCopy,dangling,createLink);
            MacBrowser::Entry sourceLink,missing;
            Check(importSource->FindChild(2,symlinkToCopy.name,sourceLink)&&
                  !importSource->FindChild(2,u"missing",missing),"Could not create a dangling APFS source link");
            auto linkCopy=FileOperations::Submit(FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(importSource,sourceLink.id)},
                FileOperations::Location::Mounted(published,nested.id)});
            const auto linkStart=GetTickCount();
            while(!linkCopy.Job()->done()){
                Check(GetTickCount()-linkStart<15000,"External APFS symbolic-link copy timed out");
                Sleep(5);
            }
            const auto linkResult=linkCopy.Job()->result();
            Check(!linkResult.code,linkResult.message.c_str());
            MacBrowser::Entry copiedLink;
            Check(published->FindChild(nested.id,symlinkToCopy.name,copiedLink)&&
                  published->SymbolicLinkTarget(copiedLink.id)==std::optional<Bytes>(dangling)&&
                  !copiedLink.forks[0].size&&!copiedLink.forks[1].size&&
                  copiedLink.created==createdTime&&copiedLink.modified==editedTime,
                  "APFS copy did not retain the dangling link target and timestamps");
            Reject([&]{published->Read(copiedLink.id,0,0,nullptr,0);},"APFS copy followed a symbolic link");
            const auto linksFolder=importSource->MakeDirectory(2,u"Linked files");
            const auto aliasesFolder=importSource->MakeDirectory(linksFolder,u"Aliases");
            const auto shared=importSource->Put(linksFolder,toCopy,writePayload,false);
            importSource->MakeHardLink(shared,aliasesFolder,u"First alias");
            importSource->MakeHardLink(shared,aliasesFolder,u"Second alias");
            auto hardLinkCopy=FileOperations::Submit(FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(importSource,linksFolder)},FileOperations::Location::Mounted(published,2)});
            const auto hardLinkStart=GetTickCount();
            while(!hardLinkCopy.Job()->done()){
                Check(GetTickCount()-hardLinkStart<15000,"External APFS hard-link copy timed out");
                Sleep(5);
            }
            const auto hardLinkResult=hardLinkCopy.Job()->result();
            Check(!hardLinkResult.code,hardLinkResult.message.c_str());
            MacBrowser::Entry copiedLinks,copiedAliases,primary,firstAlias,secondAlias;
            Check(published->FindChild(2,u"Linked files",copiedLinks)&&
                  published->FindChild(copiedLinks.id,toCopy.name,primary)&&
                  published->FindChild(copiedLinks.id,u"Aliases",copiedAliases)&&
                  published->FindChild(copiedAliases.id,u"First alias",firstAlias)&&
                  published->FindChild(copiedAliases.id,u"Second alias",secondAlias)&&
                  primary.id!=firstAlias.id&&primary.id!=secondAlias.id&&firstAlias.id!=secondAlias.id&&
                  primary.linkCount==3&&firstAlias.linkCount==3&&secondAlias.linkCount==3&&
                  primary.resolvedId==firstAlias.resolvedId&&primary.resolvedId==secondAlias.resolvedId,
                  "Copied APFS aliases did not share one inode");
            for(const auto* alias:{&primary,&firstAlias,&secondAlias}){
                for(unsigned fork=0;fork<2;++fork){
                    Check(alias->forks[fork].size==toCopy.forks[fork].size,"Copied APFS hard link lost a fork");
                    Bytes actual(size_t(alias->forks[fork].size));
                    published->Read(alias->id,fork,0,actual.data(),actual.size());
                    Check(actual==Bytes(actual.size(),fork?0x7b:0x6a),"Copied APFS hard-link fork differs");
                }
            }
            Reject([&]{published->Rename(firstAlias.id,copiedAliases.id,secondAlias.name);},
                   "APFS rename overwrote a sibling name");
            published->Rename(primary.id,copiedLinks.id,u"Renamed shared file");
            published->Rename(firstAlias.id,copiedAliases.id,u"Longer renamed alias");
            published->Rename(secondAlias.id,copiedAliases.id,u"x");
            MacBrowser::Entry renamedPrimary,renamedFirst,renamedSecond;
            Check(!published->FindChild(copiedLinks.id,toCopy.name,renamedPrimary)&&
                  !published->FindChild(copiedAliases.id,u"First alias",renamedFirst)&&
                  !published->FindChild(copiedAliases.id,u"Second alias",renamedSecond)&&
                  published->FindChild(copiedLinks.id,u"Renamed shared file",renamedPrimary)&&
                  published->FindChild(copiedAliases.id,u"Longer renamed alias",renamedFirst)&&
                  published->FindChild(copiedAliases.id,u"x",renamedSecond)&&
                  renamedPrimary.id==primary.id&&renamedFirst.id==firstAlias.id&&renamedSecond.id==secondAlias.id&&
                  renamedPrimary.resolvedId==renamedFirst.resolvedId&&renamedPrimary.resolvedId==renamedSecond.resolvedId&&
                  renamedPrimary.linkCount==3,"Renaming APFS names changed their shared inode");
            for(const auto* entry:{&renamedPrimary,&renamedFirst,&renamedSecond}){
                for(unsigned fork=0;fork<2;++fork){
                    Check(entry->forks[fork].size==toCopy.forks[fork].size,"Renaming an APFS alias lost a fork");
                    Bytes actual(size_t(entry->forks[fork].size));
                    published->Read(entry->id,fork,0,actual.data(),actual.size());
                    Check(actual==Bytes(actual.size(),fork?0x7b:0x6a),"APFS hard-link rename changed shared fork bytes");
                }
            }
            MacBrowser::Entry victim=toCopy;
            victim.name=u"Replace with hard link";
            const auto victimId=published->Put(copiedAliases.id,victim,[](unsigned,uint64_t,void* bytes,size_t size){
                memset(bytes,0x21,size);
            },false);
            Storage::Job linkOverwrite;
            const auto aliasId=published->MakeHardLinkForTransfer(primary.id,copiedAliases.id,victim.name,true,linkOverwrite);
            const auto alias=published->Stat(aliasId);
            Check(aliasId!=victimId&&alias.linkCount==4&&alias.resolvedId==published->Stat(primary.id).resolvedId&&
                  published->GetAttributes(aliasId)==toCopy.attributes,
                  "APFS hard-link overwrite did not redirect the destination to the source inode");
            Reject([&]{published->Stat(victimId);},"APFS retained an overwritten file's selection");
            for(unsigned fork=0;fork<2;++fork){
                Check(alias.forks[fork].size==toCopy.forks[fork].size,"APFS hard-link overwrite lost a source fork");
                Bytes actual(size_t(alias.forks[fork].size));
                published->Read(aliasId,fork,0,actual.data(),actual.size());
                Check(actual==Bytes(actual.size(),fork?0x7b:0x6a),"APFS hard-link overwrite did not share the source fork");
            }
            auto sharedEdit=toCopy;
            sharedEdit.name=renamedFirst.name;
            sharedEdit.modified=editedTime+99;
            sharedEdit.finder[0]='W';
            sharedEdit.attributes={{u"user.shared",{8,6,7}}};
            sharedEdit.attributesKnown=true;
            const auto verifyShared=[&](bool committed){
                for(const auto id:{renamedPrimary.id,renamedFirst.id,renamedSecond.id,aliasId}){
                    const auto entry=published->Stat(id);
                    const auto& expected=committed?sharedEdit:toCopy;
                    Check(entry.resolvedId==published->Stat(renamedFirst.id).resolvedId&&entry.linkCount==4&&
                          entry.created==expected.created&&entry.modified==expected.modified&&
                          entry.finder==expected.finder&&published->GetAttributes(id)==expected.attributes,
                          "Shared APFS overwrite changed the wrong inode or metadata");
                    for(unsigned fork=0;fork<2;++fork){
                        Check(entry.forks[fork].size==expected.forks[fork].size,"Shared APFS overwrite changed a fork size");
                        Bytes actual(size_t(entry.forks[fork].size));
                        published->Read(id,fork,0,actual.data(),actual.size());
                        const auto fill=committed?(fork?0x4d:0x3c):(fork?0x7b:0x6a);
                        Check(actual==Bytes(actual.size(),fill),"Shared APFS overwrite left inconsistent fork bytes");
                    }
                }
            };
            Storage::Job cancelShared;
            bool sharedCancelled=false;
            try{
                published->PutForTransfer(copiedAliases.id,sharedEdit,[&](unsigned fork,uint64_t,void* bytes,size_t size){
                    memset(bytes,0x41,size);
                    if(fork)cancelShared.cancel();
                },true,cancelShared);
            }catch(const Storage::Cancelled&){sharedCancelled=true;}
            Check(sharedCancelled,"APFS alias overwrite did not cancel during resource-fork staging");
            verifyShared(false);
            Storage::Job writeShared;
            Check(published->PutForTransfer(copiedAliases.id,sharedEdit,[](unsigned fork,uint64_t,void* bytes,size_t size){
                memset(bytes,fork?0x4d:0x3c,size);
            },true,writeShared)==renamedFirst.id,"APFS overwrite replaced the alias identity");
            verifyShared(true);
            auto replacement=copied;
            replacement.forks[0].size=9001;
            replacement.forks[1].size=6001;
            replacement.finder[0]='R';
            replacement.modified=editedTime+17;
            replacement.attributes={{u"user.replacement",{31,42}}};
            replacement.attributesKnown=true;
            Storage::Job cancelOverwrite;
            bool cancelled=false;
            try{
                published->PutForTransfer(2,replacement,[&](unsigned fork,uint64_t offset,void* data,size_t length){
                    memset(data,0x55,length);
                    if(fork)cancelOverwrite.cancel();
                },true,cancelOverwrite);
            }catch(const Storage::Cancelled&){cancelled=true;}
            const auto retained=published->Stat(copied.id);
            Check(cancelled&&retained.identity==copied.identity&&retained.modified==copied.modified&&
                  retained.created==copied.created&&retained.finder==copied.finder&&
                  published->GetAttributes(copied.id)==toCopy.attributes,"Cancelled APFS overwrite changed file metadata");
            for(unsigned fork=0;fork<2;++fork){
                Check(retained.forks[fork].size==toCopy.forks[fork].size,"Cancelled APFS overwrite changed fork size");
                Bytes actual(size_t(retained.forks[fork].size));
                published->Read(retained.id,fork,0,actual.data(),actual.size());
                Check(actual==Bytes(actual.size(),fork?0x7b:0x6a),"Cancelled APFS overwrite changed fork bytes");
            }
            Storage::Job overwrite;
            const auto replaced=published->PutForTransfer(2,replacement,[](unsigned fork,uint64_t offset,void* data,size_t length){
                memset(data,fork?0x44:0x55,length);
            },true,overwrite);
            const auto updatedFile=published->Stat(replaced);
            Check(replaced==copied.id&&updatedFile.identity==copied.identity&&updatedFile.finder==replacement.finder&&
                  updatedFile.modified==replacement.modified&&published->GetAttributes(replaced)==replacement.attributes,
                  "Completed APFS overwrite lost identity or replacement metadata");
            for(unsigned fork=0;fork<2;++fork){
                Check(updatedFile.forks[fork].size==replacement.forks[fork].size,"Completed APFS overwrite lost a fork");
                Bytes actual(size_t(updatedFile.forks[fork].size));
                published->Read(replaced,fork,0,actual.data(),actual.size());
                Check(actual==Bytes(actual.size(),fork?0x44:0x55),"Completed APFS overwrite has incorrect fork contents");
            }
            MacBrowser::Entry renamedFolder;
            auto rename=FileOperations::Submit(FileOperations::RenameEntry{
                FileOperations::Location::Mounted(published,publishedFolder.id),u"Browser renamed folder"});
            const auto renameStart=GetTickCount();
            while(!rename.Job()->done()){
                Check(GetTickCount()-renameStart<15000,"External APFS browser rename timed out");
                Sleep(5);
            }
            const auto renamed=rename.Job()->result();
            Check(!renamed.code,renamed.message.c_str());
            Check(published->FindChild(2,u"Browser renamed folder",renamedFolder)&&renamedFolder.id==publishedFolder.id&&
                  published->List(renamedFolder.id).size()==400,"External APFS browser rename lost the folder or children");
            auto create=FileOperations::Submit(FileOperations::CreateFolder{
                FileOperations::Location::Mounted(published,renamedFolder.id),u"New APFS folder"});
            const auto createStart=GetTickCount();
            while(!create.Job()->done()){
                Check(GetTickCount()-createStart<15000,"External APFS folder creation timed out");
                Sleep(5);
            }
            const auto creation=create.Job()->result();
            Check(!creation.code,creation.message.c_str());
            MacBrowser::Entry child;
            Check(published->FindChild(renamedFolder.id,u"New APFS folder",child)&&child.directory&&
                  published->List(child.id).empty(),"External APFS folder did not reopen empty");
            Reject([&]{published->MakeDirectory(renamedFolder.id,u"NEW APFS FOLDER");},
                   "APFS folder creation accepted a case-insensitive collision");
            const auto erase=[&](uint32_t id){
                auto operation=FileOperations::Submit(FileOperations::DeleteEntries{
                    {FileOperations::Location::Mounted(published,id)}});
                const auto start=GetTickCount();
                while(!operation.Job()->done()){
                    Check(GetTickCount()-start<15000,"External APFS deletion timed out");
                    Sleep(5);
                }
                return operation.Job()->result();
            };
            const auto renameCompressed=[&](const MacBrowser::Entry& entry,const std::u16string& name,const Bytes& expected){
                const auto attributes=published->GetAttributes(entry.id);
                auto operation=FileOperations::Submit(FileOperations::RenameEntry{
                    FileOperations::Location::Mounted(published,entry.id),name});
                const auto start=GetTickCount();
                while(!operation.Job()->done()){
                    Check(GetTickCount()-start<15000,"External compressed APFS rename timed out");
                    Sleep(5);
                }
                const auto result=operation.Job()->result();
                Check(!result.code,result.message.c_str());
                MacBrowser::Entry renamed;
                Check(!published->FindChild(entry.parent,entry.name,renamed)&&
                      published->FindChild(entry.parent,name,renamed)&&renamed.id==entry.id&&
                      renamed.identity==entry.identity&&renamed.compressed&&renamed.finder==entry.finder&&
                      renamed.created==entry.created&&renamed.modified==entry.modified&&
                      renamed.forks[0].size==expected.size()&&renamed.forks[1].size==entry.forks[1].size&&
                      published->GetAttributes(renamed.id)==attributes,
                      "APFS compressed rename changed file metadata or identity");
                Bytes actual(expected.size());
                published->Read(renamed.id,0,0,actual.data(),actual.size());
                Check(actual==expected,"APFS compressed rename changed file contents");
            };
            MacBrowser::Entry deletedCompressed,unrelatedCompressed;
            Check(published->FindChild(2,compressed.name,unrelatedCompressed),"Missing streamed APFS compression fixture");
            constexpr auto renamedStream=u"Renamed macOS compressed file.txt";
            constexpr auto renamedInline=u"Renamed inline compressed file";
            renameCompressed(unrelatedCompressed,renamedStream,decoded);
            renameCompressed(shortCompressed,renamedInline,actualInline);
            fprintf(report,"PASS: macOS APFS compressed-file rename with inline and stream-backed headers, exact bytes, metadata, and checkpoint cancellation.\n");
            const auto deletedInline=erase(shortCompressed.id);
            Check(!deletedInline.code,deletedInline.message.c_str());
            Check(!published->FindChild(2,renamedInline,deletedCompressed),
                  "APFS did not remove the inline compressed file");
            const auto deletedTree=erase(compressionFolder);
            Check(!deletedTree.code,deletedTree.message.c_str());
            Check(!published->FindChild(2,u"Compressed copies",deletedCompressed)&&
                  published->FindChild(2,renamedStream,unrelatedCompressed)&&unrelatedCompressed.compressed,
                  "APFS recursive deletion removed an unrelated compressed file");
            Bytes untouchedCompressed(decoded.size());
            published->Read(unrelatedCompressed.id,0,0,untouchedCompressed.data(),untouchedCompressed.size());
            Check(untouchedCompressed==decoded,"APFS deletion changed an unrelated compressed file");
            const auto compressedAttributes=published->GetAttributes(unrelatedCompressed.id);
            const auto verifyStreamDecompression=[&](bool stillCompressed){
                const auto current=published->Stat(unrelatedCompressed.id);
                Check(current.identity==unrelatedCompressed.identity&&current.compressed==stillCompressed&&
                      current.name==unrelatedCompressed.name&&current.finder==unrelatedCompressed.finder&&
                      current.created==unrelatedCompressed.created&&current.modified==unrelatedCompressed.modified&&
                      current.forks[0].size==decoded.size()&&!current.forks[1].size&&
                      published->GetAttributes(current.id)==compressedAttributes,
                      "APFS streamed-header decompression changed file metadata");
                Bytes bytes(decoded.size());
                published->Read(current.id,0,0,bytes.data(),bytes.size());
                Check(bytes==decoded,"APFS streamed-header decompression changed file contents");
            };
            Storage::Job cancelStreamDecode;
            bool streamDecodeCancelled=false;
            try{
                published->SetCompressionMethodForJob(unrelatedCompressed.id,CompressionMode::Uncompressed,
                    [&](uint64_t done,uint64_t){if(done)cancelStreamDecode.cancel();},cancelStreamDecode);
            }catch(const Storage::Cancelled&){streamDecodeCancelled=true;}
            Check(streamDecodeCancelled,"APFS streamed-header decompression did not cancel during staging");
            verifyStreamDecompression(true);
            Storage::Job streamDecode;
            published->SetCompressionMethodForJob(unrelatedCompressed.id,CompressionMode::Uncompressed,{},streamDecode);
            verifyStreamDecompression(false);
            fprintf(report,"PASS: genuine macOS APFS streamed-header decompression, exact bytes, metadata, and cancellation.\n");
            Check(!erase(aliasId).code&&!published->FindChild(copiedAliases.id,victim.name,firstAlias),
                  "Deleting an APFS alias did not remove its name");
            auto retainedAlias=published->Stat(primary.id);
            Check(retainedAlias.linkCount==3&&retainedAlias.hardLink,"Deleting one APFS name changed the wrong inode");
            const auto verifyForks=[&](const MacBrowser::Entry& entry){
                for(unsigned fork=0;fork<2;++fork){
                    Check(entry.forks[fork].size==toCopy.forks[fork].size,"APFS unlink lost a fork");
                    Bytes actual(size_t(entry.forks[fork].size));
                    published->Read(entry.id,fork,0,actual.data(),actual.size());
                    Check(actual==Bytes(actual.size(),fork?0x4d:0x3c),"APFS unlink changed shared fork bytes");
                }
            };
            verifyForks(retainedAlias);
            Check(!erase(copiedAliases.id).code&&!published->FindChild(copiedLinks.id,u"Aliases",copiedAliases)&&
                  published->FindChild(copiedLinks.id,u"Renamed shared file",retainedAlias)&&
                  retainedAlias.linkCount==1&&!retainedAlias.hardLink,
                  "Recursive APFS deletion did not preserve the alias outside the removed folder");
            verifyForks(retainedAlias);
            MacBrowser::Entry tree,kept,subfolder;
            Check(published->FindChild(2,u"Cancel tree",tree)&&
                  published->FindChild(tree.id,u"Kept child.bin",kept),"Missing recursive deletion fixture");
            const auto subfolderId=published->MakeDirectory(tree.id,u"Nested empty folder");
            MacBrowser::Entry liveLink,treeLink;
            liveLink.name=u"Link to unrelated file";
            const auto targetBytes=[](std::string_view path){
                Bytes result(path.begin(),path.end());
                result.push_back(0);
                return result;
            };
            Storage::Job links;
            published->PutSymbolicLink(2,liveLink,targetBytes("Created data.bin"),links);
            published->PutSymbolicLink(tree.id,liveLink,targetBytes("../Created data.bin"),links);
            Check(published->FindChild(2,liveLink.name,liveLink)&&liveLink.symbolicLink&&
                  published->FindChild(tree.id,liveLink.name,treeLink)&&treeLink.symbolicLink,
                  "Missing APFS links to an existing file");
            Check(!erase(liveLink.id).code&&!published->FindChild(2,liveLink.name,subfolder),
                  "External APFS browser did not delete the symbolic link");
            auto lockedChild=kept;
            lockedChild.protectedFile=true;
            published->SetBasicMetadata(kept.id,lockedChild);
            Check(erase(tree.id).code&&published->FindChild(tree.id,u"Kept child.bin",kept)&&
                  published->FindChild(tree.id,u"Nested empty folder",subfolder)&&subfolder.id==subfolderId,
                  "Recursive APFS deletion changed a tree containing a locked child");
            lockedChild=kept;
            lockedChild.protectedFile=false;
            published->SetBasicMetadata(kept.id,lockedChild);
            Check(!erase(tree.id).code&&!published->FindChild(2,u"Cancel tree",subfolder),
                  "External APFS browser did not recursively delete the tree");
            Reject([&]{published->Stat(kept.id);},"Deleted APFS descendant still resolved");
            Reject([&]{published->Stat(subfolderId);},"Deleted APFS folder still resolved");
            Reject([&]{published->Stat(treeLink.id);},"Recursively deleted APFS symbolic link still resolved");
            Check(!erase(child.id).code&&!published->FindChild(renamedFolder.id,u"New APFS folder",child),
                  "External APFS browser did not delete the empty folder");
            Check(!erase(replaced).code&&!published->FindChild(2,toCopy.name,child),
                  "External APFS browser did not delete the overwritten file");
            Bytes unrelated(importedBytes.size());
            published->Read(imported.id,0,0,unrelated.data(),unrelated.size());
            Check(unrelated==importedBytes,"APFS symbolic-link deletion changed the referenced file bytes");
            renamedFolder=published->Stat(renamedFolder.id);
            auto desired=renamedFolder;
            desired.modified=editedTime;
            desired.protectedFile=true;
            desired.finder[0]=0x12;
            auto edit=FileOperations::Submit(FileOperations::EditBasicMetadata{
                FileOperations::Location::Mounted(published,renamedFolder.id),renamedFolder,desired});
            const auto editStart=GetTickCount();
            while(!edit.Job()->done()){
                Check(GetTickCount()-editStart<15000,"External APFS Properties operation timed out");
                Sleep(5);
            }
            const auto result=edit.Job()->result();
            Check(!result.code,result.message.c_str());
            const auto updated=published->Stat(renamedFolder.id);
            Check(updated.modified==editedTime&&updated.protectedFile&&updated.finder==desired.finder,
                  "External APFS Properties did not preserve Finder metadata and apply the verified edits");
            desired=updated;
            desired.finder[0]^=1;
            Reject([&]{published->SetBasicMetadata(updated.id,desired);},"Locked APFS folder accepted a Finder metadata edit");
            desired.protectedFile=false;
            published->SetBasicMetadata(updated.id,desired);
            Check(published->Stat(updated.id).finder==desired.finder,"APFS did not replace Finder information");
            desired=published->Stat(updated.id);
            desired.finder={};
            published->SetBasicMetadata(updated.id,desired);
            Check(published->Stat(updated.id).finder==desired.finder,"APFS did not clear Finder information");
            desired=published->Stat(updated.id);
            desired.finder[0]=0x12;
            published->SetBasicMetadata(updated.id,desired);
            Check(published->Stat(updated.id).finder==desired.finder&&published->List(updated.id).size()==400,
                  "APFS Finder information edit lost unrelated folder contents");
            Bytes originalHardLink(size_t(a.forks[0].size)),reopenedHardLink(originalHardLink.size());
            authentic->Read(a.id,0,0,originalHardLink.data(),originalHardLink.size());
            published->Read(publishedB.id,0,0,reopenedHardLink.data(),reopenedHardLink.size());
            Check(reopenedHardLink==originalHardLink,"APFS hard-link Properties changed the shared fork");
            MacBrowser::Entry renamedLink;
            Check(!published->FindChild(2,u"link.txt",renamedLink)&&
                  published->FindChild(2,u"Renamed symbolic link",renamedLink)&&
                  published->SymbolicLinkTarget(renamedLink.id)==authentic->SymbolicLinkTarget(symlink.id),
                  "APFS rename changed the symbolic-link target");
            symlinkToCopy.name=renamedLink.name;
            importSource->PutSymbolicLink(2,symlinkToCopy,dangling,createLink);
            Check(importSource->FindChild(2,symlinkToCopy.name,sourceLink),"Missing APFS replacement link");
            auto replaceLink=FileOperations::Submit(FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(importSource,sourceLink.id)},FileOperations::Location::Mounted(published,2)});
            const auto replaceStart=GetTickCount();
            while(!replaceLink.Job()->done()){
                Check(GetTickCount()-replaceStart<15000,"External APFS symbolic-link overwrite timed out");
                const auto [id,message]=replaceLink.Job()->conflictState();
                if(!message.empty())replaceLink.Job()->resolve(id,Storage::ConflictChoice::Replace);
                Sleep(5);
            }
            const auto replacedLink=replaceLink.Job()->result();
            Check(!replacedLink.code,replacedLink.message.c_str());
            const auto changedLink=published->Stat(renamedLink.id);
            Check(changedLink.identity==renamedLink.identity&&
                  published->SymbolicLinkTarget(changedLink.id)==std::optional<Bytes>(dangling)&&
                  !changedLink.forks[0].size&&!changedLink.forks[1].size,
                  "APFS overwrite changed link identity or materialized its target");
            Reject([&]{published->Read(changedLink.id,0,0,nullptr,0);},
                   "APFS overwrite followed the replacement symbolic-link target");
            published->Read(publishedFile.id,0,0,data,sizeof(data));
            Check(!memcmp(data,"entry 222\n",sizeof(data)),"Publishing a checkpoint changed macOS-generated file data");
            MacBrowser::Entry small,originalSmall;
            Check(!published->FindChild(2,u"small.txt",small)&&published->FindChild(2,u"x",small)&&
                  authentic->FindChild(2,u"small.txt",originalSmall)&&
                  small.protectedFile&&small.modified==editedTime&&small.created==createdTime&&
                  small.forks[0].size==originalSmall.forks[0].size,"APFS timestamp edit changed the wrong fields");
            Bytes originalBytes(size_t(small.forks[0].size)),editedBytes(originalBytes.size());
            authentic->Read(originalSmall.id,0,0,originalBytes.data(),originalBytes.size());
            published->Read(small.id,0,0,editedBytes.data(),editedBytes.size());
            Check(editedBytes==originalBytes,"APFS timestamp edit changed the target file contents");
            fprintf(report,"PASS: macOS-generated APFS checkpoint publication, cancellation, file dates, lock edits, folder Properties, file and symbolic-link renames, symbolic-link overwrite and cancellation.\n");
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
            auto committed=MacBrowser::OpenBrowserImage(extracted);
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
            committed.reset();
            SnapshotLiveWrites(extracted,report,u"Folder",u"sample.txt");
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
        const auto symlinkPath=VolumeCoord::Wide(root+"apfs-replaced-symlink.img");
        {
            auto image=CreateFormatOutput(symlinkPath.c_str(),8*1024*1024);
            Storage::Job job;
            Apfs::Format(*image,u"APFS symbolic link copies",false,job);
            image->Keep();
        }
        const auto confirmLink=[](FileOperations::Operation& operation,Storage::ConflictChoice choice){
            const auto limit=GetTickCount64()+15000;
            while(operation.Job()->conflict().empty()&&WaitForSingleObject(operation.Job()->event(),0)!=WAIT_OBJECT_0&&
                  GetTickCount64()<limit)Sleep(1);
            Check(!operation.Job()->conflict().empty(),"APFS symbolic link did not prompt before overwrite");
            operation.Job()->resolve(choice);
            Check(WaitForSingleObject(operation.Job()->event(),15000)==WAIT_OBJECT_0,"APFS link overwrite timed out");
            return operation.Job()->result();
        };
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
            const auto destination=MacBrowser::OpenBrowserImage(symlinkPath);
            Check(destination&&destination->Writable(),"Could not mount the writable APFS link fixture");
            auto copy=FileOperations::Submit(FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(volume,file.id)},FileOperations::Location::Mounted(destination,2)});
            Check(WaitForSingleObject(copy.Job()->event(),15000)==WAIT_OBJECT_0,"APFS symbolic link copy timed out");
            const auto result=copy.Job()->result();
            Check(bool(result.code)==(link==Symlink::Malformed),result.message.c_str());
            MacBrowser::Entry copied;
            Check(destination->List().size()==1&&destination->FindChild(2,file.name,copied)&&
                  destination->PropertiesDetails(copied.id)=="Symbolic link to /a/b\\x0a",
                  "Copy changed a symbolic link or followed its target");
            if(link==Symlink::Valid){
                auto duplicate=FileOperations::Submit(FileOperations::OrdinaryCopy{
                    {FileOperations::Location::Mounted(volume,file.id)},FileOperations::Location::Mounted(destination,2)});
                Check(confirmLink(duplicate,Storage::ConflictChoice::Cancel).code&&
                      destination->Stat(copied.id).name==file.name,"APFS symbolic link copy replaced an existing entry");
                Storage::Job job;
                Reject([&]{MacBrowser::Copy(*volume,file.id,*targetMac,2,job);},
                       "APFS symbolic link was flattened into an unsupported destination");
            }
        }
        if(const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE")){
            const auto source=MacBrowser::OpenBrowserImage(fixture);
            const auto destination=MacBrowser::OpenBrowserImage(symlinkPath);
            MacBrowser::Entry link,copied;
            Check(source&&destination&&source->FindChild(2,u"link.txt",link),"Missing genuine APFS symbolic link fixture");
            VolumeCoord::OperationLease lease({{source->ResourceKey(),VolumeCoord::Mode::Read},
                                               {destination->ResourceKey(),VolumeCoord::Mode::Write}});
            Storage::Job job;
            Check(MacBrowser::Copy(*source,link.id,*destination,2,job)&&
                  destination->FindChild(2,link.name,copied)&&
                  destination->SymbolicLinkTarget(copied.id)==source->SymbolicLinkTarget(link.id),
                  "Copying the genuine APFS symbolic link changed its target");
        }
        {
            const auto image=MacBrowser::OpenBrowserImage(symlinkPath);
            MacBrowser::Entry original;
            Check(image&&image->FindChild(2,u"Long \u00e9.txt",original),"Could not reopen the APFS symbolic link");
            const auto target=image->SymbolicLinkTarget(original.id);
            uint32_t folder=0;
            {
                VolumeCoord::OperationLease lease({{image->ResourceKey(),VolumeCoord::Mode::Write}});
                folder=image->MakeDirectory(2,u"Moved links");
                Storage::Job job;
                image->PutSymbolicLink(folder,original,Bytes{'o','l','d',0},job);
            }
            const auto submit=[](FileOperations::Request request){
                auto work=FileOperations::Submit(std::move(request));
                Check(WaitForSingleObject(work.Job()->event(),15000)==WAIT_OBJECT_0,"APFS link operation timed out");
                return work.Job()->result();
            };
            auto cut=FileOperations::Location::Mounted(image,original.id);
            cut.cut=true;
            const auto destination=FileOperations::Location::Mounted(image,folder);
            auto cancelled=FileOperations::Submit(FileOperations::OrdinaryCopy{{cut},destination});
            Check(confirmLink(cancelled,Storage::ConflictChoice::Cancel).code&&
                  image->SymbolicLinkTarget(original.id)==target,"Cancelled APFS cut removed or changed the source link");
            MacBrowser::Entry duplicate;
            Check(image->FindChild(folder,original.name,duplicate)&&
                  image->SymbolicLinkTarget(duplicate.id)==std::optional<Bytes>(Bytes{'o','l','d',0}),
                  "Cancelled APFS overwrite changed the destination target");
            auto overwrite=FileOperations::Submit(FileOperations::OrdinaryCopy{
                {FileOperations::Location::Mounted(image,original.id)},destination});
            const auto replaced=confirmLink(overwrite,Storage::ConflictChoice::Replace);
            Check(!replaced.code&&image->Stat(duplicate.id).identity==duplicate.identity&&
                  image->SymbolicLinkTarget(duplicate.id)==target,"APFS overwrite did not preserve destination identity");
            const auto deleted=submit(FileOperations::DeleteEntries{{FileOperations::Location::Mounted(image,duplicate.id)}});
            Check(!deleted.code,deleted.message.c_str());
            const auto moved=submit(FileOperations::OrdinaryCopy{{cut},destination});
            Check(!moved.code,moved.message.c_str());
            MacBrowser::Entry result;
            Check(!image->FindChild(2,original.name,duplicate)&&image->FindChild(folder,original.name,result)&&
                  image->SymbolicLinkTarget(result.id)==target,"APFS cut did not preserve the target and remove the source");
            const auto renamed=submit(FileOperations::RenameEntry{FileOperations::Location::Mounted(image,result.id),u"Renamed link"});
            Check(!renamed.code&&image->FindChild(folder,u"Renamed link",result)&&
                  image->SymbolicLinkTarget(result.id)==target,"APFS rename did not retain the link target");
        }
        fprintf(report,"PASS: APFS symbolic link copy, cut, rename, deletion, and read refusal.\n");
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
        const auto badPath=VolumeCoord::Wide(root+"bad-sibling-map.img");
        {
            auto damaged=BrowseFixture(false,true,false,Symlink::None,true);
            auto image=CreateFormatOutput(badPath.c_str(),damaged.Size());
            image->Write(0,damaged.bytes.data(),damaged.bytes.size());image->Flush();image->Keep();
        }
        auto damaged=MacBrowser::OpenBrowserImage(badPath);
        bool rejected=false;
        try{damaged->List(damaged->List().at(0).id);}
        catch(const std::exception& e){rejected=std::string(e.what())=="APFS sibling map does not match its inode";}
        Check(rejected,"Incorrect APFS sibling map was accepted");
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
        const auto apfsCopyPath=VolumeCoord::Wide(root+"apfs-copied-hard-links.img");
        {
            const auto baseline=VolumeCoord::Wide(root+"checkpoint-renamed-catalog.img");
            const auto image=OpenImage(baseline.c_str(),false);
            const auto bytes=Disk::Read(*image,0,size_t(image->Size()));
            auto output=CreateFormatOutput(apfsCopyPath.c_str(),bytes.size());
            output->Write(0,bytes.data(),bytes.size());
            output->Flush();
            output->Keep();
        }
        {
            auto source=OpenImage(apfsCopyPath.c_str(),true);
            auto destination=Apfs::OpenWritable(source,VolumeCoord::ImageKey(apfsCopyPath));
            Check(destination&&destination->Writable(),"Could not open writable APFS copy fixture");
            auto lockedAlias=sourceAlias;
            lockedAlias.protectedFile=true;
            Reject([&]{destination->ValidateEntry(lockedAlias);},"APFS accepted a locked hard-link group in preflight");
            Storage::Job copy;
            Check(MacBrowser::Copy(*linked,sourceFolder.id,*destination,2,copy),
                  "Copying hard-link groups into APFS failed");
            MacBrowser::Entry directory,alias;
            Check(destination->FindChild(2,u"Folder",directory)&&
                  destination->FindChild(directory.id,u"Alias A",alias),"Missing APFS overwrite source");
            MacBrowser::Entry victim;
            victim.name=u"Replace me";
            victim.forks[0].size=5000;
            victim.forks[1].size=6200;
            const auto old=destination->Put(directory.id,victim,[](unsigned,uint64_t,void* data,size_t size){
                memset(data,0x55,size);
            },false);
            const auto replacement=destination->MakeHardLink(alias.id,directory.id,u"Replace me",true);
            Check(replacement!=old&&destination->Stat(replacement).resolvedId==destination->Stat(alias.id).resolvedId,
                  "APFS overwrite did not publish a new hard-link identity");
            Reject([&]{destination->Stat(old);},"APFS retained the overwritten file's browser row");
            Check(destination->MakeHardLink(alias.id,directory.id,u"Replace me",true)==replacement,
                  "APFS overwrite of an existing same-inode alias changed identity");
        }
        const auto copiedVolume=MacBrowser::OpenBrowserImage(apfsCopyPath);
        MacBrowser::Entry copiedFolder,copiedA,copiedB,copiedOriginal,copiedOther;
        Check(copiedVolume&&copiedVolume->FindChild(2,u"Folder",copiedFolder)&&
              copiedVolume->FindChild(copiedFolder.id,u"Alias A",copiedA)&&
              copiedVolume->FindChild(copiedFolder.id,u"Alias B",copiedB)&&
              copiedVolume->FindChild(copiedFolder.id,withMetadata.name,copiedOriginal)&&
              copiedVolume->FindChild(copiedFolder.id,u"Other",copiedOther)&&
              copiedA.id!=copiedOriginal.id&&copiedB.id!=copiedOther.id&&
              copiedA.linkCount==3&&copiedB.linkCount==2&&copiedA.resolvedId==copiedOriginal.resolvedId&&
              copiedB.resolvedId==copiedOther.resolvedId&&copiedA.resolvedId!=copiedB.resolvedId,
              "APFS transfer did not preserve both hard-link groups after reopen");
        MacBrowser::Entry replacement;
        Check(copiedVolume->FindChild(copiedFolder.id,u"Replace me",replacement)&&
              replacement.resolvedId==copiedA.resolvedId&&!replacement.forks[1].size,
              "APFS overwrite left the old resource fork or lost its shared inode");
        for(const auto* entry:{&copiedA,&copiedB}){
            MacBrowser::Entry sourceEntry;
            Check(linked->FindChild(sourceFolder.id,entry->name,sourceEntry)&&
                  sourceEntry.forks[0].size==entry->forks[0].size,"Copied APFS hard link changed size");
            Bytes expected(size_t(entry->forks[0].size)),actual(expected.size());
            linked->Read(sourceEntry.id,0,0,expected.data(),expected.size());
            copiedVolume->Read(entry->id,0,0,actual.data(),actual.size());
            Check(actual==expected,"Copied APFS hard-link data differs");
        }
        fprintf(report,"PASS: copying two distinct hard-link groups into writable APFS preserves both shared inodes.\n");
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
