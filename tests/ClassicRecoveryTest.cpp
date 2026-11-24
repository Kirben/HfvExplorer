// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HfsPlusDialogs.h"
#include "LauncherWriteBack.h"
#include "ApfsFormat.h"
#include "ApfsVolume.h"
#include "ClassicCopyArchive.h"
#include "ClassicHfsRecovery.h"
#include "ClassicFormatCommand.h"
#include "MacBrowserModel.h"
#include "VolumeSession.h"
#include "OperationCompletion.h"
#include "WorkerTransport.h"
#include "VolumeCoordinator.h"
#include "ReadOnlyCdBackend.h"
#include "DiskImageOpen.h"
#include "DiskPartitions.h"
#include "BrowserPresentation.h"
#include "WindowsMoveUndo.h"
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
constexpr uint64_t FormatBytes=1474560;
constexpr char FormatLabel[]="Verified";

Storage::Result MoveWindowsTree(const std::wstring& image,bool importing,Storage::Job& job,bool converted,bool persistent=false,bool copyOnly=false)
{
    auto classic=persistent?MacBrowser::OpenBrowserImage(image):MacBrowser::OpenImage(image);
    auto windows=FileOperations::Location::Windows(image+L".windows");
    auto root=FileOperations::Location::Mounted(classic,2);
    auto tree=importing?FileOperations::Location::Windows(image+L".windows\\Tree"):
                        FileOperations::Location::Mounted(classic,Find(*classic,2,u"Tree"));
    tree.cut=!copyOnly;
    FileOperations::Completion completion;
    if(converted){
        LegacyCopyOptions options;
        options.input='a'; // Automatic conversion recognizes the AppleDouble companion.
        return FileOperations::ExecuteInWorker(FileOperations::LegacyConversion{
            tree,importing?root:windows,true,true,false,options,FileOperations::ConversionTarget::Folder},job,completion);
    }
    return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{
        {tree},importing?root:windows},job,completion);
}

void WindowsMoveRecovery(const CString& root,const Bytes& blank,FILE* report)
{
    for(const bool importing:{false,true}){
        for(const unsigned phase:{0U,7U,8U,9U,10U}){
            CString image;
            image.Format("%s\\%s-%u.dsk",root.GetString(),importing?"windows-import":"windows-export",phase);
            Save(image,blank);
            const auto path=VolumeCoord::Wide(image.GetString());
            const auto hostPath=path+L".windows";
            Check(CreateDirectoryW(hostPath.c_str(),nullptr)!=FALSE,"create Windows move fixture");
            {
                auto hfs=MacBrowser::OpenImage(path);
                auto host=MacBrowser::OpenWindows(hostPath);
                auto& source=importing?*host:*hfs;
                auto& target=importing?*hfs:*host;
                Put(source,false,false,2,u"Source neighbor");
                Put(target,false,false,2,u"Target neighbor");
                const auto incoming=source.MakeDirectory(2,u"Tree");
                const auto existing=target.MakeDirectory(2,u"Tree");
                Put(source,true,false,incoming);
                Put(source,false,false,source.MakeDirectory(incoming,u"New folder"),u"New entry");
                Put(target,false,false,existing);
                Put(target,false,false,existing,u"Preserved");
            }
            const auto before=Load(image);
            if(!phase){
                Storage::Job job;
                job.transport([&](const Storage::Progress& progress){
                    if(progress.files==1)job.cancel();
                },[](const std::string&){return Storage::ConflictChoice::Replace;});
                const auto result=MoveWindowsTree(path,importing,job,false,true);
                if(result.outcome!=Storage::Outcome::Cancelled||result.rollback!=Storage::Rollback::Restored||result.partial)
                    throw HfsPlus::Error("Cancelled Windows/HFS move did not restore both participants: "+result.message);
            }else if(phase==10){
                Storage::Job job;
                job.transport({},[](const std::string&){return Storage::ConflictChoice::Replace;});
                const auto result=MoveWindowsTree(path,importing,job,importing,true);
                if(result.code||!result.moved)
                    throw HfsPlus::Error("Persistent browser move did not finish: "+result.message);
            }else{
                CrashChild(image,phase,importing?L"windows-import":L"windows-export");
                Check(ClassicHfsRecovery::Pending(path),"interrupted Windows/HFS move lost HFS recovery");
            }
            if(!importing&&phase==9){
                WIN32_FIND_DATAA found{};
                const auto search=FindFirstFileA(image+".windows\\.hfv-move-*",&found);
                Check(search!=INVALID_HANDLE_VALUE,"locate interrupted Windows staging");
                FindClose(search);
                const CString staged=image+".windows\\"+found.cFileName+"\\Unrelated";
                const Bytes unrelated{9,7,5};
                Save(staged,unrelated);
                bool rejected=false;
                try{auto open=MacBrowser::OpenImage(path);}
                catch(const HfsPlus::Error&){rejected=true;}
                Check(rejected&&ClassicHfsRecovery::Pending(path)&&Load(staged)==unrelated,
                      "committed recovery deleted an untracked file in staging");
                Check(MoveFileA(staged,image+".staged-unrelated")!=FALSE,"move foreign staging file aside");
                fprintf(report,"PASS: committed Windows recovery retained an untracked file in staging.\n");
            }
            if(importing&&phase==9){
                WIN32_FIND_DATAA found{};
                auto search=FindFirstFileA(image+".windows\\.hfv-move-*",&found);
                Check(search!=INVALID_HANDLE_VALUE,"locate the staged Windows import");
                FindClose(search);
                const CString stage=image+".windows\\"+found.cFileName;
                search=FindFirstFileA(stage+"\\*",&found);
                Check(search!=INVALID_HANDLE_VALUE,"locate the staged Windows source folder");
                CString nested;
                do{
                    if((found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&strcmp(found.cFileName,".")&&strcmp(found.cFileName,"..")){
                        nested=stage+"\\"+found.cFileName+"\\New folder";
                        break;
                    }
                }while(FindNextFileA(search,&found));
                FindClose(search);
                Check(!nested.IsEmpty(),"missing staged Windows source tree");
                const CString intruder=nested+"\\Unrelated";
                const Bytes unrelated{4,6,8};
                Save(intruder,unrelated);
                bool rejected=false;
                try{auto open=MacBrowser::OpenImage(path);}
                catch(const HfsPlus::Error&){rejected=true;}
                Check(rejected&&ClassicHfsRecovery::Pending(path)&&Load(intruder)==unrelated,
                      "committed recovery removed an untracked nested Windows file");
                Check(MoveFileA(intruder,image+".nested-unrelated")!=FALSE,
                      "move the nested Windows file aside for recovery retry");
                fprintf(report,"PASS: committed recovery retained an untracked file inside a staged Windows source tree.\n");
            }
            if(!importing&&phase==8){
                const CString journal=image+":HFVExplorer.ClassicUndo";
                const CString published=image+".windows\\Tree\\Document";
                const CString companion=image+".windows\\Tree\\%Document";
                const auto validJournal=Load(journal);
                const auto movedData=Load(published),movedResource=Load(companion),movedImage=Load(image);
                Check(!validJournal.empty(),"interrupted move has no HFS undo data");
                auto damaged=validJournal;
                damaged.back()^=0x80;
                Save(journal,damaged);
                bool invalidRejected=false;
                try{auto open=MacBrowser::OpenImage(path);}
                catch(const HfsPlus::Error&){invalidRejected=true;}
                Check(invalidRejected&&ClassicHfsRecovery::Pending(path)&&Load(journal)==damaged&&
                      Load(image)==movedImage&&Load(published)==movedData&&Load(companion)==movedResource,
                      "invalid HFS journal changed the image or Windows side before validation");
                Save(journal,validJournal);
                fprintf(report,"PASS: a damaged classic HFS move journal leaves both participants untouched.\n");
                const CString intruder=image+".windows\\Tree\\New folder\\Unrelated";
                const CString saved=image+".external-file";
                const Bytes external{1,2,3,4};
                Save(intruder,external);
                bool rejected=false;
                try{auto open=MacBrowser::OpenImage(path);}
                catch(const HfsPlus::Error&){rejected=true;}
                Check(rejected&&ClassicHfsRecovery::Pending(path)&&Load(intruder)==external,
                      "recovery removed an unrelated file from the newly published folder");
                Check(MoveFileA(intruder,saved)!=FALSE,"move the unrelated Windows file aside for retry");

                const CString output=image+".windows\\Tree\\Document";
                const CString moved=image+".published-file";
                Check(MoveFileA(output,moved)!=FALSE,"simulate a competing edit to the published Windows name");
                Save(output,external);
                rejected=false;
                try{auto open=MacBrowser::OpenImage(path);}
                catch(const HfsPlus::Error&){rejected=true;}
                Check(rejected&&ClassicHfsRecovery::Pending(path)&&Load(output)==external,
                      "recovery overwrote a competing Windows file");
                Check(DeleteFileA(output)&&MoveFileA(moved,output),"restore the published file for recovery retry");
                const CString stream=published+":unrelated";
                const auto times=CreateFileA(published,FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
                FILETIME date{};
                Check(times!=INVALID_HANDLE_VALUE&&GetFileTime(times,nullptr,nullptr,&date),
                      "capture Windows output timestamp before adding a stream");
                const Bytes unrelated{9,7,5,3};
                Save(stream,unrelated);
                Check(SetFileTime(times,nullptr,nullptr,&date)!=FALSE,"restore the original publication timestamp");
                CloseHandle(times);
                bool streamRejected=false;
                try{auto open=MacBrowser::OpenImage(path);}
                catch(const HfsPlus::Error&){streamRejected=true;}
                Check(streamRejected&&ClassicHfsRecovery::Pending(path)&&Load(stream)==unrelated&&
                      Load(published)==movedData,"recovery discarded a newly added NTFS stream");
                Check(DeleteFileA(stream)!=FALSE,"remove the injected stream before replay");
                const auto restoredTime=CreateFileA(published,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,
                    nullptr,OPEN_EXISTING,0,nullptr);
                Check(restoredTime!=INVALID_HANDLE_VALUE&&SetFileTime(restoredTime,nullptr,nullptr,&date),
                      "restore the publication timestamp for retry");
                CloseHandle(restoredTime);
                fprintf(report,"PASS: aborted recovery retained a Windows output with a newly added NTFS stream.\n");
            }
            auto hfs=MacBrowser::OpenImage(path);
            auto host=MacBrowser::OpenWindows(hostPath);
            auto& source=importing?*host:*hfs;
            auto& target=importing?*hfs:*host;
            Check(!ClassicHfsRecovery::Pending(path),"reopening HFS left the move journal pending");
            MacBrowser::Entry tree;
            const auto dest=Find(target,2,u"Tree");
            VerifyFile(target,dest,u"Document",phase>=9);
            VerifyFile(target,dest,u"Preserved",false);
            VerifyFile(source,2,u"Source neighbor",false);
            VerifyFile(target,2,u"Target neighbor",false);
            if(phase>=9){
                Check(!source.FindChild(2,u"Tree",tree),"committed move kept the old tree");
                VerifyFile(target,Find(target,dest,u"New folder"),u"New entry",false);
            }else{
                Check(!target.FindChild(dest,u"New folder",tree),"rollback retained the new destination folder");
                const auto original=Find(source,2,u"Tree");
                VerifyFile(source,original,u"Document",true);
                VerifyFile(source,Find(source,original,u"New folder"),u"New entry",false);
                Check(Load(image)==before,"Windows move rollback changed the original HFS image");
            }
            WIN32_FIND_DATAW data{};
            const auto staged=FindFirstFileW((hostPath+L"\\.hfv-move-*").c_str(),&data);
            if(staged!=INVALID_HANDLE_VALUE)FindClose(staged);
            Check(staged==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_FILE_NOT_FOUND,
                  "Windows move recovery did not clear staging");
        }
    }
    fprintf(report,"PASS: moves in both directions between Windows and classic HFS recover during publication and on both sides of commit, preserving folders, both forks, Finder metadata and unrelated files. Cancellation restores both endpoints, and conflict recovery retains unrelated Windows files.\n");
}

void WindowsExportRecovery(const CString& root,const Bytes& blank,FILE* report)
{
    for(const unsigned phase:{7U,8U,9U}){
        CString image;
        image.Format("%s\\export-only-%u.dsk",root.GetString(),phase);
        Save(image,blank);
        const auto path=VolumeCoord::Wide(image.GetString()),hostPath=path+L".windows";
        Check(CreateDirectoryW(hostPath.c_str(),nullptr)!=FALSE,"create durable export fixture");
        {
            auto hfs=MacBrowser::OpenImage(path);
            auto host=MacBrowser::OpenWindows(hostPath);
            const auto incoming=hfs->MakeDirectory(2,u"Tree");
            const auto existing=host->MakeDirectory(2,u"Tree");
            Put(*hfs,true,false,incoming);
            Put(*host,false,false,existing);
            Put(*host,false,false,existing,u"Neighbor");
        }
        const auto before=Load(image);
        CrashChild(image,phase,L"export-only");
        Check(!ClassicHfsRecovery::Pending(path)&&Load(image)==before,
              "copy-only export changed the classic HFS source");
        auto host=MacBrowser::OpenWindows(hostPath);
        const auto folder=Find(*host,2,u"Tree");
        VerifyFile(*host,folder,u"Document",phase==9);
        VerifyFile(*host,folder,u"Neighbor",false);
    }
    fprintf(report,"PASS: interrupted copy-only NTFS exports recover both forks from the destination folder without reopening the source.\n");
}

unsigned StagedImages(const CString& root)
{
    WIN32_FIND_DATAA data{};
    HANDLE search=FindFirstFileA(root+"\\.hfv-image-*.tmp",&data);
    if(search==INVALID_HANDLE_VALUE){
        Check(GetLastError()==ERROR_FILE_NOT_FOUND,"enumerate temporary sibling images");
        return 0;
    }
    unsigned count=1;
    while(FindNextFileA(search,&data))++count;
    const auto last=GetLastError();
    FindClose(search);
    Check(last==ERROR_NO_MORE_FILES,"finish enumerating temporary sibling images");
    return count;
}

void FormatRecovery(const CString& root,FILE* report)
{
    const CString folder=root+"\\format";
    Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create formatting crash-test directory");
    const auto verify=[](const CString& image){
        const auto path=VolumeCoord::Wide(image.GetString());
        Check(Load(image).size()==FormatBytes,"published image has the wrong length");
        auto volume=MacBrowser::OpenImage(path);
        Check(volume&&volume->Name()==u"Verified"&&volume->List(2).empty(),
              "published image cannot be reopened as the expected empty HFS volume");
        volume->Close();
    };
    for(unsigned phase:{10u,11u,12u}){
        CString image;
        image.Format("%s\\crash-%u.dsk",folder.GetString(),phase);
        const auto previous=StagedImages(folder);
        CrashChild(image,phase,L"format");
        if(phase==12){
            verify(image);
            Check(StagedImages(folder)==previous,"publication left a temporary sibling");
        }else{
            Check(GetFileAttributesA(image)==INVALID_FILE_ATTRIBUTES,
                  "interrupted format exposed the destination");
            Check(StagedImages(folder)==previous+1,"interrupted format did not use a temporary sibling");
            FormatClassicImage(VolumeCoord::Wide(image.GetString()),FormatLabel,FormatBytes);
            verify(image);
            Check(StagedImages(folder)==previous+1,"retry changed an earlier staged file");
        }
    }

    const auto previous=StagedImages(folder);
    const CString image=folder+"\\failed.dsk";
    const auto path=VolumeCoord::Wide(image.GetString());
    Storage::Job job;
    const Bytes sentinel{1,2,3,4};
    for(unsigned failure=0;failure<4;++failure){
        struct Reset {
            ~Reset(){classicFormatTestStage={};HfsPlus::workerIoFault={};}
        } reset;
        if(failure==0){
            HfsPlus::workerIoFault=[](bool write,uint64_t offset,size_t){
                if(write&&offset==0)throw HfsPlus::Error("Injected format boot write failure");
            };
        }
        classicFormatTestStage=[&](ClassicFormatStage stage,HfsPlus::Source& output){
            if(failure==1&&stage==ClassicFormatStage::Verifying){
                const uint8_t invalid=0;
                output.Write(output.Size()-1024,&invalid,1);
            }
            if(failure==2&&stage==ClassicFormatStage::Publishing)Save(image,sentinel);
            if(failure==3&&stage==ClassicFormatStage::Publishing)job.cancel();
        };
        bool failed=false;
        try{FormatClassicImage(path,FormatLabel,FormatBytes,&job);}
        catch(const HfsPlus::Error&){failed=failure!=3;}
        catch(const Storage::Cancelled&){failed=failure==3;}
        Check(failed,"failed, conflicting or cancelled format was reported as successful");
        Check(StagedImages(folder)==previous,"ordinary format failure left a temporary image");
        if(failure==2){
            Check(Load(image)==sentinel,"publication replaced a concurrently created destination");
            Check(DeleteFileA(image)!=FALSE,"remove competing test output");
        }else{
            Check(GetFileAttributesA(image)==INVALID_FILE_ATTRIBUTES,
                  "failed verification or cancellation exposed a destination");
        }
    }
    fprintf(report,"PASS: interrupted classic HFS formatting publishes only after verification; retries, write errors, corrupt headers, name collisions and cancellation preserve the destination.\n");
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

Storage::Result TransferTree(const std::shared_ptr<MacBrowser::Volume>& source,
                             const std::shared_ptr<MacBrowser::Volume>& target,bool move,Storage::Job& job)
{
    auto origin=FileOperations::Location::Mounted(source,Find(*source,2,u"Tree"));
    origin.cut=move;
    FileOperations::Completion completion;
    return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{
        {origin},FileOperations::Location::Mounted(target,2)},job,completion);
}

Storage::Result TransferSelection(const std::shared_ptr<MacBrowser::Volume>& source,
                                  const std::shared_ptr<MacBrowser::Volume>& target,Storage::Job& job)
{
    std::vector<FileOperations::Location> selected;
    for(const auto* name:{u"Top entry",u"Tree"})
        selected.push_back(FileOperations::Location::Mounted(source,Find(*source,2,name)));
    FileOperations::Completion completion;
    if(target)return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{
        std::move(selected),FileOperations::Location::Mounted(target,2)},job,completion);
    return FileOperations::ExecuteInWorker(FileOperations::DeleteEntries{std::move(selected)},job,completion);
}

Storage::Result DeleteArchiveSelection(const std::shared_ptr<MacBrowser::Volume>& volume,uint32_t id,Storage::Job& job)
{
    FileOperations::Completion completion;
    return FileOperations::ExecuteInWorker(FileOperations::DeleteEntries{
        {FileOperations::Location::Mounted(volume,id)}},job,completion);
}

std::shared_ptr<MacBrowser::Volume> OpenCrossFormatFixture(const std::wstring& path)
{
    auto volume=MacBrowser::OpenImage(path);
    if(volume&&strcmp(volume->Kind(),"APFS")==0){
        volume.reset();
        const auto key=VolumeCoord::ImageKey(path);
        auto disk=HfsPlus::OpenImage(path.c_str(),true);
        volume=Apfs::OpenWritable(disk,key);
    }
    Check(volume&&volume->Writable(),"open a writable cross-format fixture");
    return volume;
}

Storage::Result CrossFormatMove(const CString& sourcePath,Storage::Job& job,bool merge=false)
{
    auto source=OpenCrossFormatFixture(VolumeCoord::Wide(sourcePath.GetString()));
    auto target=OpenCrossFormatFixture(VolumeCoord::Wide((sourcePath+".target.dsk").GetString()));
    Check(source&&target,"open both cross-format move participants");
    std::vector<FileOperations::Location> entries;
    for(const auto* name:merge?std::vector<const char16_t*>{u"Merge"}:std::vector<const char16_t*>{u"First",u"Second"}){
        auto location=FileOperations::Location::Mounted(source,Find(*source,2,name));
        location.cut=true;
        entries.push_back(std::move(location));
    }
    FileOperations::Completion completion;
    std::atomic<bool> finished{false};
    std::thread approve([&]{
        while(!finished){
            const auto prompt=job.conflictState();
            if(prompt.kind==Storage::CopyPrompt::ClassicArchiveMove&&!prompt.path.empty())
                job.resolve(prompt.id,Storage::ConflictChoice::ReplaceAll);
            if(prompt.kind==Storage::CopyPrompt::ClassicArchiveRestoreConflicts&&!prompt.path.empty())
                job.resolve(prompt.id,Storage::ConflictChoice::Replace);
            if(prompt.kind==Storage::CopyPrompt::ArchiveFolderMerge&&!prompt.path.empty())
                job.resolve(prompt.id,Storage::ConflictChoice::Skip);
            Sleep(1);
        }
    });
    const auto result=FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{
        std::move(entries),FileOperations::Location::Mounted(target,2)},job,completion);
    finished=true;
    approve.join();
    return result;
}

