// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "ClassicHfsRecovery.h"
#include "MacBrowserModel.h"
#include "VolumeSession.h"
#include "OperationCompletion.h"
#include "WorkerTransport.h"
#include "VolumeCoordinator.h"
#include "ReadOnlyCdBackend.h"
#include "DiskImageOpen.h"
#include "DiskPartitions.h"
#include "NdifImage.h"
#include <cstdio>
#include <vector>
extern "C" {
#include "hfs/libhfs/hfs.h"
}

namespace {
using Bytes=std::vector<uint8_t>;
void Check(bool ok,const char* message) { if(!ok)throw HfsPlus::Error(message); }

MacBrowser::Entry Entry(bool updated)
{
    MacBrowser::Entry entry{};
    entry.name=u"Document";
    entry.forks[0].size=updated?49003:13009;
    entry.forks[1].size=updated?7431:2001;
    memcpy(entry.finder.data(),updated?"BINANEW!":"TEXTOLD!",8);
    entry.created=3500000000U;
    entry.modified=updated?3500003000U:3500001000U;
    entry.datesValid=true;
    return entry;
}
uint8_t Byte(bool updated,unsigned fork,uint64_t at)
{
    return uint8_t((updated?173:31)+fork*61+at%251);
}
void Put(MacBrowser::Volume& volume,bool updated,bool cancelling=false,
         uint32_t parent=2,const char16_t* name=u"Document")
{
    auto entry=Entry(updated);
    entry.name=name;
    volume.Put(parent,entry,[&](unsigned fork,uint64_t offset,void* data,size_t size){
        if(cancelling&&fork)throw Storage::Cancelled();
        auto* output=static_cast<uint8_t*>(data);
        for(size_t i=0;i<size;++i)output[i]=Byte(updated,fork,offset+i);
    },updated);
}
void VerifyForks(MacBrowser::Volume& volume,const MacBrowser::Entry& actual,bool updated)
{
    const auto expected=Entry(updated);
    for(unsigned fork=0;fork<2;++fork){
        Check(actual.forks[fork].size==expected.forks[fork].size,"reopened HFS fork has the wrong length");
        Bytes bytes(size_t(actual.forks[fork].size));
        volume.Read(actual.id,fork,0,bytes.data(),bytes.size());
        for(size_t i=0;i<bytes.size();++i)
            Check(bytes[i]==Byte(updated,fork,i),"reopened HFS fork has different bytes");
    }
}
void VerifyFile(MacBrowser::Volume& volume,uint32_t parent,const char16_t* name,bool updated,unsigned modified=0)
{
    MacBrowser::Entry actual{};
    auto expected=Entry(updated);
    if(modified)expected.modified=modified;
    Check(volume.FindChild(parent,name,actual)&&actual.name==name,"reopened HFS image lost the expected file name");
    Check(actual.finder==expected.finder&&actual.created==expected.created&&actual.modified==expected.modified,
          "reopened HFS image changed Finder information or timestamps");
    VerifyForks(volume,actual,updated);
}
void Verify(MacBrowser::Volume& volume,bool updated)
{
    Check(volume.List(2).size()==1,"reopened HFS image retained a temporary or backup entry");
    VerifyFile(volume,2,u"Document",updated);
}
uint32_t Find(MacBrowser::Volume& volume,uint32_t parent,const char16_t* name)
{
    MacBrowser::Entry entry{};
    Check(volume.FindChild(parent,name,entry),"missing delete recovery fixture entry");
    return entry.id;
}
Bytes Load(const CString& path)
{
    CFile file;
    Check(file.Open(path,CFile::modeRead|CFile::shareDenyNone)!=FALSE,"open scratch image for comparison");
    Bytes bytes(size_t(file.GetLength()));
    Check(file.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"read scratch image for comparison");
    return bytes;
}
void Save(const CString& path,const Bytes& bytes)
{
    CFile file;
    Check(file.Open(path,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create a scratch recovery image");
    file.Write(bytes.data(),UINT(bytes.size()));
}
void CrashChild(const CString& image,unsigned phase,const wchar_t* action=L"overwrite")
{
    wchar_t executable[MAX_PATH]{};
    const auto length=GetModuleFileNameW(nullptr,executable,MAX_PATH);
    Check(length&&length<MAX_PATH,"locate recovery test executable");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --classic-recovery-child \""+
                         VolumeCoord::Wide(image.GetString())+L"\" "+std::to_wstring(phase)+L" "+action;
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                         nullptr,nullptr,&startup,&child)!=FALSE,"start classic recovery crash subprocess");
    CloseHandle(child.hThread);
    const auto wait=WaitForSingleObject(child.hProcess,45000);
    DWORD exit=0;
    if(wait!=WAIT_OBJECT_0)TerminateProcess(child.hProcess,3);
    GetExitCodeProcess(child.hProcess,&exit);
    CloseHandle(child.hProcess);
    if(wait!=WAIT_OBJECT_0||exit!=90+phase)
        throw HfsPlus::Error("Classic recovery subprocess missed crash phase "+std::to_string(phase)+
                             " (exit "+std::to_string(exit)+")");
}
void DeleteRecovery(const CString& root,const Bytes& original,FILE* report)
{
    const CString seed=root+"\\delete-seed.dsk";
    Save(seed,original);
    {
        auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(seed.GetString()));
        const auto tree=volume->MakeDirectory(2,u"Tree");
        const auto nested=volume->MakeDirectory(tree,u"Nested");
        Put(*volume,true,false,tree,u"Top");
        Put(*volume,false,false,nested,u"Leaf");
        Put(*volume,true,false,2,u"Unrelated");
        volume->Close();
    }
    const auto before=Load(seed);
    const auto verifyTree=[](MacBrowser::Volume& volume){
        const auto tree=Find(volume,2,u"Tree");
        const auto nested=Find(volume,tree,u"Nested");
        VerifyFile(volume,tree,u"Top",true);
        VerifyFile(volume,nested,u"Leaf",false);
        VerifyFile(volume,2,u"Unrelated",true);
        return tree;
    };
    for(unsigned phase:{2u,3u}){
        CString image;
        image.Format("%s\\delete-file-%u.dsk",root.GetString(),phase);
        Save(image,before);
        const auto path=VolumeCoord::Wide(image.GetString());
        CrashChild(image,phase,L"delete");
        Check(ClassicHfsRecovery::Pending(path)&&Load(image)!=before,"file deletion did not leave recoverable sectors");
        auto volume=MacBrowser::OpenImage(path);
        Check(volume&&volume->Writable(),"file deletion did not recover writable");
        MacBrowser::Entry removed;
        if(phase==2)VerifyFile(*volume,2,u"Document",false);
        else Check(!volume->FindChild(2,u"Document",removed),"durably deleted file returned after reopen");
        verifyTree(*volume);
        volume->Close();
        Check(!ClassicHfsRecovery::Pending(path),"file deletion recovery left its journal pending");
        if(phase==2)Check(Load(image)==before,"file deletion rollback changed image bytes");
    }
    const CString image=root+"\\delete-tree.dsk";
    Save(image,before);
    const auto path=VolumeCoord::Wide(image.GetString());
    auto volume=MacBrowser::OpenBrowserImage(path);
    Check(volume&&volume->Writable(),"mount delete tree through volume worker");
    const auto pid=VolumeSession::VolumeSessionPidForTest(volume);
    Check(pid!=0,"delete tree arrived without a mounted worker");
    const auto tree=verifyTree(*volume);
    VolumeSession::CrashVolumeSessionDuringRemoveTreeForTest(volume);
    bool crashed=false;
    try { Storage::Job job;MacBrowser::RemoveTree(*volume,tree,job,true); }
    catch(const HfsPlus::Error&) { crashed=true; }
    Check(crashed&&WorkerTransport::lastExitCode==96&&ClassicHfsRecovery::Pending(path)&&Load(image)!=before,
          "volume worker did not crash after deleting a subtree child");
    volume->Reload();
    Check(volume->Writable()&&VolumeSession::VolumeSessionPidForTest(volume)!=pid,
          "browser could not recover the durable tree delete after worker exit");
    const auto recovered=verifyTree(*volume);
    VerifyFile(*volume,2,u"Document",false);
    Check(!ClassicHfsRecovery::Pending(path)&&Load(image)==before,"tree rollback did not restore every image byte");

