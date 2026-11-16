// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HfsPlus.h"
#include "FloppyWriteLease.h"
#include "DirectFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "FakeFloppy.h"
#include <cstdio>
#include <fcntl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "Filehook.h"
}
namespace {
void require(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
std::wstring wide(const std::string& value){return {value.begin(),value.end()};}
struct Session {
    std::unique_ptr<HfsPlus::Source> image;
    hfsvol* classic=nullptr;
    int legacy=-1;
    FloppyWriteLease floppy;
    bool Open(const std::string& kind,const char* path){
        if(kind=="floppy")return floppy.Acquire(25);
        bool write=kind.find("writer")!=std::string::npos;
        if(kind=="legacy-writer"){legacy=fhook_open(path,O_BINARY|O_RDWR);return legacy>=0;}
        if(kind.find("classic")==0){classic=hfs_mount(const_cast<char*>(path),0,write?O_RDWR:O_RDONLY);
            if(classic&&write){hfsvolent info{};hfs_vstat(classic,&info);if(info.flags&HFS_ISLOCKED){hfs_umount(classic);classic=nullptr;}}
            return classic!=nullptr;}
        try{image=HfsPlus::OpenImage(wide(path).c_str(),write);return true;}catch(const HfsPlus::Error&){return false;}
    }
    ~Session(){if(classic)hfs_umount(classic);if(legacy>=0)fhook_close(legacy);}
};
struct Process {
    PROCESS_INFORMATION info{};
    explicit Process(const std::string& arguments){
        char executable[MAX_PATH];require(GetModuleFileNameA(nullptr,executable,MAX_PATH)!=0,"test executable path");
        std::string command='"'+std::string(executable)+"\" "+arguments;
        STARTUPINFOA startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        require(CreateProcessA(executable,&command[0],nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&info)!=FALSE,"spawn conflicting instance");
        CloseHandle(info.hThread);info.hThread=nullptr;
    }
    DWORD Exit(){require(WaitForSingleObject(info.hProcess,30000)==WAIT_OBJECT_0,"child process timeout");DWORD code;GetExitCodeProcess(info.hProcess,&code);return code;}
    ~Process(){if(info.hProcess){if(WaitForSingleObject(info.hProcess,0)!=WAIT_OBJECT_0){TerminateProcess(info.hProcess,99);WaitForSingleObject(info.hProcess,5000);}CloseHandle(info.hProcess);}}
};
std::string quote(const std::string& text){return '"'+text+'"';}
unsigned serial=0;
struct Holder {
    std::string readyName,releaseName;
    HANDLE ready=nullptr,release=nullptr;
    std::unique_ptr<Process> process;
    Holder(const std::string& kind,const std::string& path){
        std::string prefix="Local\\HFVExplorer.ExclusiveTest."+std::to_string(GetCurrentProcessId())+"."+std::to_string(++serial);
        readyName=prefix+".ready";releaseName=prefix+".release";
        ready=CreateEventA(nullptr,TRUE,FALSE,readyName.c_str());release=CreateEventA(nullptr,TRUE,FALSE,releaseName.c_str());
        require(ready&&release,"create child synchronization events");
        process.reset(new Process("--exclusive-hold "+kind+" "+quote(path)+" "+quote(readyName)+" "+quote(releaseName)));
        HANDLE waits[]={ready,process->info.hProcess};require(WaitForMultipleObjects(2,waits,FALSE,30000)==WAIT_OBJECT_0,"child did not acquire storage access");
    }
    void Finish(){SetEvent(release);require(process->Exit()==0,"holder child failed");}
    ~Holder(){if(release)SetEvent(release);process.reset();if(ready)CloseHandle(ready);if(release)CloseHandle(release);}
};
DWORD attempt(const std::string& kind,const std::string& path){Process child("--exclusive-attempt "+kind+" "+quote(path));return child.Exit();}
std::vector<BYTE> bytes(const std::string& path){
    HANDLE h=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"read image for unchanged-source comparison");
    LARGE_INTEGER size;GetFileSizeEx(h,&size);std::vector<BYTE> data(size_t(size.QuadPart));DWORD got=0;
    bool ok=ReadFile(h,data.data(),DWORD(data.size()),&got,nullptr)&&got==data.size();CloseHandle(h);require(ok,"read comparison bytes");return data;
}
class Writer : public FloppyWriteApi {
public:
    FakeFloppy disk;bool failLock=false,locked=false,checkPeer=false,peerBlocked=false;unsigned writes=0,formats=0;
    explicit Writer(const std::vector<BYTE>& image):disk(image){}
    UINT DriveType(int drive)override{return disk.DriveType(drive);}
    HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f)override{return disk.Open(p,a,s,f);}
    bool Geometry(HANDLE h,DISK_GEOMETRY& g)override{return disk.Geometry(h,g);}
    bool Verify(HANDLE h,DWORD& n)override{return disk.Verify(h,n);}
    bool Read(HANDLE h,ULONGLONG at,DWORD n,void* p,DWORD& got)override{return disk.Read(h,at,n,p,got);}
    void Close(HANDLE h)override{disk.Close(h);}
    bool Lock(HANDLE)override{if(failLock){SetLastError(ERROR_ACCESS_DENIED);return false;}locked=true;return true;}
    bool Dismount(HANDLE)override{return locked;}
    bool Unlock(HANDLE)override{locked=false;return true;}
    bool FormatTracks1440(HANDLE)override{++formats;return locked;}
    bool Write(HANDLE,ULONGLONG at,DWORD n,const void* p,DWORD& got)override{
        if(!locked){SetLastError(ERROR_ACCESS_DENIED);return false;}if(!writes&&checkPeer)peerBlocked=attempt("floppy","unused")==10;++writes;memcpy(disk.media.data()+size_t(at),p,n);got=n;return true;
    }
    bool Flush(HANDLE)override{return true;}
};
}
int RunExclusiveAccessChild(const char* kind,const char* path,const char* readyName,const char* releaseName){
    std::string family=kind;
    if(family.find("hooked-")==0){readonly_cd_backend_init();family.erase(0,7);}
    Session session;bool opened=session.Open(family,path);
    if(!readyName)return opened?0:10;
    if(!opened)return 11;
    HANDLE ready=OpenEventA(EVENT_MODIFY_STATE,FALSE,readyName);
    HANDLE release=OpenEventA(SYNCHRONIZE,FALSE,releaseName);
    if(!ready||!release)return 12;
    SetEvent(ready);DWORD result=WaitForSingleObject(release,30000);CloseHandle(ready);CloseHandle(release);
    return result==WAIT_OBJECT_0?0:13;
}
int RunExclusiveAccessTest(const char* directory){
    if(!CreateDirectoryA(directory,nullptr))return 2;
    std::string root=directory;FILE* report=fopen((root+"\\exclusive-access-report.txt").c_str(),"w");if(!report)return 2;
    try{
        std::string classic=root+"\\classic.dsk";
        {auto file=HfsPlus::CreateImage(wide(classic).c_str(),8*1024*1024);}
        char label[]="Exclusive";require(hfs_format(const_cast<char*>(classic.c_str()),0,label)==0,"format classic fixture");
        for(auto kind:{HfsPlus::Volume::FormatKind::HfsPlus,HfsPlus::Volume::FormatKind::HfsXCaseSensitive,HfsPlus::Volume::FormatKind::HfsXCaseInsensitive}){
            std::string path=root+"\\plus-"+std::to_string(int(kind))+".dsk";
            {auto file=HfsPlus::CreateImage(wide(path).c_str(),8*1024*1024);HfsPlus::Volume::Format(*file,u"Exclusive",true,kind);}
            std::string alias=path+".alias";require(CreateHardLinkA(alias.c_str(),path.c_str(),nullptr)!=FALSE,"create same-file alias");
            {Holder holder("plus-writer",path);auto before=bytes(path);
                require(attempt("plus-writer",path)==10&&attempt("plus-reader",alias)==10,"HFS+ conflicting instance was admitted");
                require(attempt("legacy-writer",alias)==10&&attempt("hooked-legacy-writer",path)==10,"classic file hook bypassed HFS+ writer sharing");
                require(bytes(path)==before,"denied HFS+ opens changed image");holder.Finish();}
            {Holder holder("plus-reader",path);require(attempt("plus-reader",alias)==0,"shared HFS+ readers rejected");
                require(attempt("plus-writer",alias)==10,"writer admitted while HFS+ reader active");holder.Finish();}
            require(attempt("plus-writer",alias)==0,"HFS+ writer not released after close");
            {Holder holder("hooked-legacy-writer",path);require(attempt("plus-writer",alias)==10,"HFS+ bypassed a legacy file-hook writer");holder.Finish();}
        }
        std::string alias=classic+".alias";require(CreateHardLinkA(alias.c_str(),classic.c_str(),nullptr)!=FALSE,"create classic same-file alias");
        {Holder holder("classic-writer",classic);auto before=bytes(classic);
            require(attempt("classic-writer",alias)==10&&attempt("hooked-classic-writer",classic)==10&&attempt("classic-reader",classic)==10,"classic conflicting instance was admitted");
            require(attempt("plus-writer",alias)==10,"HFS+ backend bypassed classic writer sharing");
            require(bytes(classic)==before,"denied classic opens changed image");holder.Finish();}
        {Holder holder("classic-reader",classic);require(attempt("classic-reader",alias)==0,"shared classic readers rejected");
            require(attempt("classic-writer",alias)==10,"classic write open silently downgraded or bypassed reader");holder.Finish();}
        require(attempt("classic-writer",alias)==0,"classic writer not released after close");
        {Holder holder("hooked-classic-writer",classic);require(attempt("classic-writer",alias)==10,"native adapter failed to deny a real-hook writer");holder.Finish();}
        {Holder holder("plus-writer",root+"\\plus-0.dsk");
            require(attempt("classic-writer",classic)==0,"unrelated image was unnecessarily locked");
            require(TerminateProcess(holder.process->info.hProcess,77)!=FALSE&&holder.process->Exit()==77,"terminate image owner");
            require(attempt("plus-writer",root+"\\plus-0.dsk")==0,"terminated image owner retained a write lock");}

        {auto writer=hfs_mount(const_cast<char*>(classic.c_str()),0,O_RDWR);require(writer!=nullptr,"same-process classic writer");
            auto reader=hfs_mount(const_cast<char*>(alias.c_str()),0,O_RDONLY);require(reader==writer,"read access did not reuse writable cache");hfs_umount(reader);hfs_umount(writer);}
        fprintf(report,"PASS: real child-process conflicts for classic HFS, HFS+ and both HFSX modes; hard-link aliases, cross-backend opens, shared readers, denied-write byte preservation and release after close.\n");
        std::vector<BYTE> image;std::string error;require(PrepareFloppyHfs("Lease",image,error),"prepare synthetic floppy");
        {Holder holder("floppy","unused");Writer api(image);FloppyWriter writer(&api);
            require(!writer.WriteImage(25,image,true)&&writer.Error()==ERROR_SHARING_VIOLATION&&!writer.WritesStarted(),"full floppy writer bypassed another instance");
            DirectFloppy direct(&api);require(!direct.Open(25)&&direct.Error()==ERROR_SHARING_VIOLATION,"direct floppy writer bypassed another instance");
            require(!api.disk.opens&&!api.writes&&!api.formats&&api.disk.media==image,"conflicting writer touched simulated media");holder.Finish();}
        {Writer api(image);DirectFloppy direct(&api);require(direct.Open(25),"direct writer after peer release");
            Writer second(image);FloppyWriter writer(&second);require(!writer.WriteImage(25,image)&&!second.disk.opens,"nested same-process writer was admitted");
            require(direct.Finish(),"finish direct writer");}
        {Writer api(image);api.failLock=true;DirectFloppy failed(&api);require(!failed.Open(25),"native lock failure ignored");
            FloppyWriteLease lease;require(lease.Acquire(25)&&api.disk.opens==api.disk.closes,"failed direct open leaked lease/device handle");}
        {Writer api(image);api.failLock=true;FloppyWriter writer(&api);require(!writer.WriteImage(25,image)&&!api.writes&&!api.formats,"failed native lock allowed writes");
            FloppyWriteLease lease;require(lease.Acquire(25),"failed full-image operation leaked lease");}
        {Holder holder("floppy","unused");require(TerminateProcess(holder.process->info.hProcess,77)!=FALSE,"terminate simulated owner");
            require(holder.process->Exit()==77,"owner termination failed");require(attempt("floppy","unused")==0,"terminated owner left a permanent floppy lock");}
        {Writer api(image);api.checkPeer=true;FloppyWriter writer(&api);require(writer.WriteImage(25,image)&&api.disk.media==image&&api.peerBlocked,"verified full-image write retains peer exclusion through actual writes");}
        fprintf(report,"PASS: child-process floppy conflicts block direct writes and format/image writes before device opens; same-process exclusion, native-lock failure cleanup, owner termination recovery and successful verified retry. All floppy APIs simulated; no physical drive opened.\n");
    }catch(const std::exception& e){fprintf(report,"FAIL: %s (errno=%d Windows=%lu HFS=%s)\n",e.what(),errno,GetLastError(),hfs_error?hfs_error:"");fclose(report);return 1;}
    fclose(report);return 0;
}
#endif