std::string ReadInterruptedMoveReport(const CString& path)
{
    WIN32_FIND_DATAA data{};
    const auto search=FindFirstFileA(path+"\\Move *.txt",&data);
    Check(search!=INVALID_HANDLE_VALUE,"interrupted move did not retain a report");
    const CString name=data.cFileName;
    const auto extra=FindNextFileA(search,&data);
    FindClose(search);
    Check(!extra,"unexpected extra interrupted move report");
    const auto bytes=Load(path+"\\"+name);
    return std::string(bytes.begin(),bytes.end());
}

Storage::Result WindowsReportedMove(const CString& root,Storage::Job& job)
{
    std::vector<FileOperations::Location> sources;
    for(const auto* name:{"First","Second"}){
        auto source=FileOperations::Location::Windows(VolumeCoord::Wide((root+"\\Source\\"+name).GetString()));
        source.cut=true;
        sources.push_back(std::move(source));
    }
    auto target=FileOperations::Location::Windows(VolumeCoord::Wide((root+"\\Destination").GetString()));
    target.directory=true;
    FileOperations::Completion completion;
    return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{
        std::move(sources),std::move(target)},job,completion);
}

void WindowsMoveReports(const CString& root,FILE* report)
{
    for(unsigned phase:{17u,18u,0u}){
        CString folder;
        folder.Format("%s\\reported-windows-move-%u",root.GetString(),phase);
        const auto from=folder+"\\Source",to=folder+"\\Destination",reports=folder+"\\Reports";
        Check(CreateDirectoryA(folder,nullptr)&&CreateDirectoryA(from,nullptr)&&
              CreateDirectoryA(to,nullptr),"create Windows interrupted move fixture");
        {
            auto source=MacBrowser::OpenWindows(VolumeCoord::Wide(from.GetString()));
            Put(*source,false,false,2,u"First");
            Put(*source,true,false,2,u"Second");
        }
        Check(SetEnvironmentVariableW(L"HFV_TEST_MOVE_REPORT_DIR",VolumeCoord::Wide(reports.GetString()).c_str())!=FALSE,
              "set isolated Windows move report directory");
        if(phase)CrashChild(folder,phase,L"reported-windows-move");
        else{
            Storage::Job job;
            const auto result=WindowsReportedMove(folder,job);
            Check(!result.code&&result.moved,"complete Windows move with reporting");
        }
        SetEnvironmentVariableW(L"HFV_TEST_MOVE_REPORT_DIR",nullptr);
        auto source=MacBrowser::OpenWindows(VolumeCoord::Wide(from.GetString()));
        auto target=MacBrowser::OpenWindows(VolumeCoord::Wide(to.GetString()));
        VerifyFile(*target,2,u"First",false);
        VerifyFile(*target,2,u"Second",true);
        MacBrowser::Entry found;
        Check(source->FindChild(2,u"First",found)==(phase==17)&&
              source->FindChild(2,u"Second",found)==(phase!=0),"Windows move removed the wrong sources");
        if(phase){
            const auto text=ReadInterruptedMoveReport(reports);
            Check(text.find("\r\nDestination published.\r\n")!=text.npos&&
                  text.find("Destination name for item 1: First")!=text.npos&&
                  text.find("Destination name for item 2: Second")!=text.npos&&
                  (text.find("Source removed for item 1.")!=text.npos)==(phase==18)&&
                  text.find("Source removed for item 2.")==text.npos,
                  "Windows move report misrepresented destination or source cleanup");
        }else{
            WIN32_FIND_DATAA find{};
            const auto search=FindFirstFileA(reports+"\\Move *.txt",&find);
            if(search!=INVALID_HANDLE_VALUE)FindClose(search);
            Check(search==INVALID_HANDLE_VALUE,"completed Windows move left its report behind");
        }
    }
    fprintf(report,"PASS: ordinary Windows Cut reports destination publication and completed source cleanup across crashes.\n");
}