    Storage::Job cancellation;
    cancellation.transport([&](const Storage::Progress& progress){
        if(progress.files) cancellation.cancel();
    },{});
    bool cancelled=false;
    try { MacBrowser::RemoveTree(*volume,recovered,cancellation,true); }
    catch(const Storage::OperationError& error) { cancelled=error.error==ERROR_CANCELLED; }
    catch(const Storage::Cancelled&) { cancelled=true; }
    Check(cancelled&&volume->Writable(),"subtree cancellation did not restore the writable mount");
    const auto retry=verifyTree(*volume);
    Check(!ClassicHfsRecovery::Pending(path)&&Load(image)==before,"subtree cancellation retained changes");

    Storage::Job success;
    MacBrowser::RemoveTree(*volume,retry,success,true);
    MacBrowser::Entry deleted;
    Check(!volume->FindChild(2,u"Tree",deleted),"completed subtree removal retained the tree");
    VerifyFile(*volume,2,u"Unrelated",true);
    VerifyFile(*volume,2,u"Document",false);
    volume.reset();
    volume=MacBrowser::OpenImage(path);
    Check(volume&&volume->Writable()&&!volume->FindChild(2,u"Tree",deleted)&&
          !ClassicHfsRecovery::Pending(path),"committed subtree deletion changed after reopen");
    VerifyFile(*volume,2,u"Unrelated",true);
    VerifyFile(*volume,2,u"Document",false);
    volume->Close();
    fprintf(report,"PASS: classic HFS file deletion before/after commit and recursive worker crash, cancellation and retry preserve unrelated files.\n");
}

