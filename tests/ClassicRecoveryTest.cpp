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
void VerifyFile(MacBrowser::Volume& volume,uint32_t parent,const char16_t* name,bool updated)
{
    MacBrowser::Entry actual{};
    const auto expected=Entry(updated);
    Check(volume.FindChild(parent,name,actual),"reopened HFS image lost the expected file");
    Check(actual.finder==expected.finder&&actual.created==expected.created&&actual.modified==expected.modified,
          "reopened HFS image changed Finder information or timestamps");
    for(unsigned fork=0;fork<2;++fork){
        Check(actual.forks[fork].size==expected.forks[fork].size,"reopened HFS fork has the wrong length");
        Bytes bytes(size_t(actual.forks[fork].size));
        volume.Read(actual.id,fork,0,bytes.data(),bytes.size());
        for(size_t i=0;i<bytes.size();++i)
            Check(bytes[i]==Byte(updated,fork,i),"reopened HFS fork has different bytes");
    }
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
void CrashChild(const CString& image,unsigned phase,bool deleting=false)
{
    wchar_t executable[MAX_PATH]{};
    const auto length=GetModuleFileNameW(nullptr,executable,MAX_PATH);
    Check(length&&length<MAX_PATH,"locate recovery test executable");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --classic-recovery-child \""+
                         VolumeCoord::Wide(image.GetString())+L"\" "+std::to_wstring(phase)+(deleting?L" delete":L" overwrite");
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                         nullptr,nullptr,&startup,&child)!=FALSE,"start overwrite crash subprocess");
    CloseHandle(child.hThread);
    const auto wait=WaitForSingleObject(child.hProcess,45000);
    DWORD exit=0;
    if(wait!=WAIT_OBJECT_0)TerminateProcess(child.hProcess,3);
    GetExitCodeProcess(child.hProcess,&exit);
    CloseHandle(child.hProcess);
    if(wait!=WAIT_OBJECT_0||exit!=90+phase)
        throw HfsPlus::Error("Overwrite subprocess missed crash phase "+std::to_string(phase)+
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
        CrashChild(image,phase,true);
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

}

int RunClassicRecoveryChild(const char* image,unsigned phase,bool deleting)
{
    try{
        readonly_cd_backend_init();
        ClassicHfsRecovery::Transaction::CrashAt(phase);
        auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(image));
        Check(bool(volume),"open classic HFS image in subprocess");
        if(deleting)volume->Remove(Find(*volume,2,u"Document"));
        else if(phase!=5)Put(*volume,true);
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
    }catch(const std::exception& error){
        fprintf(report,"FAIL: %s\n",error.what());
        result=1;
    }
    fclose(report);
    return result;
}
#endif