void CrossFormatMoveRecovery(const CString& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    for(unsigned modern:{0u,1u,2u})for(bool fromClassic:{false,true}){
        for(unsigned phase:{17u,18u,0u}){
            CString from;
            from.Format("%s\\cross-format-%u-%u-%u.dsk",root.GetString(),modern,unsigned(fromClassic),phase);
            CString to=from+".target.dsk";
            FormatClassicImage(VolumeCoord::Wide((fromClassic?from:to).GetString()),"Classic move",4*1024*1024);
            auto disk=HfsPlus::CreateImage(VolumeCoord::Wide((fromClassic?to:from).GetString()).c_str(),16*1024*1024);
            if(modern==2){
                Storage::Job format;
                Apfs::Format(*disk,u"Modern move",true,format);
            }else HfsPlus::Volume::Format(*disk,u"Modern move",true,modern?Kind::HfsXCaseSensitive:Kind::HfsPlus);
            disk.reset();
            const auto fromPath=VolumeCoord::Wide(from.GetString()),toPath=VolumeCoord::Wide(to.GetString());
            {
                auto source=OpenCrossFormatFixture(fromPath);
                Put(*source,false,false,2,u"First");
                Put(*source,true,false,2,u"Second");
                if(fromClassic){
                    Storage::Job archive;
                    const auto first=source->Stat(Find(*source,2,u"First"));
                    ClassicCopyArchive::Save(*source,first.id,ClassicCopyArchive::Metadata(first),archive);
                }
                auto target=OpenCrossFormatFixture(toPath);
                Put(*target,true,false,2,u"Unrelated");
            }
            const CString reports=from+".reports";
            Check(SetEnvironmentVariableW(L"HFV_TEST_MOVE_REPORT_DIR",VolumeCoord::Wide(reports.GetString()).c_str())!=FALSE,
                  "set isolated interrupted move report directory");
            if(phase)CrashChild(from,phase,L"cross-format-move");
            else{
                Storage::Job cancel;
                cancel.transport([&](const Storage::Progress& progress){
                    if(progress.phase=="Removing moved sources"&&progress.path=="Second")cancel.cancel();
                },[](const std::string&){return Storage::ConflictChoice::ReplaceAll;});
                const auto result=CrossFormatMove(from,cancel);
                Check(result.error==ERROR_CANCELLED&&result.partial&&!result.moved&&result.completed==2&&
                      result.message.find("Source entries removed: 1")!=std::string::npos,
                      "cross-format cancellation did not report published copies and partial cleanup");
            }
            auto source=OpenCrossFormatFixture(fromPath);
            auto target=OpenCrossFormatFixture(toPath);
            VerifyForks(*target,target->Stat(Find(*target,2,u"First")),false);
            VerifyForks(*target,target->Stat(Find(*target,2,u"Second")),true);
            VerifyForks(*target,target->Stat(Find(*target,2,u"Unrelated")),true);
            VerifyForks(*source,source->Stat(Find(*source,2,u"Second")),true);
            MacBrowser::Entry first;
            Check(source->FindChild(2,u"First",first)==(phase==17),"wrong source cleanup after interruption");
            if(phase==17)VerifyForks(*source,source->Stat(first.id),false);
            const auto receipt=ReadInterruptedMoveReport(reports);
            Check(receipt.find("\r\nDestination published.\r\n")!=std::string::npos&&
                  receipt.find("Destination name for item 1: First")!=std::string::npos&&
                  receipt.find("Destination name for item 2: Second")!=std::string::npos&&
                  (receipt.find("Source removed for item 1.")!=std::string::npos)==(phase!=17)&&
                  receipt.find("Source removed for item 2.")==std::string::npos,
                  "interrupted move report did not distinguish verified copies from completed source removal");
        }
    }
    WindowsMoveReports(root,report);
    fprintf(report,"PASS: classic HFS moves with HFS+, HFSX and APFS retain verified destination forks and unprocessed sources when interrupted before and during source cleanup in either direction.\n");
}

void ArchiveMergeInterruption(const CString& root,FILE* report)
{
    for(unsigned formatKind:{0u,1u,2u})for(bool fromClassic:{false,true})for(bool crash:{false,true}){
        CString from;
        from.Format("%s\\archive-merge-%u-%u-%u.dsk",root.GetString(),formatKind,unsigned(fromClassic),unsigned(crash));
        const CString to=from+".target.dsk";
        const auto fromPath=VolumeCoord::Wide(from.GetString()),toPath=VolumeCoord::Wide(to.GetString());
        FormatClassicImage((fromClassic?fromPath:toPath),"Merge classic",4*1024*1024);
        const auto modern=fromClassic?toPath:fromPath;
        auto disk=HfsPlus::CreateImage(modern.c_str(),16*1024*1024);
        Storage::Job format;
        using Kind=HfsPlus::Volume::FormatKind;
        if(formatKind==2)Apfs::Format(*disk,u"Merge modern",true,format);
        else HfsPlus::Volume::Format(*disk,u"Merge modern",true,formatKind?Kind::HfsXCaseSensitive:Kind::HfsPlus);
        disk.reset();
        {
            auto source=OpenCrossFormatFixture(fromPath);
            auto target=OpenCrossFormatFixture(toPath);
            const auto fromFolder=source->MakeDirectory(2,u"Merge");
            const auto toFolder=target->MakeDirectory(2,u"Merge");
            Put(*source,true,false,fromFolder,u"First");
            Put(*source,true,false,fromFolder,u"Second");
            Put(*target,false,false,toFolder,u"First");
            Put(*target,false,false,toFolder,u"Unrelated");
            if(fromClassic){
                auto folder=source->Stat(fromFolder);
                ClassicCopyArchive::Save(*source,fromFolder,ClassicCopyArchive::Metadata(folder),format);
            }
        }
        const CString reports=from+".reports";
        Check(SetEnvironmentVariableW(L"HFV_TEST_MOVE_REPORT_DIR",VolumeCoord::Wide(reports.GetString()).c_str())!=FALSE,
              "set isolated archive merge report directory");
        if(crash)CrashChild(from,19,L"archive-merge-move");
        else{
            Storage::Job cancel;
            cancel.transport([&](const Storage::Progress& progress){
                if(progress.phase=="Copying"&&progress.files==1)cancel.cancel();
            },[](const std::string&){return Storage::ConflictChoice::Replace;});
            const auto result=CrossFormatMove(from,cancel,true);
            Check(result.error==ERROR_CANCELLED&&!result.moved&&result.partial==fromClassic&&
                  result.completed==unsigned(fromClassic),
                  "late archive merge cancellation did not report retained destination changes accurately");
        }
        auto source=OpenCrossFormatFixture(fromPath);
        auto target=OpenCrossFormatFixture(toPath);
        const auto fromFolder=Find(*source,2,u"Merge"),toFolder=Find(*target,2,u"Merge");
        VerifyForks(*source,source->Stat(Find(*source,fromFolder,u"First")),true);
        VerifyForks(*source,source->Stat(Find(*source,fromFolder,u"Second")),true);
        if(fromClassic)
            Check(ClassicCopyArchive::RequireVerifiedPair(*source,source->Stat(fromFolder),format).has_value(),
                  "interruption broke the source archive companion");
        VerifyForks(*target,target->Stat(Find(*target,toFolder,u"First")),fromClassic);
        VerifyForks(*target,target->Stat(Find(*target,toFolder,u"Unrelated")),false);
        MacBrowser::Entry absent;
        Check(!target->FindChild(toFolder,u"Second",absent),"interrupted merge copied the second entry");
        const auto receipt=ReadInterruptedMoveReport(reports);
        Check(receipt.find("Item 1 to move: Merge")!=std::string::npos&&
              receipt.find("\r\nDestination published.\r\n")==std::string::npos&&
              receipt.find("Source removal started for item")==std::string::npos,
              "interrupted merge report incorrectly claimed complete publication or source removal");
    }
    SetEnvironmentVariableW(L"HFV_TEST_MOVE_REPORT_DIR",nullptr);
    fprintf(report,"PASS: interrupted move reports survive worker exit and distinguish unverified merges, verified destinations and completed source removals.\n");
    fprintf(report,"PASS: late archive merge cancellation and process loss preserve both source files, both forks and unrelated destination files. Classic HFS rolls back the replacement; APFS, HFS+ and HFSX retain the completed replacement.\n");
}

void ArchiveDeleteRecovery(const CString& root,const Bytes& original,FILE* report)
{
    const CString image=root+"\\archive-delete.dsk";
    Save(image,original);
    const auto path=VolumeCoord::Wide(image.GetString());
    uint32_t companionId=0;
    std::u16string companionName;
    {
        auto volume=MacBrowser::OpenImage(path);
        auto item=volume->Stat(Find(*volume,2,u"Document"));
        Put(*volume,true,false,2,u"Unrelated");
        Storage::Job job;
        ClassicCopyArchive::Save(*volume,item.id,ClassicCopyArchive::Metadata(item),job);
        const auto pair=ClassicCopyArchive::RequireVerifiedPair(*volume,item,job);
        Check(pair.has_value(),"archive delete fixture has no companion");
        companionId=pair->companion.id;
        companionName=pair->companion.name;
        volume->Close();
    }
    const auto before=Load(image);
    {
        auto volume=MacBrowser::OpenImage(path);
        Storage::Job isolated;
        Check(DeleteArchiveSelection(volume,companionId,isolated).code!=0,
              "ordinary Delete removed a companion by itself");
        Storage::Job cancellation;
        cancellation.transport([&](const Storage::Progress& progress){
            if(!progress.scanning&&progress.files&&progress.path=="Document")cancellation.cancel();
        },{});
        const auto id=Find(*volume,2,u"Document");
        const auto cancelled=DeleteArchiveSelection(volume,id,cancellation);
        if(cancelled.error!=ERROR_CANCELLED||cancelled.rollback!=Storage::Rollback::Restored||cancelled.partial)
            throw HfsPlus::Error("Archive cancellation returned "+std::to_string(cancelled.error)+": "+cancelled.message);
        volume->Close();
    }
    Check(Load(image)==before&&!ClassicHfsRecovery::Pending(path),
          "cancelled archive deletion did not restore both files");
    for(const auto phase:{6u,2u,3u}){
        Save(image,before);
        CrashChild(image,phase,L"archive-delete");
        Check(ClassicHfsRecovery::Pending(path),"archive deletion did not leave recoverable state");
        auto volume=MacBrowser::OpenImage(path);
        Check(volume&&volume->Writable(),"archive deletion did not recover writable");
        VerifyFile(*volume,2,u"Unrelated",true);
        MacBrowser::Entry item;
        if(phase==3){
            Check(!volume->FindChild(2,u"Document",item)&&!volume->FindChild(2,companionName,item),
                  "committed archive deletion retained part of the pair");
        }else{
            VerifyFile(*volume,2,u"Document",false);
            Storage::Job check;
            Check(ClassicCopyArchive::RequireVerifiedPair(*volume,volume->Stat(Find(*volume,2,u"Document")),check).has_value(),
                  "interrupted archive deletion did not restore the verified pair");
        }
        volume->Close();
        Check(!ClassicHfsRecovery::Pending(path),"archive deletion recovery left a pending journal");
        if(phase!=3)Check(Load(image)==before,"archive deletion rollback changed the original image");
    }
    fprintf(report,"PASS: classic archive Delete rejects direct companion deletion and recovers both files on cancellation or process loss.\n");
}

Storage::Result MoveSelection(const std::shared_ptr<MacBrowser::Volume>& volume,Storage::Job& job)
{
    std::vector<FileOperations::Location> selected;
    for(const auto* name:{u"Top entry",u"Tree"}){
        auto item=FileOperations::Location::Mounted(volume,Find(*volume,2,name));
        item.cut=true;
        selected.push_back(std::move(item));
    }
    FileOperations::Completion completion;
    return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{
        std::move(selected),FileOperations::Location::Mounted(volume,Find(*volume,2,u"Drop here"))},job,completion);
}