void RenameRecovery(const CString& root,const Bytes& original,FILE* report)
{
    const CString seed=root+"\\rename-seed.dsk";
    Save(seed,original);
    uint32_t document=0,bundle=0,leaf=0;
    {
        auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(seed.GetString()));
        document=Find(*volume,2,u"Document");
        const auto from=volume->MakeDirectory(2,u"From");
        volume->MakeDirectory(2,u"To");
        bundle=volume->MakeDirectory(from,u"Bundle");
        Put(*volume,true,false,bundle,u"Leaf");
        leaf=Find(*volume,bundle,u"Leaf");
        Put(*volume,true,false,2,u"Unrelated");
        volume->Close();
    }
    const auto before=Load(seed);
    const auto verifyMove=[&](MacBrowser::Volume& volume,bool moved){
        const auto from=Find(volume,2,u"From"),to=Find(volume,2,u"To");
        const auto parent=moved?to:from;
        MacBrowser::Entry other;
        Check(Find(volume,parent,u"Bundle")==bundle&&volume.Stat(bundle).parent==parent&&
              !volume.FindChild(moved?from:to,u"Bundle",other),"folder move did not preserve its catalog ID and correct parent");
        Check(Find(volume,bundle,u"Leaf")==leaf,"folder move changed a child's catalog ID");
        VerifyFile(volume,bundle,u"Leaf",true);
        VerifyFile(volume,2,u"Unrelated",true);
    };
    for(const auto* action:{L"rename",L"move",L"properties"}){
        for(unsigned phase:{2u,3u}){
            CString image;
            image.Format("%s\\rename-%ls-%u.dsk",root.GetString(),action,phase);
            Save(image,before);
            const auto path=VolumeCoord::Wide(image.GetString());
            CrashChild(image,phase,action);
            Check(ClassicHfsRecovery::Pending(path)&&Load(image)!=before,"rename or move left no recoverable journal");
            auto volume=MacBrowser::OpenImage(path);
            Check(volume&&volume->Writable(),"rename or move did not recover writable");
            const bool committed=phase==3;
            const bool move=wcscmp(action,L"move")==0;
            const bool properties=wcscmp(action,L"properties")==0;
            const auto* expected=committed&&!move?(properties?u"Properties name":u"DOCUMENT"):u"Document";
            Check(Find(*volume,2,expected)==document,"rename changed the file's catalog ID");
            VerifyFile(*volume,2,expected,false,committed&&properties?3500001060U:0);
            verifyMove(*volume,committed&&move);
            volume->Close();
            Check(!ClassicHfsRecovery::Pending(path),"rename or move left its journal pending after recovery");
            if(!committed)Check(Load(image)==before,"rename or move rollback did not restore every byte");
        }
    }

    const CString image=root+"\\rename-mounted-move.dsk";
    Save(image,before);
    const auto path=VolumeCoord::Wide(image.GetString());
    auto volume=MacBrowser::OpenBrowserImage(path);
    Check(volume&&volume->Writable(),"mount classic move fixture in worker");
    const auto pid=VolumeSession::VolumeSessionPidForTest(volume);
    Check(pid!=0,"move fixture opened without its worker");
    VolumeSession::CrashVolumeSessionOnRenameForTest(volume);
    bool crashed=false;
    try{volume->MoveHere(*volume,bundle,Find(*volume,2,u"To"),volume->Stat(bundle));}
    catch(const HfsPlus::Error&){crashed=true;}
    Check(crashed&&WorkerTransport::lastExitCode==93&&ClassicHfsRecovery::Pending(path),
          "mounted move missed the durable commit checkpoint");
    volume->Reload();
    Check(volume->Writable()&&VolumeSession::VolumeSessionPidForTest(volume)!=pid&&
          !ClassicHfsRecovery::Pending(path),"browser did not recover the committed move after worker exit");
    verifyMove(*volume,true);
    VerifyFile(*volume,2,u"Document",false);
    fprintf(report,"PASS: classic HFS case-only and Properties renames and same-volume folder moves recover before and after commit, preserving IDs, forks, metadata and unrelated files.\n");
}

void CreationRecovery(const CString& root,const Bytes& original,FILE* report)
{
    const CString folderSeed=root+"\\creation-folder-seed.dsk";
    Save(folderSeed,original);
    uint64_t initialModified=0;
    {
        auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(folderSeed.GetString()));
        initialModified=volume->Stat(volume->MakeDirectory(2,u"New folder")).modified;
        volume->Close();
    }
    const auto folderBefore=Load(folderSeed);
    for(const auto* action:{L"create-file",L"create-folder",L"finalize-folder"}){
        const bool file=wcscmp(action,L"create-file")==0;
        const bool finalize=wcscmp(action,L"finalize-folder")==0;
        const auto& before=finalize?folderBefore:original;
        for(unsigned phase:{2u,3u}){
            CString image;
            image.Format("%s\\%ls-%u.dsk",root.GetString(),action,phase);
            Save(image,before);
            const auto path=VolumeCoord::Wide(image.GetString());
            CrashChild(image,phase,action);
            Check(ClassicHfsRecovery::Pending(path)&&Load(image)!=before,
                  "creation or folder finalization left no recovery record");
            auto volume=MacBrowser::OpenImage(path);
            Check(volume&&volume->Writable(),"creation recovery did not reopen writable");
            const bool committed=phase==3;
            MacBrowser::Entry created{};
            if(file&&committed)VerifyFile(*volume,2,u"New file",false);
            else if(finalize || (!file&&committed)){
                Check(volume->FindChild(2,u"New folder",created)&&created.directory&&volume->List(created.id).empty(),
                      "expected an empty folder after reopening the image");
                if(finalize)Check(created.modified==(committed?3500002000U:initialModified),
                                  "folder finalization did not recover its recorded state");
            }else{
                Check(!volume->FindChild(2,file?u"New file":u"New folder",created),
                      "uncommitted creation remained visible after recovery");
            }
            VerifyFile(*volume,2,u"Document",false);
            volume->Close();
            Check(!ClassicHfsRecovery::Pending(path),"creation recovery left a pending journal");
            if(!committed)Check(Load(image)==before,"creation rollback did not restore every byte");
        }
    }
    const CString cancelled=root+"\\create-cancelled.dsk";
    Save(cancelled,original);
    const auto path=VolumeCoord::Wide(cancelled.GetString());
    auto volume=MacBrowser::OpenImage(path);
    bool stopped=false;
    try{Put(*volume,false,true,2,u"New file");}
    catch(const Storage::Cancelled&){stopped=true;}
    Check(stopped&&volume->Writable(),"cancelled new file did not restore the writable mount");
    Verify(*volume,false);
    volume->Close();
    Check(!ClassicHfsRecovery::Pending(path)&&Load(cancelled)==original,
          "cancelled creation did not restore the exact image");
    fprintf(report,"PASS: classic HFS file and folder creation, imported-folder finalization and cancelled new files recover before and after commit.\n");
}