Storage::Result MoveAcrossImages(const std::wstring& destination,Storage::Job& job)
{
    auto left=MacBrowser::OpenImage(destination+L".left");
    auto right=MacBrowser::OpenImage(destination+L".right");
    auto target=MacBrowser::OpenImage(destination);
    auto first=FileOperations::Location::Mounted(left,Find(*left,2,u"Left"));
    auto second=FileOperations::Location::Mounted(right,Find(*right,2,u"Right"));
    first.cut=second.cut=true;
    FileOperations::Completion completion;
    return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{
        {first,second},FileOperations::Location::Mounted(target,2)},job,completion);
}
void MixedImageMoveRecovery(const CString& root,const Bytes& blank,FILE* report)
{
    for(const unsigned phase:{0u,8u,9u}){
        CString image;
        image.Format("%s\\mixed-image-move-%u.dsk",root.GetString(),phase);
        const auto path=VolumeCoord::Wide(image.GetString());
        const CString left=image+".left",right=image+".right";
        for(const auto& file:{image,left,right})Save(file,blank);
        Put(*MacBrowser::OpenImage(path),false,false,2,u"Left");
        Put(*MacBrowser::OpenImage(path+L".left"),true,false,2,u"Left");
        Put(*MacBrowser::OpenImage(path+L".right"),true,false,2,u"Right");
        const auto original=Load(image),originalLeft=Load(left),originalRight=Load(right);
        if(phase){
            CrashChild(image,phase,L"mixed-image-move");
            Check(ClassicHfsRecovery::Pending(path)&&ClassicHfsRecovery::Pending(path+L".left")&&
                  ClassicHfsRecovery::Pending(path+L".right"),"mixed image move lost a participant journal");
        }else{
            Storage::Job job;
            job.transport([&](const Storage::Progress& p){if(p.files==1)job.cancel();},
                          [](const std::string&){return Storage::ConflictChoice::Replace;});
            const auto result=MoveAcrossImages(path,job);
            Check(result.error==ERROR_CANCELLED&&result.rollback==Storage::Rollback::Restored&&!result.partial,
                  "mixed image cancellation reported retained edits");
        }
        {
            // Open the sources in reverse order to verify independent recovery.
            auto second=MacBrowser::OpenImage(path+L".right");
            auto first=MacBrowser::OpenImage(path+L".left");
            auto dest=MacBrowser::OpenImage(path);
            VerifyFile(*dest,2,u"Left",phase==9);
            MacBrowser::Entry item;
            if(phase==9){
                Check(!first->FindChild(2,u"Left",item)&&!second->FindChild(2,u"Right",item),
                      "committed mixed move kept a source");
                VerifyFile(*dest,2,u"Right",true);
            }else{
                Check(!dest->FindChild(2,u"Right",item),"aborted mixed move kept a destination file");
                VerifyFile(*first,2,u"Left",true);
                VerifyFile(*second,2,u"Right",true);
            }
        }
        if(phase!=9)Check(Load(image)==original&&Load(left)==originalLeft&&Load(right)==originalRight,
                         "mixed image rollback did not restore exact bytes");
    }
    fprintf(report,"PASS: a mixed selection across two classic HFS images commits together at one destination, with fork-safe cancellation and crash recovery.\n");
}

void SameVolumeBatchRecovery(const CString& folder,const Bytes& baseline,FILE* report)
{
    for(unsigned phase:{7u,3u}){
        CString image;
        image.Format("%s\\same-volume-batch-%u.dsk",folder.GetString(),phase);
        Save(image,baseline);
        CrashChild(image,phase,L"batch-move");
        const auto path=VolumeCoord::Wide(image.GetString());
        Check(ClassicHfsRecovery::Pending(path),"interrupted in-place move did not keep its journal");
        {
            auto volume=MacBrowser::OpenBrowserImage(path);
            Check(volume&&volume->Writable(),"batch move did not recover writable");
            const auto drop=Find(*volume,2,u"Drop here");
            const auto parent=phase==3?drop:2;
            const auto empty=phase==3?2:drop;
            VerifyFile(*volume,parent,u"Top entry",false);
            VerifyFile(*volume,Find(*volume,parent,u"Tree"),u"A File",true);
            MacBrowser::Entry entry{};
            Check(!volume->FindChild(empty,u"Tree",entry)&&!volume->FindChild(empty,u"Top entry",entry),
                  "same-volume recovery split the selection");
        }
        if(phase==7)Check(Load(image)==baseline,"interrupted in-place move did not restore exact image bytes");
    }
    const CString image=folder+"\\cancel-batch-move.dsk";
    Save(image,baseline);
    {
        auto volume=MacBrowser::OpenBrowserImage(VolumeCoord::Wide(image.GetString()));
        Storage::Job job;
        job.transport([&](const Storage::Progress& p){if(p.files==1)job.cancel();},{});
        const auto result=MoveSelection(volume,job);
        Check(result.error==ERROR_CANCELLED&&result.rollback==Storage::Rollback::Restored&&
              !result.partial&&result.completed==0,"cancelled in-place batch reported committed moves");
    }
    Check(Load(image)==baseline,"cancelled in-place batch changed image bytes");
    fprintf(report,"PASS: same-volume classic HFS multi-item moves recover together on interruption and cancellation.\n");
}

void VerifyTransferredTree(MacBrowser::Volume& volume,bool copied)
{
    const auto tree=Find(volume,2,u"Tree");
    VerifyFile(volume,tree,u"A File",copied);
    VerifyFile(volume,tree,u"Merge extra",false);
    MacBrowser::Entry folder{};
    const bool hasNested=volume.FindChild(tree,u"Z Folder",folder);
    Check(hasNested==copied,"destination contains an incomplete transferred subtree");
    if(copied)VerifyFile(volume,folder.id,u"Nested file",true);
    VerifyFile(volume,2,u"Unrelated",true);
}

void FolderTransferRecovery(const CString& root,const Bytes& blank,FILE* report)
{
    const CString sourceSeed=root+"\\transfer-source.dsk",targetSeed=root+"\\transfer-target.dsk";
    Save(sourceSeed,blank);
    Save(targetSeed,blank);
    const auto sourceSeedPath=VolumeCoord::Wide(sourceSeed.GetString());
    const auto targetSeedPath=VolumeCoord::Wide(targetSeed.GetString());
    {
        auto source=MacBrowser::OpenImage(sourceSeedPath);
        auto target=MacBrowser::OpenImage(targetSeedPath);
        Put(*source,false,false,2,u"Top entry");
        source->MakeDirectory(2,u"Drop here");
        const auto tree=source->MakeDirectory(2,u"Tree");
        Put(*source,true,false,tree,u"A File");
        Put(*source,true,false,source->MakeDirectory(tree,u"Z Folder"),u"Nested file");
        const auto targetTree=target->MakeDirectory(2,u"Tree");
        Put(*target,false,false,targetTree,u"A File");
        Put(*target,false,false,targetTree,u"Merge extra");
        Put(*target,true,false,2,u"Unrelated");
        source->Close();
        target->Close();
    }
    const auto sourceBefore=Load(sourceSeed);
    const auto targetBefore=Load(targetSeed);
    SameVolumeBatchRecovery(root,sourceBefore,report);
    for(bool deleting:{false,true}){
        const CString sourceFile=root+(deleting?"\\batch-delete.dsk":"\\batch-copy.dsk");
        const CString targetFile=sourceFile+".target.dsk";
        Save(sourceFile,sourceBefore);
        Save(targetFile,targetBefore);
        CrashChild(sourceFile,7,deleting?L"batch-delete":L"batch-copy");
        const auto changed=VolumeCoord::Wide((deleting?sourceFile:targetFile).GetString());
        Check(ClassicHfsRecovery::Pending(changed),"interrupted selection left no batch journal");
        {
            auto recovered=MacBrowser::OpenBrowserImage(changed);
            Check(recovered&&recovered->Writable(),"batch recovery did not reopen the image writable");
        }
        Check(Load(sourceFile)==sourceBefore&&Load(targetFile)==targetBefore,
              "batch rollback retained a change from an earlier selected item");
        auto source=MacBrowser::OpenBrowserImage(VolumeCoord::Wide(sourceFile.GetString()));
        auto target=deleting?nullptr:MacBrowser::OpenBrowserImage(VolumeCoord::Wide(targetFile.GetString()));
        Storage::Job job;
        job.transport([&](const Storage::Progress& p){
            if(p.files==1)job.cancel();
        },[](const std::string&){return Storage::ConflictChoice::Replace;});
        const auto result=TransferSelection(source,target,job);
        Check(result.error==ERROR_CANCELLED&&result.rollback==Storage::Rollback::Restored&&
              !result.partial&&result.completed==0,"cancelled batch reported committed items");
        source.reset();
        target.reset();
        Check(Load(sourceFile)==sourceBefore&&Load(targetFile)==targetBefore,
              "cancelled batch left an earlier selected item changed");
    }
    fprintf(report,"PASS: multi-item classic HFS copy and delete restore earlier selected items after cancellation or process loss.\n");
    for(unsigned phase:{7u,6u,8u,9u,3u}){
        CString sourceFile;
        sourceFile.Format("%s\\transfer-%u.dsk",root.GetString(),phase);
        const CString targetFile=sourceFile+".target.dsk";
        Save(sourceFile,sourceBefore);
        Save(targetFile,targetBefore);
        const auto sourcePath=VolumeCoord::Wide(sourceFile.GetString());
        const auto targetPath=VolumeCoord::Wide(targetFile.GetString());
        CrashChild(sourceFile,phase,phase==7?L"folder-copy":L"folder-move");
        Check(ClassicHfsRecovery::Pending(targetPath),"interrupted transfer did not keep the destination journal");
        Check(phase==7||ClassicHfsRecovery::Pending(sourcePath),"interrupted move did not keep the source journal");
        if(phase==9){
            const CString decisions=root+"\\HFVExplorer\\ClassicMoves";
            const CString hidden=root+"\\saved-decisions";
            const auto changedSource=Load(sourceFile),changedTarget=Load(targetFile);
            Check(MoveFileA(decisions,hidden)!=FALSE,"simulate unavailable classic HFS move decisions");
            bool fenced=true;
            for(const auto& path:{sourcePath,targetPath}){
                try{if(MacBrowser::OpenImage(path))fenced=false;}
                catch(const HfsPlus::Error&){}
            }
            Check(MoveFileA(hidden,decisions)!=FALSE,"restore classic HFS move decisions");
            Check(fenced&&Load(sourceFile)==changedSource&&Load(targetFile)==changedTarget&&
                  ClassicHfsRecovery::Pending(sourcePath)&&ClassicHfsRecovery::Pending(targetPath),
                  "unavailable shared decision did not fence both images");
        }
        const bool committed=phase==9||phase==3;
        {
            auto source=MacBrowser::OpenBrowserImage(sourcePath);
            auto target=MacBrowser::OpenBrowserImage(targetPath);
            Check(target&&target->Writable()&&source&&source->Writable(),"reopen both transfer images writable");
            VerifyTransferredTree(*target,committed);
            MacBrowser::Entry item{};
            if(committed)Check(!source->FindChild(2,u"Tree",item),"committed move retained a duplicate source tree");
            else{
                const auto tree=Find(*source,2,u"Tree");
                VerifyFile(*source,tree,u"A File",true);
                VerifyFile(*source,Find(*source,tree,u"Z Folder"),u"Nested file",true);
            }
            VerifyFile(*source,2,u"Top entry",false);
        }
        if(!committed)Check(Load(sourceFile)==sourceBefore&&Load(targetFile)==targetBefore,
                            "aborted coordinated move changed either image");
    }

    for(unsigned phase:{8u,9u}){
        CString sourceFile;
        sourceFile.Format("%s\\paired-transfer-%u.dsk",root.GetString(),phase);
        const CString targetFile=sourceFile+".target.dsk";
        const auto sourcePath=VolumeCoord::Wide(sourceFile.GetString());
        const auto targetPath=VolumeCoord::Wide(targetFile.GetString());
        Save(sourceFile,sourceBefore);
        Save(targetFile,blank);
        Storage::Job job;
        {
            auto source=MacBrowser::OpenImage(sourcePath);
            const auto tree=Find(*source,2,u"Tree");
            for(auto id:{Find(*source,tree,u"A File"),tree}){
                ClassicCopyArchive::Metadata metadata(source->Stat(id));
                metadata.entry.attributes[u"user.archive-verifier"]={8,9};
                ClassicCopyArchive::Save(*source,id,metadata,job);
            }
            source->Close();
        }
        const auto pairedBefore=Load(sourceFile);
        CrashChild(sourceFile,phase,L"folder-move");
        auto source=MacBrowser::OpenImage(sourcePath);
        auto target=MacBrowser::OpenImage(targetPath);
        const bool committed=phase==9;
        auto& kept=committed?*target:*source;
        auto& empty=committed?*source:*target;
        MacBrowser::Entry tree;
        Check(kept.FindChild(2,u"Tree",tree)&&!empty.FindChild(2,u"Tree",tree),
              "Crash recovery left the archive on the wrong volume");
        Check(kept.FindChild(2,u"Tree",tree),"Missing recovered archive folder");
        MacBrowser::Entry leftover;
        Check(!empty.FindChild(2,ClassicCopyArchive::CompanionName(tree.resolvedId),leftover),
              "Crash recovery left an unpaired companion");
        const auto child=Find(kept,tree.id,u"A File");
        for(const auto& item:{tree,kept.Stat(child)}){
            const auto pair=ClassicCopyArchive::RequireVerifiedPair(kept,item,job);
            Check(pair&&pair->metadata.entry.attributes.count(u"user.archive-verifier"),
                  "Crash recovery dropped an archive companion or its attributes");
        }
        VerifyFile(kept,tree.id,u"A File",true);
        source->Close();
        target->Close();
        if(!committed)
            Check(Load(sourceFile)==pairedBefore&&Load(targetFile)==blank,
                  "Uncommitted archive move did not restore both images byte for byte");
    }
    fprintf(report,"PASS: paired classic HFS Cut/Paste recovers both forks and companions before and after the shared commit.\n");

    for(bool crash:{false,true}){
        CString sourceFile;
        sourceFile.Format("%s\\transfer-worker-%u.dsk",root.GetString(),unsigned(crash));
        const CString targetFile=sourceFile+".target.dsk";
        Save(sourceFile,sourceBefore);
        Save(targetFile,targetBefore);
        const auto targetPath=VolumeCoord::Wide(targetFile.GetString());
        {
            auto source=MacBrowser::OpenBrowserImage(VolumeCoord::Wide(sourceFile.GetString()));
            auto target=MacBrowser::OpenBrowserImage(targetPath);
            const auto pid=VolumeSession::VolumeSessionPidForTest(target);
            bool interrupted=false;
            Storage::Job job;
            job.transport([&](const Storage::Progress& progress){
                if(progress.files!=1||interrupted)return;
                interrupted=true;
                if(crash)VolumeSession::CrashVolumeSessionDuringPutForTest(target);
                else job.cancel();
            },[](const std::string&){return Storage::ConflictChoice::Replace;});
            const auto result=TransferTree(source,target,true,job);
            Check(interrupted&&result.code,"persistent folder move did not fail or cancel");
            if(crash){
                Check(ClassicHfsRecovery::Pending(targetPath),"worker interruption discarded folder undo");
                target->Reload();
                Check(VolumeSession::VolumeSessionPidForTest(target)!=pid,"persistent worker did not restart");
            }else{
                Check(result.error==ERROR_CANCELLED&&result.rollback==Storage::Rollback::Restored&&
                      !result.partial&&result.completed==0,"cancelled folder move reported retained writes");
                Check(VolumeSession::VolumeSessionPidForTest(target)==pid,"normal cancellation restarted the worker");
            }
            Check(target->Writable(),"recovered folder destination is read-only");
            VerifyTransferredTree(*target,false);
        }
        Check(!ClassicHfsRecovery::Pending(targetPath)&&Load(targetFile)==targetBefore&&Load(sourceFile)==sourceBefore,
              "persistent folder recovery failed to restore both image files");
    }
    {
        const CString sourceFile=root+"\\completed-source.dsk",targetFile=root+"\\completed-target.dsk";
        Save(sourceFile,sourceBefore);
        Save(targetFile,targetBefore);
        auto source=MacBrowser::OpenBrowserImage(VolumeCoord::Wide(sourceFile.GetString()));
        auto target=MacBrowser::OpenBrowserImage(VolumeCoord::Wide(targetFile.GetString()));
        Storage::Job job;
        job.transport({},[](const std::string&){return Storage::ConflictChoice::Replace;});
        const auto result=TransferTree(source,target,true,job);
        MacBrowser::Entry old{};
        Check(!result.code&&result.moved&&!source->FindChild(2,u"Tree",old),"completed move retained its source tree");
        VerifyTransferredTree(*target,true);
    }
    if(const wchar_t* fixtures=_wgetenv(L"HFV_NDIF_INTERCHANGE_FIXTURES")){
        const CString sourceFile=root+"\\transfer-new-ndif.dsk",targetFile=sourceFile+".target.dsk";
        const auto sourcePath=VolumeCoord::Wide(sourceFile.GetString());
        const auto targetPath=VolumeCoord::Wide(targetFile.GetString());
        const auto fixture=std::wstring(fixtures)+L"\\partitioned-classic.bin";
        Save(sourceFile,sourceBefore);
        Check(CopyFileW(fixture.c_str(),targetPath.c_str(),TRUE)!=FALSE,"copy shared folder-transfer NDIF target");
        const auto before=Load(targetFile);
        CrashChild(sourceFile,7,L"folder-copy");
        Check(ClassicHfsRecovery::Pending(targetPath,true),"interrupted NDIF folder creation has no undo journal");
        {
            auto target=MacBrowser::OpenBrowserImage(targetPath);
            MacBrowser::Entry tree{};
            Check(target&&target->Writable()&&!target->FindChild(2,u"Tree",tree),
                  "interrupted NDIF copy retained a newly created folder");
        }
        Check(Load(targetFile)==before&&Load(sourceFile)==sourceBefore,
              "NDIF folder undo changed the partition wrapper or source");
        fprintf(report,"PASS: an interrupted copy of a new folder into shared partitioned NDIF restores the exact container bytes.\n");
        for(unsigned phase:{8u,9u}){
            CString sourceFile;
            sourceFile.Format("%s\\ndif-shared-move-%u.dsk",root.GetString(),phase);
            const CString targetFile=sourceFile+".target.dsk";
            const auto from=VolumeCoord::Wide(sourceFile.GetString());
            const auto to=VolumeCoord::Wide(targetFile.GetString());
            Save(sourceFile,sourceBefore);
            Check(CopyFileW(fixture.c_str(),to.c_str(),TRUE)!=FALSE,"copy shared NDIF move target");
            const auto original=Load(targetFile);
            CrashChild(sourceFile,phase,L"folder-move");
            Check(ClassicHfsRecovery::Pending(from)&&ClassicHfsRecovery::Pending(to,true),
                  "interrupted raw-to-NDIF move did not keep both journals");
            {
                auto target=MacBrowser::OpenBrowserImage(to);
                auto source=MacBrowser::OpenBrowserImage(from);
                MacBrowser::Entry entry{};
                Check(target&&target->Writable()&&source&&source->Writable()&&
                      target->FindChild(2,u"Tree",entry)==(phase==9)&&
                      source->FindChild(2,u"Tree",entry)==(phase==8),"raw and NDIF chose different move outcomes");
                if(phase==9)VerifyFile(*target,Find(*target,2,u"Tree"),u"A File",true);
            }
            if(phase==8)Check(Load(sourceFile)==sourceBefore&&Load(targetFile)==original,
                              "aborted raw-to-NDIF move changed image bytes");
        }
        fprintf(report,"PASS: raw and partitioned NDIF use the same durable decision during a cross-volume move.\n");
    }
    fprintf(report,"PASS: whole-folder merge recovery restores both forks and exact destination metadata; worker crash and cancellation roll back the tree; both images recover from the shared move decision, including a crash during source deletion and after commit.\n");
}