hfsdirent Properties(MacBrowser::Volume& volume,uint32_t id)
{
    hfsdirent entry{};
    Check(volume.GetClassicProperties(id,entry),"read classic metadata recovery fixture");
    return entry;
}
hfsdirent EditedProperties(hfsdirent entry)
{
    entry.fdflags=short((entry.fdflags&~14)|10);
    entry.fdlocation.h=123;
    entry.fdlocation.v=-45;
    entry.crdate=1400000000;
    entry.mddate=1400000100;
    entry.bkdate=1400000200;
    if(!(entry.flags&HFS_ISDIR)){
        strcpy_s(entry.u.file.type,"BINA");
        strcpy_s(entry.u.file.creator,"EDIT");
    }
    return entry;
}
void VerifyProperties(MacBrowser::Volume& volume,uint32_t id,const hfsdirent& expected)
{
    const auto actual=Properties(volume,id);
    Check(actual.cnid==expected.cnid&&actual.parid==expected.parid&&
          !strcmp(actual.name,expected.name)&&actual.fdflags==expected.fdflags&&
          actual.fdlocation.h==expected.fdlocation.h&&actual.fdlocation.v==expected.fdlocation.v&&
          actual.crdate==expected.crdate&&actual.mddate==expected.mddate&&actual.bkdate==expected.bkdate,
          "reopened classic HFS entry has different identity, Finder information or dates");
    if(!(expected.flags&HFS_ISDIR))
        Check(!strcmp(actual.u.file.type,expected.u.file.type)&&
              !strcmp(actual.u.file.creator,expected.u.file.creator),
              "reopened classic HFS file has different Finder type or creator");
}
void MetadataRecovery(const CString& root,const Bytes& original,FILE* report)
{
    const CString seed=root+"\\metadata-seed.dsk";
    Save(seed,original);
    hfsdirent file{},folder{};
    {
        auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(seed.GetString()));
        const auto directory=volume->MakeDirectory(2,u"Metadata folder");
        Put(*volume,true,false,directory,u"Child");
        file=Properties(*volume,Find(*volume,2,u"Document"));
        folder=Properties(*volume,directory);
        volume->Close();
    }
    const auto before=Load(seed);
    for(const auto* action:{L"metadata-file",L"metadata-folder",L"metadata-merge"}){
        const bool editFile=wcscmp(action,L"metadata-file")==0;
        const bool merge=wcscmp(action,L"metadata-merge")==0;
        for(unsigned phase:{2u,3u}){
            CString image;
            image.Format("%s\\%ls-%u.dsk",root.GetString(),action,phase);
            Save(image,before);
            const auto path=VolumeCoord::Wide(image.GetString());
            CrashChild(image,phase,action);
            Check(ClassicHfsRecovery::Pending(path)&&Load(image)!=before,"metadata edit left no recoverable journal");
            auto volume=MacBrowser::OpenImage(path);
            Check(volume&&volume->Writable(),"metadata recovery did not reopen writable");
            const auto id=Find(*volume,2,u"Document");
            const auto directory=Find(*volume,2,u"Metadata folder");
            auto expectedFile=file;
            auto expectedFolder=folder;
            if(phase==3){
                if(editFile)expectedFile=EditedProperties(file);
                else if(merge){
                    const auto desired=EditedProperties(folder);
                    expectedFolder.crdate=desired.crdate;
                    expectedFolder.mddate=desired.mddate;
                }else expectedFolder=EditedProperties(folder);
            }
            VerifyProperties(*volume,id,expectedFile);
            VerifyProperties(*volume,directory,expectedFolder);
            VerifyForks(*volume,volume->Stat(id),false);
            VerifyFile(*volume,directory,u"Child",true);
            volume->Close();
            Check(!ClassicHfsRecovery::Pending(path),"metadata recovery left a pending journal");
            if(phase==2)Check(Load(image)==before,"metadata rollback did not restore every image byte");
        }
    }
    fprintf(report,"PASS: classic HFS metadata-only file and folder edits and merge timestamps recover before and after commit, preserving both forks and unrelated entries.\n");
}