void VerifyOutside(const Bytes& before,const Bytes& after,uint64_t start,uint64_t length)
{
    Check(before.size()==after.size()&&start<=before.size()&&length<=before.size()-start&&
          std::equal(before.begin(),before.begin()+size_t(start),after.begin())&&
          std::equal(before.begin()+size_t(start+length),before.end(),after.begin()+size_t(start+length)),
          "NDIF recovery or commit changed bytes outside the selected HFS partition");
}
void SiblingRawRecovery(const CString& root,FILE* report)
{
    const CString seed=root+"\\sibling-seed.dsk";
    const auto seedPath=VolumeCoord::Wide(seed.GetString());
    FormatClassicImage(seedPath,"SiblingUndo",4*1024*1024);
    {
        auto volume=MacBrowser::OpenImage(seedPath);
        Check(volume&&volume->Writable(),"open raw sibling-recovery seed");
        Put(*volume,false);
        Put(*volume,true,false,2,u"Unrelated");
        volume->Close();
    }
    const auto original=Load(seed);
    for(const auto* action:{L"overwrite",L"delete"}){
        for(unsigned phase:{1u,2u,3u}){
            CString image;
            image.Format("%s\\sibling-%ls-%u.dsk",root.GetString(),action,phase);
            Save(image,original);
            const auto path=VolumeCoord::Wide(image.GetString());
            const CString journal=image+".hfv-classic-undo";
            CrashChild(image,phase,action);
            Check(ClassicHfsRecovery::Pending(path)&&!Load(journal).empty()&&Load(image)!=original,
                  "interrupted raw edit did not write a sibling undo file");
            if(phase==1)CrashChild(image,5);
            {
                auto volume=MacBrowser::OpenBrowserImage(path);
                Check(volume&&volume->Writable(),"open raw sibling recovery in the browser worker");
                MacBrowser::Entry item{};
                if(phase==3&&wcscmp(action,L"delete")==0)
                    Check(!volume->FindChild(2,u"Document",item),"committed sibling delete returned");
                else VerifyFile(*volume,2,u"Document",phase==3);
                VerifyFile(*volume,2,u"Unrelated",true);
            }
            Check(!ClassicHfsRecovery::Pending(path)&&Load(journal).empty(),
                  "recovered raw image still has sibling undo records");
            if(phase!=3)Check(Load(image)==original,"sibling undo did not restore exact raw image bytes");
        }
    }

    const CString image=root+"\\replaced.dsk",replacement=root+"\\replacement.dsk";
    const auto path=VolumeCoord::Wide(image.GetString());
    Save(image,original);
    CrashChild(image,2);
    const CString journal=image+".hfv-classic-undo";
    const auto stale=Load(journal);
    FormatClassicImage(VolumeCoord::Wide(replacement.GetString()),"Replacement",original.size());
    const auto before=Load(replacement);
    Check(MoveFileExA(replacement,image,MOVEFILE_REPLACE_EXISTING)!=FALSE,"replace raw crash-test image");
    bool rejected=false;
    try{auto volume=MacBrowser::OpenImage(path);rejected=!volume;}
    catch(const HfsPlus::Error&){rejected=true;}
    Check(rejected&&Load(image)==before&&Load(journal)==stale,
          "stale sibling undo was applied to a replacement image or erased");
    const CString collision=root+"\\collision.dsk";
    const CString unrelated=collision+".hfv-classic-undo";
    const Bytes unrelatedBytes{0x12,0x34,0x56};
    Save(collision,original);
    Save(unrelated,unrelatedBytes);
    rejected=false;
    try{auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(collision.GetString()));rejected=!volume;}
    catch(const HfsPlus::Error&){rejected=true;}
    Check(rejected&&Load(collision)==original&&Load(unrelated)==unrelatedBytes,
          "an unrelated file at the sibling path was altered or accepted");
    fprintf(report,"PASS: sibling undo recovers raw overwrites and deletes before and after commit, retries interrupted replay, and rejects stale and unrelated sibling files.\n");
}

void NdifRecovery(const CString& root,FILE* report,bool sibling=false)
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
                if(sibling)
                    Check(!Load(image+".hfv-ndif-classic-undo").empty(),"NDIF undo did not use the selected sibling file");
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

Storage::Result DeleteWindowsFixture(const std::wstring& path,Storage::Job& job,
                                     const wchar_t* leaf=L"Document")
{
    FileOperations::DeleteEntries request{{FileOperations::Location::Windows(path+L"\\"+leaf)}};
    FileOperations::Completion completion;
    return FileOperations::ExecuteInWorker(FileOperations::Request(std::move(request)),job,completion);
}
static void RunLauncherInterruptionTests(const CString& root,FILE* report)
{
    readonly_cd_backend_init();
    for(unsigned phase:{8u,9u,10u,11u}){
        CString folder;
        folder.Format("%s\\launcher-%u",root.GetString(),phase);
        const auto staged=folder+"\\hfv123.tmp.dir";
        Check(CreateDirectoryA(folder,nullptr)&&CreateDirectoryA(staged,nullptr),"create launcher crash fixture");
        const auto image=folder+"\\Launch.dsk";
        const auto imagePath=VolumeCoord::Wide(image.GetString());
        FormatClassicImage(imagePath,"Launch",4*1024*1024);
        {
            auto volume=MacBrowser::OpenImage(imagePath);
            Put(*volume,false);
            volume->Close();
        }
        const auto editPath=staged+"\\Edit.dat";
        Save(editPath,Bytes{1,2,3});
        auto location=FileOperations::Location::Image(image.GetString(),"Launch:Document");
        location.expectedResource=location.ResourceKey();
        Storage::Job job;
        const auto version=FileOperations::CaptureLaunchVersion(location,job);
        {
            LauncherWriteBack edit(location,version,editPath);
            Check(edit.SavePending(),"persist launcher crash fixture");
        }
        CrashChild(folder,phase==11?1:0,L"launcher-fake-editor");
        const Bytes edited{9,8,7,6};
        const auto start=GetTickCount();
        while(GetFileAttributesA(staged+"\\editor-saved")==INVALID_FILE_ATTRIBUTES&&GetTickCount()-start<10000)
            Sleep(10);
        Check(Load(editPath)==edited,"delegated editor did not replace the temporary document");
        CrashChild(folder,phase,L"launcher-write-back");
        std::wstring previousMarker;
        if(phase==8){
            const auto record=Load(staged+"\\.hfv-pending-edit");
            Check(record.size()>ClassicHfsRecovery::MoveIdSize,"interrupted launcher write did not persist its decision");
            const std::string prior(record.end()-ClassicHfsRecovery::MoveIdSize,record.end());
            previousMarker=ClassicHfsRecovery::MoveDecisionPath(prior);
            Check(GetFileAttributesW(previousMarker.c_str())!=INVALID_FILE_ATTRIBUTES&&
                  ClassicHfsRecovery::Pending(imagePath),"interrupted launcher write lost its decision or undo");
        }
        {
            auto recovered=LauncherWriteBack::LoadPending(folder);
            bool committed=false;
            Check(phase==10?recovered.empty():(recovered.size()==1&&
                  !recovered.front()->CheckCommitStatus(committed).code&&committed==(phase==9)),
                  "reopened launcher record has the wrong commit status");
            if(phase==9||phase==10) {
                auto volume=MacBrowser::OpenImage(imagePath);
                const auto id=Find(*volume,2,u"Document");
                const auto entry=volume->Stat(id);
                Bytes data(edited.size()),resource(size_t(entry.forks[1].size));
                volume->Read(id,0,0,data.data(),data.size());
                volume->Read(id,1,0,resource.data(),resource.size());
                Check(entry.forks[0].size==edited.size()&&data==edited&&resource.size()==Entry(false).forks[1].size,
                      "committed launcher edit did not preserve both forks");
                for(size_t i=0;i<resource.size();++i)
                    Check(resource[i]==Byte(false,1,i),"launcher write-back changed the resource fork");
                volume->Close();
                if(phase==9)Check(recovered.front()->RetireCommitted(),"retire already committed edit");
            }else{
                auto volume=MacBrowser::OpenImage(imagePath);
                VerifyFile(*volume,2,u"Document",false);
                volume->Close();
                if(phase==8){
                    const auto edit=VolumeCoord::Wide(editPath.GetString());
                    LauncherTemporaryDocument document;
                    Storage::Job retry;
                    Check(!document.Open(edit.c_str(),true).code,"protect staged copy for retry");
                    const auto result=recovered.front()->ApplyPending(document,retry);
                    Check(!result.code,result.message.c_str());
                    Check(GetFileAttributesW(previousMarker.c_str())==INVALID_FILE_ATTRIBUTES&&
                          !ClassicHfsRecovery::Pending(imagePath),"retry orphaned the previous launcher marker or undo");
                    bool committed=false;
                    Check(!recovered.front()->CheckCommitStatus(committed).code&&committed,
                          "cleaning the previous marker lost the successful retry decision");
                }
            }
        }
        Check(Load(editPath)==edited,"launcher interruption removed or changed the edited copy");
        if(phase==11)Check(Load(editPath+":EditorState")==Bytes({4,2}),"launcher lost named stream");
    }
    fprintf(report,"PASS: delegated editor safe-save, named stream retention, and subprocess crashes before commit, after commit and during cleanup preserve launcher data.\n");
}