void VerifyOutside(const Bytes& before,const Bytes& after,uint64_t start,uint64_t length)
{
    Check(before.size()==after.size()&&start<=before.size()&&length<=before.size()-start&&
          std::equal(before.begin(),before.begin()+size_t(start),after.begin())&&
          std::equal(before.begin()+size_t(start+length),before.end(),after.begin()+size_t(start+length)),
          "NDIF recovery or commit changed bytes outside the selected HFS partition");
}
void NdifRecovery(const CString& root,FILE* report)
{
    const wchar_t* fixtures=_wgetenv(L"HFV_NDIF_INTERCHANGE_FIXTURES");
    if(!fixtures||!*fixtures){
        fprintf(report,"SKIP: set HFV_NDIF_INTERCHANGE_FIXTURES to the shared NDIF Interchange folder.\n");
        return;
    }
    for(unsigned partitioned:{0u,1u}){
        const auto fixture=std::wstring(fixtures)+(partitioned?L"\\partitioned-classic.bin":L"\\writable-HFS.bin");
        CString seed;
        seed.Format("%s\\ndif-seed-%u.bin",root.GetString(),partitioned);
        const auto seedPath=VolumeCoord::Wide(seed.GetString());
        Check(CopyFileW(fixture.c_str(),seedPath.c_str(),TRUE)!=FALSE,"copy the shared writable NDIF sample to scratch");
        const auto original=Load(seed);
        uint64_t start=0,length=0;
        {
            auto opened=DiskImage::Open(seedPath,{});
            const auto backing=Ndif::RecoveryRange(*opened.source);
            const auto volume=DiskPartitions::FindLegacy(*opened.source);
            Check(opened.container==DiskImage::Container::Ndif&&backing&&volume&&
                  volume->filesystem==DiskPartitions::Filesystem::Classic&&
                  bool(volume->partition.index)==bool(partitioned),"shared NDIF sample has the wrong layout");
            start=backing->offset+(partitioned?volume->partition.offset:0);
            length=partitioned?volume->partition.length:backing->length;
        }
        {
            auto volume=MacBrowser::OpenImage(seedPath);
            Check(volume&&volume->Writable()&&!strcmp(volume->Kind(),"HFS"),"open shared RdWr scratch image writable");
            Put(*volume,false);
            Put(*volume,true,false,2,u"Unrelated");
            if(partitioned){
                const auto folder=volume->MakeDirectory(2,u"Tree");
                Put(*volume,false,false,folder,u"Child");
            }
            volume->Close();
        }
        const auto before=Load(seed);
        VerifyOutside(original,before,start,length);
        for(const auto* action:{L"overwrite",L"delete"}){
            for(unsigned phase:{2u,3u}){
                CString image;
                image.Format("%s\\ndif-%u-%ls-%u.bin",root.GetString(),partitioned,action,phase);
                Save(image,before);
                const auto path=VolumeCoord::Wide(image.GetString());
                CrashChild(image,phase,action);
                Check(ClassicHfsRecovery::Pending(path,true)&&!ClassicHfsRecovery::Pending(path)&&Load(image)!=before,
                      "interrupted NDIF change did not leave a container recovery record");
                if(!partitioned&&phase==2&&wcscmp(action,L"overwrite")==0){
                    // A changed wrapper must leave the pending transaction intact.
                    const auto replaceHeaderByte=[&](BYTE value){
                        CFile file;
                        Check(file.Open(image,CFile::modeWrite|CFile::shareExclusive)!=FALSE,
                              "open scratch NDIF header");
                        file.Write(&value,1);
                        file.Flush();
                    };
                    replaceHeaderByte(0x7f);
                    const auto damaged=Load(image);
                    bool rejected=false;
                    try { MacBrowser::OpenImage(path); }
                    catch(const HfsPlus::Error&){rejected=true;}
                    Check(rejected&&ClassicHfsRecovery::Pending(path,true)&&Load(image)==damaged,
                          "missing NDIF wrapper did not fence pending recovery");
                    replaceHeaderByte(before[0]);
                }
                auto volume=MacBrowser::OpenBrowserImage(path);
                Check(volume&&volume->Writable()&&!strcmp(volume->Kind(),"HFS"),"browser did not recover writable NDIF");
                MacBrowser::Entry item{};
                if(phase==3&&wcscmp(action,L"delete")==0)
                    Check(!volume->FindChild(2,u"Document",item),"committed delete returned after reopening NDIF");
                else VerifyFile(*volume,2,u"Document",phase==3);
                VerifyFile(*volume,2,u"Unrelated",true);
                volume.reset();
                const auto after=Load(image);
                Check(!ClassicHfsRecovery::Pending(path,true),"NDIF recovery left a pending record");
                VerifyOutside(before,after,start,length);
                if(phase==2)Check(after==before,"interrupted NDIF change did not restore the exact container bytes");
            }
        }
        if(partitioned){
            const CString image=root+"\\ndif-worker-delete.bin";
            Save(image,before);
            const auto path=VolumeCoord::Wide(image.GetString());
            auto volume=MacBrowser::OpenBrowserImage(path);
            const auto pid=VolumeSession::VolumeSessionPidForTest(volume);
            const auto tree=Find(*volume,2,u"Tree");
            VolumeSession::CrashVolumeSessionDuringRemoveTreeForTest(volume);
            bool crashed=false;
            try{Storage::Job job;MacBrowser::RemoveTree(*volume,tree,job,true);}
            catch(const HfsPlus::Error&){crashed=true;}
            Check(crashed&&WorkerTransport::lastExitCode==96&&ClassicHfsRecovery::Pending(path,true),
                  "NDIF worker did not stop during recursive deletion");
            volume->Reload();
            Check(volume->Writable()&&VolumeSession::VolumeSessionPidForTest(volume)!=pid,
                  "NDIF worker did not resume writable after interrupted delete");
            VerifyFile(*volume,Find(*volume,2,u"Tree"),u"Child",false);
            VerifyFile(*volume,2,u"Unrelated",true);
            bool cancelled=false;
            try{Put(*volume,true,true);}
            catch(const Storage::Cancelled&){cancelled=true;}
            Check(cancelled&&volume->Writable(),"NDIF cancelled overwrite did not retain writable access");
            VerifyFile(*volume,2,u"Document",false);
            volume.reset();
            Check(!ClassicHfsRecovery::Pending(path,true)&&Load(image)==before,
                  "NDIF worker recovery or cancellation altered the original container");
        }
    }
    fprintf(report,"PASS: shared writable NDIF images recover interrupted overwrites and deletes, including partitioned NDIF, worker restart, and cancelled overwrite.\n");
}

}