void RunDurableWindowsMutationTests(const char* directory,FILE* report)
{
    const CString root=directory;
    for(unsigned phase:{7u,8u,9u,12u}){
        CString folder;
        folder.Format("%s\\paired-rename-%u",root.GetString(),phase);
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create paired rename fixture");
        const auto path=VolumeCoord::Wide(folder.GetString());
        {
            auto host=MacBrowser::OpenWindows(path);
            Put(*host,false);
            Put(*host,false,false,2,u"Neighbor");
        }
        CrashChild(folder,phase,L"paired-windows-rename");
        auto host=MacBrowser::OpenWindows(path);
        const auto* name=phase==9?u"Renamed":u"Document";
        VerifyFile(*host,2,name,false);
        VerifyFile(*host,2,u"Neighbor",false);
        Check(host->List(2).size()==2,"paired Windows rename left a stray file or companion");
        host->Rename(Find(*host,2,name),2,phase==9?u"RENAMED":u"DOCUMENT");
        host=MacBrowser::OpenWindows(path);
        VerifyFile(*host,2,phase==9?u"RENAMED":u"DOCUMENT",false);
    }
    fprintf(report,"PASS: paired NTFS file rename recovers both forks after companion publication, before commit and after commit, including case-only renames.\n");
    {
        const CString folder=root+"\\unsupported-paired-rename";
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create unsupported paired rename fixture");
        const auto path=VolumeCoord::Wide(folder.GetString());
        auto host=MacBrowser::OpenWindows(path);
        Put(*host,false);
        const CString plain=folder+"\\Plain";
        Save(plain,Bytes{17});
        struct Unsupported{
            Unsupported(){WindowsMoveUndo::forceUnsupportedForTest=true;}
            ~Unsupported(){WindowsMoveUndo::forceUnsupportedForTest=false;}
        } unsupported;
        bool refused=false;
        try{host->Rename(Find(*host,2,u"Document"),2,u"Renamed");}
        catch(const HfsPlus::Error& error){refused=std::string(error.what()).find("requires a local NTFS folder")!=std::string::npos;}
        Check(refused&&GetFileAttributesW((path+L"\\Renamed").c_str())==INVALID_FILE_ATTRIBUTES&&
              GetFileAttributesW((path+L"\\%Renamed").c_str())==INVALID_FILE_ATTRIBUTES,
              "unsupported paired rename changed a name");
        VerifyFile(*host,2,u"Document",false);
        host->Rename(Find(*host,2,u"Plain"),2,u"Ordinary");
        Check(Load(folder+"\\Ordinary")==Bytes{17},"unsupported media rejected an ordinary rename");
    }
    fprintf(report,"PASS: an unsupported Windows backend refuses paired rename before either name changes and permits ordinary rename.\n");

    for(unsigned phase:{0u,7u,8u,9u}){
        CString folder;
        folder.Format("%s\\paired-delete-%u",root.GetString(),phase);
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create paired delete fixture");
        const auto path=VolumeCoord::Wide(folder.GetString());
        {
            auto host=MacBrowser::OpenWindows(path);
            Put(*host,false);
            Put(*host,false,false,2,u"Neighbor");
        }
        if(phase==0){
            Storage::Job job;
            MacBrowser::windowsPublicationFault=[&](const char* step){
                if(!strcmp(step,"delete-companion-staged"))job.cancel();
            };
            struct ClearFault{~ClearFault(){MacBrowser::windowsPublicationFault={};}} clear;
            const auto result=DeleteWindowsFixture(path,job);
            Check(result.error==ERROR_CANCELLED,"cancel after companion staging");
        }else CrashChild(folder,phase,L"paired-windows-delete");
        const auto canonical=BrowserPresentation::WindowsPath(path);
        const auto child=path+L"\\Document";
        if(phase==7){
            const auto sibling=path+L"\\Neighbor";
            const VolumeCoord::Lease lease({{VolumeCoord::PathKey(sibling),VolumeCoord::Mode::Read}});
            bool deferred=false;
            try{WindowsMoveUndo::RecoverExports(canonical);}
            catch(const VolumeCoord::Busy&){deferred=true;}
            Check(deferred&&GetFileAttributesW(child.c_str())!=INVALID_FILE_ATTRIBUTES&&
                  GetFileAttributesW((path+L"\\%Document").c_str())==INVALID_FILE_ATTRIBUTES,
                  "browse recovery wrote while another operation held a descendant lease");
        }
        auto host=MacBrowser::OpenWindows(path);
        VerifyFile(*host,2,u"Neighbor",false);
        if(phase==9){
            Check(GetFileAttributesW(child.c_str())==INVALID_FILE_ATTRIBUTES&&
                  GetFileAttributesW((path+L"\\%Document").c_str())==INVALID_FILE_ATTRIBUTES,
                  "committed paired delete kept the primary or companion");
        }else VerifyFile(*host,2,u"Document",false);
        Check(host->List(2).size()==(phase==9?1:2),"paired delete left unexpected files");
    }
    for(bool tree:{false,true}){
        const auto folder=root+(tree?"\\changed-tree-stream":"\\changed-file-stream");
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create staged stream verification fixture");
        const auto path=VolumeCoord::Wide(folder.GetString());
        {
            auto host=MacBrowser::OpenWindows(path);
            auto parent=tree?host->MakeDirectory(2,u"Tree"):2;
            if(tree)parent=host->MakeDirectory(parent,u"Nested");
            Put(*host,false,false,parent);
            Put(*host,false,false,2,u"Neighbor");
        }
        CrashChild(folder,9,tree?L"recursive-windows-delete":L"paired-windows-delete");
        WIN32_FIND_DATAA data{};
        const auto search=FindFirstFileA(folder+"\\.hfv-export-*",&data);
        Check(search!=INVALID_HANDLE_VALUE,"find committed Windows staging directory");
        FindClose(search);
        const auto staged=folder+"\\"+data.cFileName+(tree?"\\1\\Nested\\Document":"\\2");
        const auto stream=staged+":Late";
        auto file=CreateFileA(staged,FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        Check(file!=INVALID_HANDLE_VALUE,"open staged original");
        FILETIME modified{};
        Check(GetFileTime(file,nullptr,nullptr,&modified)!=FALSE,"capture staged original timestamp");
        Save(stream,Bytes{23,45,67});
        Check(SetFileTime(file,nullptr,nullptr,&modified)!=FALSE,"restore primary timestamp after stream change");
        CloseHandle(file);
        bool rejected=false;
        try{MacBrowser::OpenWindows(path);}
        catch(const HfsPlus::Error& error){rejected=std::string(error.what()).find("alternate streams")!=std::string::npos;}
        Check(rejected&&Load(stream)==Bytes({23,45,67})&&
              GetFileAttributesA(folder+"\\Neighbor")!=INVALID_FILE_ATTRIBUTES,
              "committed Windows recovery deleted a changed named stream");
    }
    fprintf(report,"PASS: committed Windows recovery keeps newly added streams on staged files and nested originals.\n");
    fprintf(report,"PASS: paired NTFS Delete restores both forks on cancellation and interruption, completes after commit, and waits for an exclusive folder lease before recovery.\n");
    for(unsigned phase:{0u,8u,9u}){
        CString folder;
        folder.Format("%s\\recursive-paired-delete-%u",root.GetString(),phase);
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create recursive paired delete fixture");
        const auto path=VolumeCoord::Wide(folder.GetString());
        {
            auto host=MacBrowser::OpenWindows(path);
            const auto tree=host->MakeDirectory(2,u"Tree");
            const auto nested=host->MakeDirectory(tree,u"Nested");
            Put(*host,false,false,nested);
            Put(*host,false,false,2,u"Neighbor");
            Check(host->Stat(tree).identity.companion.key.empty(),"recursive test root unexpectedly has a companion");
        }
        const auto lateFile=folder+"\\Tree\\Nested\\Late";
        const Bytes lateBytes{17, 29, 43};
        if(phase==0){
            bool injected=false;
            MacBrowser::windowsPublicationFault=[&](const char* step){
                if(!strcmp(step,"tree-inventoried-before-staging")){
                    Save(lateFile,lateBytes);
                    injected=true;
                }
            };
            struct ClearFault{~ClearFault(){MacBrowser::windowsPublicationFault={};}} clear;
            Storage::Job job;
            const auto result=DeleteWindowsFixture(path,job,L"Tree");
            Check(injected&&result.code&&result.message.find("changed while staging")!=std::string::npos&&
                  Load(lateFile)==lateBytes,
                  "late child was lost or left in staging");
        }else CrashChild(folder,phase,L"recursive-windows-delete");
        auto host=MacBrowser::OpenWindows(path);
        VerifyFile(*host,2,u"Neighbor",false);
        MacBrowser::Entry tree{};
        if(phase!=9){
            const auto nested=Find(*host,Find(*host,2,u"Tree"),u"Nested");
            VerifyFile(*host,nested,u"Document",false);
        }else Check(!host->FindChild(2,u"Tree",tree),"committed recursive delete retained the selected folder");
        Check(host->List(2).size()==(phase==9?1:2),"recursive delete left unexpected files");
    }
    fprintf(report,"PASS: recursive NTFS Delete recovers nested AppleDouble pairs under an unpaired folder before commit removes the complete tree after commit, and rolls back a late addition to the original path.\n");
    for(unsigned kind:{0u,1u,2u})for(bool cut:{false,true})for(unsigned phase:{7u,9u}){
        CString image;
        image.Format("%s\\modern-export-%u-%u-%u.dsk",root.GetString(),kind,unsigned(cut),phase);
        const auto path=VolumeCoord::Wide(image.GetString()),hostPath=path+L".windows";
        auto disk=HfsPlus::CreateImage(path.c_str(),16*1024*1024);
        Storage::Job format;
        using Kind=HfsPlus::Volume::FormatKind;
        if(kind==2)Apfs::Format(*disk,u"Export modern",true,format);
        else HfsPlus::Volume::Format(*disk,u"Export modern",true,kind?Kind::HfsXCaseSensitive:Kind::HfsPlus);
        disk.reset();
        const auto classicPath=path+L".classic.dsk";
        CString classicFile=image+".classic.dsk";
        Save(classicFile,Bytes(4*1024*1024));
        char label[]="Export classic";
        const auto formatted=hfs_format(classicFile.GetBuffer(),0,label);
        classicFile.ReleaseBuffer();
        Check(formatted==0,"format classic mixed export fixture");
        Check(CreateDirectoryW(hostPath.c_str(),nullptr)!=FALSE,"create modern export destination");
        {
            auto source=OpenCrossFormatFixture(path);
            auto classic=MacBrowser::OpenImage(classicPath);
            auto host=MacBrowser::OpenWindows(hostPath);
            Put(*source,true);
            Put(*classic,true,false,2,u"Second");
            Put(*host,false);
            Put(*host,false,false,2,u"Second");
            Put(*host,false,false,2,u"Neighbor");
        }
        const auto sourceBefore=Load(image),classicBefore=Load(image+".classic.dsk");
        CrashChild(image,phase,cut?L"mixed-windows-cut":L"mixed-windows-export");
        auto host=MacBrowser::OpenWindows(hostPath);
        VerifyFile(*host,2,u"Document",phase==9);
        VerifyFile(*host,2,u"Second",phase==9);
        VerifyFile(*host,2,u"Neighbor",false);
        Check(host->List(2).size()==3&&Load(image)==sourceBefore&&Load(image+".classic.dsk")==classicBefore,
              "interrupted mixed export changed a source or kept unexpected destination entries");
    }
    fprintf(report,"PASS: mixed classic HFS and HFS+/HFSX/APFS Copy and Cut recover interrupted NTFS overwrites and preserve both forks and untouched sources.\n");
    for(unsigned kind:{0u,1u,2u})for(unsigned phase:{7u,9u}){
        CString image;
        image.Format("%s\\windows-cut-modern-%u-%u.dsk",root.GetString(),kind,phase);
        const auto path=VolumeCoord::Wide(image.GetString()),hostPath=path+L".windows";
        auto disk=HfsPlus::CreateImage(path.c_str(),16*1024*1024);
        Storage::Job format;
        using Kind=HfsPlus::Volume::FormatKind;
        if(kind==2)Apfs::Format(*disk,u"Cut destination",true,format);
        else HfsPlus::Volume::Format(*disk,u"Cut destination",true,kind?Kind::HfsXCaseSensitive:Kind::HfsPlus);
        disk.reset();
        Check(CreateDirectoryW(hostPath.c_str(),nullptr)!=FALSE,"create paired Windows cut source");
        {
            auto host=MacBrowser::OpenWindows(hostPath);
            Put(*host,true);
            Put(*host,false,false,2,u"Neighbor");
            auto destination=OpenCrossFormatFixture(path);
            Put(*destination,false);
        }
        CrashChild(image,phase,L"windows-cut-to-modern");
        auto destination=OpenCrossFormatFixture(path);
        VerifyFile(*destination,2,u"Document",true);
        auto host=MacBrowser::OpenWindows(hostPath);
        VerifyFile(*host,2,u"Neighbor",false);
        if(phase==7)VerifyFile(*host,2,u"Document",true);
        else Check(GetFileAttributesW((hostPath+L"\\Document").c_str())==INVALID_FILE_ATTRIBUTES&&
                   GetFileAttributesW((hostPath+L"\\%Document").c_str())==INVALID_FILE_ATTRIBUTES,
                   "committed Windows cut left an orphaned source file");
        Check(host->List(2).size()==(phase==7?2:1),"Windows cut recovery left unexpected entries");
    }
    fprintf(report,"PASS: Windows Cut to HFS+, HFSX and APFS restores both source files before cleanup commit and removes both after commit while preserving the destination forks.\n");
    for(unsigned phase:{7u,8u,9u}){
        CString folder;
        folder.Format("%s\\windows-metadata-%u",root.GetString(),phase);
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create Windows metadata recovery fixture");
        const auto path=VolumeCoord::Wide(folder.GetString());
        {
            auto host=MacBrowser::OpenWindows(path);
            Put(*host,false);
            Put(*host,false,false,2,u"Neighbor");
        }
        const auto stream=folder+"\\%Document:Notes";
        const Bytes notes{24,32,48,72};
        Save(stream,notes);
        CrashChild(folder,phase,L"windows-metadata-replace");
        auto host=MacBrowser::OpenWindows(path);
        VerifyFile(*host,2,u"Neighbor",false);
        const auto entry=host->Stat(Find(*host,2,u"Document"));
        const auto old=Entry(false),replacement=Entry(phase==9);
        Check(entry.forks[0].size==old.forks[0].size&&entry.forks[1].size==replacement.forks[1].size&&
              entry.finder==old.finder,"Windows metadata recovery changed the primary file or Finder information");
        for(unsigned fork=0;fork<2;++fork){
            Bytes data(size_t(entry.forks[fork].size));
            host->Read(entry.id,fork,0,data.data(),data.size());
            for(size_t at=0;at<data.size();++at)
                Check(data[at]==Byte(fork==1&&phase==9,fork,at),"Windows metadata recovery restored the wrong fork bytes");
        }
        Check(Load(stream)==notes&&host->List(2).size()==2,
              "Windows metadata recovery lost an alternate stream or left staging");
    }
    fprintf(report,"PASS: standalone Windows resource-fork replacement restores the old companion before commit and retains the new companion after commit, preserving the primary file and alternate streams.\n");
    const Bytes exportBytes(2*1024*1024+29,0xb5),decoy{17,43,92};
    for(bool attribute:{false,true})for(unsigned phase:{7u,9u,10u,11u}){
        CString folder;
        folder.Format("%s\\standalone-export-%u-%u",root.GetString(),unsigned(attribute),phase);
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create standalone export fixture");
        Save(folder+"\\input.bin",phase==11?Bytes{}:exportBytes);
        const CString output=folder+"\\output.bin";
        CrashChild(folder,phase,attribute?L"atomic-attribute-export":L"atomic-fork-export");
        if(phase==7)
            Check(GetFileAttributesA(output)==INVALID_FILE_ATTRIBUTES&&StagedImages(folder)==1,
                  "interrupted export exposed a partial final file");
        else{
            const auto expected=phase==10?decoy:phase==11?Bytes{}:exportBytes;
            Check(Load(output)==expected&&StagedImages(folder)==0,
                  "standalone export truncated its data, replaced an existing name, or leaked staging");
        }
    }
    fprintf(report,"PASS: standalone fork and attribute exports publish only complete data, retain a late destination collision, and support empty exports.\n");
    for(bool wave:{false,true})for(unsigned phase:{7u,9u}){
        CString folder;
        folder.Format("%s\\optical-output-%u-%u",root.GetString(),unsigned(wave),phase);
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create interrupted optical export fixture");
        const auto main=folder+(wave?"\\Tracks-01.wav":"\\Disc.bin");
        const auto second=folder+(wave?"\\Tracks-02.wav":"\\Disc.cue");
        const Bytes neighbor{5,9,13};
        Save(folder+"\\Neighbor",neighbor);
        CrashChild(folder,phase,wave?L"optical-wav-crash":L"optical-cue-crash");
        Check(GetFileAttributesA(main)!=INVALID_FILE_ATTRIBUTES,"the optical subprocess never published its first output");
        auto host=MacBrowser::OpenWindows(VolumeCoord::Wide(folder.GetString()));
        if(phase==7){
            Check(GetFileAttributesA(main)==INVALID_FILE_ATTRIBUTES&&GetFileAttributesA(second)==INVALID_FILE_ATTRIBUTES,
                  "interrupted optical recovery retained a partial output set");
        }else{
            const auto first=Load(main),last=Load(second);
            Check(first.size()==(wave?2u*2352+44:34u*2352),"committed optical recovery truncated the first output");
            if(wave)Check(last.size()==32u*2352+44,"committed optical recovery truncated the second WAV");
            else Check(std::string(last.begin(),last.end()).find("Disc.bin")!=std::string::npos,
                       "committed CUE lost its BIN reference");
        }
        Check(Load(folder+"\\Neighbor")==neighbor&&StagedImages(folder)==0&&host->List(2).size()==(phase==7?1:3),
              "optical recovery changed unrelated data or kept staging");
    }
    fprintf(report,"PASS: BIN/CUE and multi-WAV output sets roll back together before commit and survive together after commit.\n");
    for(const auto* action:{L"format-mfs-image",L"format-hfsx-image",L"format-external-image"}){
        const bool external=!wcscmp(action,L"format-external-image");
        for(unsigned phase:{7u,9u}){
            if(!external&&phase==9)continue;
            CString folder;
            folder.Format("%s\\%ls-%u",root.GetString(),action,phase);
            Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create formatting crash fixture");
            CrashChild(folder,phase,action);
            auto windows=MacBrowser::OpenWindows(VolumeCoord::Wide(folder.GetString()));
            const auto home=folder+"\\Format.dsk",journal=folder+"\\Journal.img";
            if(phase==7){
                Check(GetFileAttributesA(home)==INVALID_FILE_ATTRIBUTES&&
                      GetFileAttributesA(journal)==INVALID_FILE_ATTRIBUTES,
                      "interrupted formatting exposed incomplete final files");
            }else{
                const auto h=VolumeCoord::Wide(home.GetString()),j=VolumeCoord::Wide(journal.GetString());
                auto image=HfsPlus::OpenImage(h.c_str(),false,j.c_str());
                Check(HfsPlus::Volume(*image,false).ExternalJournal(),"committed external-journal format failed to reopen");
            }
        }
    }
    for(unsigned phase:{7u,9u}){
        CString folder;
        folder.Format("%s\\media-overwrite-%u",root.GetString(),phase);
        Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create removable-image overwrite fixture");
        const auto path=folder+"\\Disc.iso";
        const Bytes original{3,7,11},neighbor{2,4,6};
        Save(path,original);
        Save(folder+"\\Neighbor",neighbor);
        CrashChild(folder,phase,L"media-image-overwrite");
        if(phase==7)Check(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES,
                          "overwrite did not stop between staging and publication");
        auto windows=MacBrowser::OpenWindows(VolumeCoord::Wide(folder.GetString()));
        Bytes expected=original;
        if(phase==9){
            expected.resize(3*65536+2048);
            for(size_t i=0;i<expected.size();++i)expected[i]=BYTE(i*71+i/2048);
            expected[0]='E';expected[1]='R';expected[512]='P';expected[513]='M';
        }
        Check(Load(path)==expected&&Load(folder+"\\Neighbor")==neighbor&&
              StagedImages(folder)==0&&windows->List(2).size()==2,
              "removable-image overwrite recovery failed or changed an unrelated file");
    }
    fprintf(report,"PASS: interrupted removable image overwrite restores the old image before commit and preserves the new image after commit.\n");
    fprintf(report,"PASS: interrupted MFS and HFSX formatting leaves final names absent; interrupted external-journal formatting recovers both outputs together.\n");
    RunLauncherInterruptionTests(root,report);
}
int RunMediaImageCrashChild(const char*,unsigned);
int RunRawOpticalCrashChild(const char*,unsigned,bool);

int RunClassicRecoveryChild(const char* image,unsigned phase,const char* action)
{
    try{
        readonly_cd_backend_init();
        if(!strcmp(action,"format")){
            if(phase==10){
                HfsPlus::workerIoFault=[writes=0u](bool write,uint64_t,size_t) mutable {
                    if(write&&++writes==2)ExitProcess(100);
                };
            }
            classicFormatTestStage=[phase](ClassicFormatStage stage,HfsPlus::Source&){
                if((phase==11&&stage==ClassicFormatStage::Publishing)||
                   (phase==12&&stage==ClassicFormatStage::Published))ExitProcess(90+phase);
            };
            FormatClassicImage(VolumeCoord::Wide(image),FormatLabel,FormatBytes);
            return 3;
        }
        ClassicHfsRecovery::Transaction::CrashAt(phase);
        if(!strcmp(action,"launcher-fake-editor")){
            char executable[MAX_PATH]{};
            if(!GetModuleFileNameA(nullptr,executable,MAX_PATH))return 4;
            std::string command="\""+std::string(executable)+"\" --classic-recovery-child \""+image+
                "\" "+std::to_string(phase)+" launcher-editor-save";
            STARTUPINFOA startup{sizeof(startup)};
            PROCESS_INFORMATION child{};
            if(!CreateProcessA(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                               nullptr,nullptr,&startup,&child))return 4;
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            ExitProcess(90+phase);
        }
        if(!strcmp(action,"launcher-editor-save")){
            const CString folder=CString(image)+"\\hfv123.tmp.dir";
            const CString edit=folder+"\\Edit.dat",replacement=folder+"\\Editor.save";
            Save(replacement,Bytes{9,8,7,6});
            if(!MoveFileExA(replacement,edit,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return 4;
            if(phase==1)Save(edit+":EditorState",Bytes{4,2});
            Save(folder+"\\editor-saved",Bytes{1});
            return 0;
        }
        if(!strcmp(action,"launcher-write-back")){
            auto pending=LauncherWriteBack::LoadPending(CString(image));
            if(pending.size()!=1)return 4;
            const auto path=VolumeCoord::Wide(pending.front()->TemporaryPath().GetString());
            LauncherTemporaryDocument file;
            if(file.Open(path.c_str(),true).code)return 4;
            Storage::Job job;
            const auto result=pending.front()->ApplyPending(file,job);
            if(phase==11&&result.error==ERROR_NOT_SUPPORTED)ExitProcess(101);
            if(phase==10&&!result.code&&pending.front()->RetireCommitted())ExitProcess(100);
            return 4;
        }
        if(!strcmp(action,"media-image-overwrite"))return RunMediaImageCrashChild(image,phase);
        const bool formatMfs=!strcmp(action,"format-mfs-image");
        const bool formatHfsx=!strcmp(action,"format-hfsx-image");
        const bool formatExternal=!strcmp(action,"format-external-image");
        if(formatMfs||formatHfsx||formatExternal){
            if(formatExternal&&phase==7)
                MacBrowser::windowsPublicationFault=[](const char* step){
                    if(!strcmp(step,"format-journal-published"))ExitProcess(97);
                };
            if(!formatExternal)
                HfsPlus::workerIoFault=[](bool write,uint64_t,size_t){if(write)ExitProcess(97);};
            const auto root=VolumeCoord::Wide(image);
            MacImageFormatRequest request{root+L"\\Format.dsk",formatExternal?root+L"\\Journal.img":L"",
                u"Format Test",formatMfs?400ULL*1024:8ULL*1024*1024,
                formatHfsx?HfsPlus::Volume::FormatKind::HfsXCaseSensitive:HfsPlus::Volume::FormatKind::HfsPlus,
                !formatMfs,formatMfs,formatMfs};
            Storage::Job job;
            return MakeMacImageFormatJob(std::move(request))(job).code?4:3;
        }
        const bool opticalWave=!strcmp(action,"optical-wav-crash");
        if(opticalWave||!strcmp(action,"optical-cue-crash"))
            return RunRawOpticalCrashChild(image,phase,opticalWave);
        if(!strcmp(action,"recursive-windows-delete")){
            Storage::Job job;
            const auto result=DeleteWindowsFixture(VolumeCoord::Wide(image),job,L"Tree");
            return result.code?4:3;
        }
        if(!strcmp(action,"paired-windows-delete")){
            if(phase==7)MacBrowser::windowsPublicationFault=[](const char* step){
                if(!strcmp(step,"delete-companion-staged"))ExitProcess(97);
            };
            Storage::Job job;
            const auto result=DeleteWindowsFixture(VolumeCoord::Wide(image),job);
            return result.code?4:3;
        }
        if(!strcmp(action,"paired-windows-rename")){
            auto volume=MacBrowser::OpenWindows(VolumeCoord::Wide(image));
            if(phase==7||phase==12)
                MacBrowser::windowsPublicationFault=[phase](const char* step){
                    if(!strcmp(step,"rename-companion-published"))ExitProcess(90+phase);
                };
            volume->Rename(Find(*volume,2,u"Document"),2,phase==12?u"DOCUMENT":u"Renamed");
            return 3;
        }
        const bool attributeExport=!strcmp(action,"atomic-attribute-export");
        if(attributeExport||!strcmp(action,"atomic-fork-export")){
            const CString folder=image,input=folder+"\\input.bin",output=folder+"\\output.bin";
            auto source=FileOperations::Location::Windows(VolumeCoord::Wide(input.GetString()));
            auto destination=FileOperations::Location::Windows(VolumeCoord::Wide(output.GetString()));
            FileOperations::Request request=attributeExport?
                FileOperations::Request(FileOperations::ExportAttributeValue{destination,HfsPlus::AttributeData(Load(input))}):
                FileOperations::Request(FileOperations::ExportFileFork{source,destination,FileOperations::ForkSelector::Data});
            if(phase==7||phase==10)
                HfsPlus::workerIoFault=[phase,output,writes=0u](bool write,uint64_t,size_t) mutable {
                    if(write&&++writes==2){
                        if(phase==7)ExitProcess(97);
                        Save(output,Bytes{17,43,92});
                    }
                };
            Storage::Job job;
            FileOperations::Completion completion;
            const auto result=FileOperations::ExecuteInWorker(request,job,completion);
            if(phase==10?result.code!=0:!result.code)ExitProcess(90+phase);
            return 4;
        }
        if(!strcmp(action,"windows-metadata-replace")){
            auto host=MacBrowser::OpenWindows(VolumeCoord::Wide(image));
            const auto id=Find(*host,2,u"Document");
            auto entry=host->Stat(id);
            entry.forks[1].size=Entry(true).forks[1].size;
            if(phase==7)MacBrowser::windowsPublicationFault=[](const char* step){
                if(!strcmp(step,"metadata-original-staged"))ExitProcess(97);
            };
            Storage::Job job;
            host->PutResourceReplacementForJob(id,entry,[](unsigned fork,uint64_t at,void* data,size_t n){
                auto out=static_cast<uint8_t*>(data);
                for(size_t i=0;i<n;++i)out[i]=Byte(true,fork,at+i);
            },{},job);
            return 3;
        }
        if(!strcmp(action,"windows-cut-to-modern")){
            const auto path=VolumeCoord::Wide(image);
            auto destination=OpenCrossFormatFixture(path);
            if(phase==7)MacBrowser::windowsPublicationFault=[](const char* step){
                if(!strcmp(step,"delete-companion-staged"))ExitProcess(97);
            };
            Storage::Job job;
            job.transport({},[](const std::string&){return Storage::ConflictChoice::Replace;});
            auto source=FileOperations::Location::Windows(path+L".windows\\Document");
            source.cut=true;
            FileOperations::OrdinaryCopy request{{std::move(source)},FileOperations::Location::Mounted(destination,2)};
            request.destination.directory=true;
            FileOperations::Completion completion;
            const auto result=FileOperations::ExecuteInWorker(FileOperations::Request(std::move(request)),job,completion);
            return result.code?4:3;
        }
        if(!strcmp(action,"mixed-windows-export")||!strcmp(action,"mixed-windows-cut")){
            const auto path=VolumeCoord::Wide(image);
            auto modern=OpenCrossFormatFixture(path);
            auto classic=MacBrowser::OpenImage(path+L".classic.dsk");
            if(phase==7)MacBrowser::windowsPublicationFault=[](const char* step){
                if(!strcmp(step,"data-published"))ExitProcess(97);
            };
            Storage::Job job;
            job.transport({},[](const std::string&){return Storage::ConflictChoice::Replace;});
            FileOperations::Completion completion;
            FileOperations::OrdinaryCopy request{{
                FileOperations::Location::Mounted(modern,Find(*modern,2,u"Document")),
                FileOperations::Location::Mounted(classic,Find(*classic,2,u"Second"))
            },FileOperations::Location::Windows(path+L".windows")};
            request.destination.directory=true;
            if(!strcmp(action,"mixed-windows-cut"))request.sources.front().cut=true;
            const auto result=FileOperations::ExecuteInWorker(FileOperations::Request(std::move(request)),job,completion);
            return result.code?4:3;
        }
        const bool importing=!strcmp(action,"windows-import");
        const bool copyOnly=!strcmp(action,"export-only");
        if(importing||copyOnly||!strcmp(action,"windows-export")){
            Storage::Job job;
            job.transport([&](const Storage::Progress& progress){
                if(phase==7&&importing&&progress.files==1)ExitProcess(97);
            },[](const std::string&){return Storage::ConflictChoice::Replace;});
            if(phase==7&&!importing)MacBrowser::windowsPublicationFault=[](const char* step){
                if(!strcmp(step,"data-published"))ExitProcess(97);
            };
            auto result=MoveWindowsTree(VolumeCoord::Wide(image),importing,job,importing&&phase!=7,false,copyOnly);
            if(result.code){
                FILE* diagnostics=nullptr;
                const auto file=std::string(image)+".move-error.txt";
                if(!fopen_s(&diagnostics,file.c_str(),"w")){
                    fprintf(diagnostics,"%s\n",result.message.c_str());
                    fclose(diagnostics);
                }
            }
            return result.code?4:3;
        }
        if(!strcmp(action,"mixed-image-move")){
            Storage::Job job;
            job.transport({},[](const std::string&){return Storage::ConflictChoice::Replace;});
            const auto result=MoveAcrossImages(VolumeCoord::Wide(image),job);
            return result.code?4:3;
        }
        if(!strcmp(action,"batch-move")){
            auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(image));
            Storage::Job job;
            job.transport([&](const Storage::Progress& p){if(phase==7&&p.files==1)ExitProcess(97);},{});
            const auto result=MoveSelection(volume,job);
            return result.code?4:3;
        }
        if(!strcmp(action,"batch-copy")||!strcmp(action,"batch-delete")){
            auto source=MacBrowser::OpenImage(VolumeCoord::Wide(image));
            auto target=!strcmp(action,"batch-delete")?nullptr:
                MacBrowser::OpenImage(VolumeCoord::Wide(std::string(image)+".target.dsk"));
            Storage::Job job;
            job.transport([](const Storage::Progress& p){if(p.files==1)ExitProcess(97);},
                          [](const std::string&){return Storage::ConflictChoice::Replace;});
            const auto result=TransferSelection(source,target,job);
            return result.code?4:3;
        }
        if(!strcmp(action,"folder-copy")||!strcmp(action,"folder-move")){
            auto source=MacBrowser::OpenImage(VolumeCoord::Wide(image));
            auto target=MacBrowser::OpenImage(VolumeCoord::Wide(std::string(image)+".target.dsk"));
            Storage::Job job;
            job.transport([&](const Storage::Progress& progress){
                if(phase==7&&progress.files==1)ExitProcess(97);
            },[](const std::string&){return Storage::ConflictChoice::Replace;});
            const auto result=TransferTree(source,target,!strcmp(action,"folder-move"),job);
            return result.code?4:3;
        }
        if(!strcmp(action,"reported-windows-move")){
            Storage::Job job;
            job.transport([&](const Storage::Progress& progress){
                if(progress.phase=="Removing moved sources"&&progress.path==(phase==17?"First":"Second"))
                    ExitProcess(90+phase);
            },{});
            return WindowsReportedMove(CString(image),job).code?4:3;
        }
        if(!strcmp(action,"cross-format-move")||!strcmp(action,"archive-merge-move")){
            Storage::Job job;
            job.transport([&](const Storage::Progress& progress){
                if((phase==19&&progress.phase=="Copying"&&progress.files==1)||
                   (progress.phase=="Removing moved sources"&&progress.path==(phase==17?"First":"Second")))
                    ExitProcess(90+phase);
            },[](const std::string&){return Storage::ConflictChoice::ReplaceAll;});
            const auto result=CrossFormatMove(image,job,phase==19);
            if(result.code){
                FILE* failure=nullptr;
                if(!fopen_s(&failure,(std::string(image)+".cross-format-error.txt").c_str(),"w")){
                    fprintf(failure,"%lu: %s\n",result.error,result.message.c_str());
                    fclose(failure);
                }
            }
            return result.code?4:3;
        }
        auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(image));
        Check(bool(volume),"open classic HFS image in subprocess");
        const auto document=Find(*volume,2,u"Document");
        if(!strcmp(action,"delete"))volume->Remove(document);
        else if(!strcmp(action,"archive-delete")){
            Storage::Job job;
            const auto result=DeleteArchiveSelection(volume,document,job);
            if(result.code)throw HfsPlus::Error(result.message);
        }
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

int RunClassicSiblingRecoveryTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const CString root=directory;
    FILE* report=nullptr;
    if(fopen_s(&report,root+"\\classic-sibling-recovery-report.txt","w")||!report)return 2;
    int result=0;
    try{
        readonly_cd_backend_init();
        SiblingRawRecovery(root,report);
        NdifRecovery(root,report,true);
    }catch(const std::exception& error){
        fprintf(report,"FAIL: %s\n",error.what());
        result=1;
    }
    fclose(report);
    return result;
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
        Check(SetEnvironmentVariableW(L"HFV_TEST_CLASSIC_MOVE_DIR",VolumeCoord::Wide(root.GetString()).c_str())!=FALSE,
              "set isolated classic HFS move decision directory");
        FormatRecovery(root,report);
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
                try { MacBrowser::OpenClassicImage(path,false,VolumeCoord::ImageKey(path)); }
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
        ArchiveDeleteRecovery(root,original,report);
        CrossFormatMoveRecovery(root,report);
        ArchiveMergeInterruption(root,report);
        RenameRecovery(root,original,report);
        {
            const CString empty=root+"\\empty-transfer.dsk";
            FormatClassicImage(VolumeCoord::Wide(empty.GetString()),"Transfer",4*1024*1024);
            FolderTransferRecovery(root,Load(empty),report);
            MixedImageMoveRecovery(root,Load(empty),report);
            WindowsMoveRecovery(root,Load(empty),report);
            WindowsExportRecovery(root,Load(empty),report);
        }
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