int RunClassicRecoveryChild(const char* image,unsigned phase,const char* action)
{
    try{
        readonly_cd_backend_init();
        ClassicHfsRecovery::Transaction::CrashAt(phase);
        auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(image));
        Check(bool(volume),"open classic HFS image in subprocess");
        const auto document=Find(*volume,2,u"Document");
        if(!strcmp(action,"delete"))volume->Remove(document);
        else if(!strcmp(action,"rename"))volume->Rename(document,2,u"DOCUMENT");
        else if(!strcmp(action,"move")){
            const auto folder=Find(*volume,Find(*volume,2,u"From"),u"Bundle");
            volume->MoveHere(*volume,folder,Find(*volume,2,u"To"),volume->Stat(folder));
        }else if(!strcmp(action,"properties")){
            hfsdirent properties{};
            Check(volume->GetClassicProperties(document,properties),"read Properties before rename");
            strcpy_s(properties.name,"Properties name");
            properties.mddate+=60;
            volume->SetClassicProperties(document,properties);
        }else if(!strcmp(action,"create-file"))Put(*volume,false,false,2,u"New file");
        else if(!strcmp(action,"create-folder"))volume->MakeDirectory(2,u"New folder");
        else if(!strcmp(action,"finalize-folder")){
            const auto id=Find(*volume,2,u"New folder");
            const auto expected=volume->Stat(id);
            auto desired=expected;
            desired.datesValid=true;
            desired.created=3500000000U;
            desired.modified=3500002000U;
            volume->FinalizeDirectory(id,desired,expected,false);
        }else if(!strcmp(action,"metadata-file")||!strcmp(action,"metadata-folder")){
            const auto id=!strcmp(action,"metadata-file")?document:Find(*volume,2,u"Metadata folder");
            const auto original=Properties(*volume,id);
            FileOperations::EditClassicProperties change{FileOperations::Location::Mounted(volume,id),
                FileOperations::ClassicPropertyEdit{original,EditedProperties(original)}};
            Storage::Job job;
            FileOperations::Completion completion;
            const auto result=FileOperations::ExecuteInWorker(change,job,completion);
            if(result.code)throw HfsPlus::Error(result.message);
        }else if(!strcmp(action,"metadata-merge")){
            const auto id=Find(*volume,2,u"Metadata folder");
            const auto expected=volume->Stat(id);
            auto desired=expected;
            const auto properties=EditedProperties(Properties(*volume,id));
            desired.datesValid=true;
            desired.created=uint64_t(properties.crdate)+2082844800ULL;
            desired.modified=uint64_t(properties.mddate)+2082844800ULL;
            // A merge restores the dates while retaining the destination's Finder fields.
            desired.finder[9]^=0x0e;
            volume->FinalizeDirectory(id,desired,expected,true);
        }else if(phase!=5)Put(*volume,true);
        volume->Close();
        return 3;
    }catch(...){return 4;}
}

int RunClassicRecoveryTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const CString root=directory,seed=root+"\\seed.dsk";
    FILE* report=nullptr;
    if(fopen_s(&report,root+"\\classic-recovery-report.txt","w")||!report)return 2;
    int result=0;
    try{
        readonly_cd_backend_init();
        Save(seed,Bytes(4*1024*1024));
        CString formatPath=seed;
        char label[]="Undo";
        Check(hfs_format(formatPath.GetBuffer(),0,label)==0,"format classic HFS recovery image");
        formatPath.ReleaseBuffer();
        {
            auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(seed.GetString()));
            Check(volume&&volume->Writable(),"open writable classic HFS recovery fixture");
            Put(*volume,false);
            Verify(*volume,false);
            volume->Close();
        }
        const auto original=Load(seed);
        for(unsigned phase:{1u,4u,2u,3u}){
            CString image;
            image.Format("%s\\crash-%u.dsk",root.GetString(),phase);
            Save(image,original);
            const auto path=VolumeCoord::Wide(image.GetString());
            CrashChild(image,phase);
            Check(ClassicHfsRecovery::Pending(path)&&Load(image)!=original,"crash left no interrupted overwrite on disk");
            if(phase!=3){
                bool rejected=false;
                try { MacBrowser::OpenClassicImage(path,false); }
                catch(const HfsPlus::Error&){ rejected=true; }
                Check(rejected&&ClassicHfsRecovery::Pending(path),"read-only open consumed pending rollback");
            }
            if(phase==4)CrashChild(image,5);
            if(phase==2){
                const auto stream=path+L":HFVExplorer.ClassicUndo";
                auto handle=CreateFileW(stream.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
                Check(handle!=INVALID_HANDLE_VALUE,"open scratch journal for incomplete tail test");
                DWORD written=0;
                const BYTE partial[]={0,1,2};
                LARGE_INTEGER end{};
                const bool appended=SetFilePointerEx(handle,end,nullptr,FILE_END)&&
                                    WriteFile(handle,partial,sizeof(partial),&written,nullptr)&&
                                    written==sizeof(partial)&&FlushFileBuffers(handle);
                CloseHandle(handle);
                Check(appended,"append a partial recovery record");
            }

            // Resolve the recovery stream through a different name for the same file.
            auto reopen=path;
            if(phase==4){
                reopen=VolumeCoord::Wide((root+"\\alias.dsk").GetString());
                Check(CreateHardLinkW(reopen.c_str(),path.c_str(),nullptr)!=FALSE,"create recovery hard-link alias");
            }
            {
                auto volume=phase==4?MacBrowser::OpenBrowserImage(reopen):MacBrowser::OpenImage(reopen);
                Check(volume&&volume->Writable(),"recover and reopen classic HFS image writable");
                Verify(*volume,phase==3);
                volume->Close();
            }
            Check(!ClassicHfsRecovery::Pending(path),"reopened image left its recovery stream pending");
            if(phase!=3)Check(Load(image)==original,"rollback did not restore exact image bytes");
            fprintf(report,"PASS: subprocess crash phase %u restored the correct forks, Finder metadata and timestamps.\n",phase);
        }
        {
            const auto path=VolumeCoord::Wide(seed.GetString());
            auto volume=MacBrowser::OpenImage(path);
            bool cancelled=false;
            try { Put(*volume,true,true); }
            catch(const Storage::Cancelled&){ cancelled=true; }
            Check(cancelled&&volume->Writable(),"cancelled overwrite did not restore the writable mount");
            Verify(*volume,false);
            volume->Close();
            Check(!ClassicHfsRecovery::Pending(path)&&Load(seed)==original,
                  "cancelled overwrite did not restore the exact image");
        }
        fprintf(report,"PASS: interrupted recovery is repeatable through a hard-link alias; cancellation restores the original bytes.\n");
        DeleteRecovery(root,original,report);
        RenameRecovery(root,original,report);
        CreationRecovery(root,original,report);
        MetadataRecovery(root,original,report);
        NdifRecovery(root,report);
    }catch(const std::exception& error){
        fprintf(report,"FAIL: %s\n",error.what());
        result=1;
    }
    fclose(report);
    return result;
}
#endif
