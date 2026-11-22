// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "WindowsMoveUndo.h"
#include "WindowsHandleRename.h"
#include "ClassicHfsRecovery.h"
#include "CopyWorkerProtocol.h"
#include "StorageWorker.h"
#include <map>
#include <set>
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"

namespace WindowsMoveUndo {
namespace {
using CopyWorkerProtocol::Data;
using Bytes=std::vector<uint8_t>;
// Each flushed frame precedes the filesystem action it records. An incomplete
// last frame cannot describe a rename that has already been attempted.
constexpr char Magic[]="HFV Windows move 1";
constexpr DWORD Share=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){Close();}
    void Close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
    HANDLE Release(){const auto handle=value;value=INVALID_HANDLE_VALUE;return handle;}
    Handle(const Handle&)=delete;
    explicit operator bool()const{return value!=INVALID_HANDLE_VALUE;}
};
void Check(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
bool Missing(DWORD error){return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;}
std::wstring Strip(std::wstring path)
{
    while(path.size()>7&&path.back()==L'\\')path.pop_back();
    return path;
}
std::wstring Child(const std::wstring& parent,const std::wstring& name)
{
    return parent+(parent.back()==L'\\'?L"":L"\\")+name;
}
std::wstring Parent(const std::wstring& path)
{
    const auto slash=path.find_last_of(L'\\');
    Check(slash!=path.npos,"Invalid Windows move path");
    return Strip(path.substr(0,(slash==6)?7:slash));
}
constexpr wchar_t ExportPrefix[]=L".hfv-export-";
std::wstring Stage(const std::wstring& root,const std::string& id,bool exportOnly)
{
    return Child(root,(exportOnly?ExportPrefix:L".hfv-move-")+std::wstring(id.begin(),id.end()));
}
std::wstring Journal(const std::string& id,bool create,bool exportOnly)
{
    return ClassicHfsRecovery::MoveDecisionPath(id,create)+(exportOnly?L".export":L".windows");
}
Handle Open(const std::wstring& path,DWORD rights,DWORD share=Share)
{
    return Handle(CreateFileW(path.c_str(),rights,share,nullptr,OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
}
struct Identity {
    uint64_t serial=0,file=0;
    bool operator==(const Identity& other)const{
        return serial==other.serial&&file==other.file;
    }
    void Encode(Data& data)const{data.put(serial);data.put(file);}
    static Identity Decode(Data& data){return {data.get(),data.get()};}
};
uint64_t Date(FILETIME time){return (uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;}
FILETIME Time(uint64_t date){return {DWORD(date),DWORD(date>>32)};}
Identity IdentityOf(const BY_HANDLE_FILE_INFORMATION& info)
{
    return {info.dwVolumeSerialNumber,(uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow};
}
BY_HANDLE_FILE_INFORMATION Info(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(handle,&info)!=FALSE&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),
          "Cannot identify Windows move file or folder");
    return info;
}
Identity Identify(const std::wstring& path)
{
    auto h=Open(path,FILE_READ_ATTRIBUTES);
    if(!h){
        Check(Missing(GetLastError()),"Cannot inspect a Windows move path");
        return {};
    }
    return IdentityOf(Info(h.value));
}
bool Under(const std::wstring& path,const std::wstring& folder)
{
    if(path.size()<=folder.size()||_wcsnicmp(path.c_str(),folder.c_str(),folder.size())||
       (folder.back()!=L'\\'&&path[folder.size()]!=L'\\'))return false;
    auto relative=path.substr(folder.size()+(folder.back()==L'\\'?0:1));
    if(relative.find_first_of(L":/\0",0,3)!=relative.npos)return false;
    for(size_t at=0;at<relative.size();){
        const auto end=relative.find(L'\\',at);
        const auto name=relative.substr(at,end-at);
        if(name.empty()||name==L"."||name==L".."||name.back()==L'.'||name.back()==L' ')return false;
        if(end==relative.npos)return true;
        at=end+1;
    }
    return false;
}
void Paths(const std::wstring& root,const std::wstring& stage,const std::wstring& from,const std::wstring& to)
{
    Check(Under(from,root)&&Under(to,root)&&Under(from,stage)!=Under(to,stage),
          "Windows move rename escaped its recorded folder");
}
void Move(HANDLE from,const std::wstring& to)
{
    if(!RenameWindowsHandle(from,to))
        throw HfsPlus::Error("Cannot rename a Windows move participant (Windows error "+
                             std::to_string(GetLastError())+")");
}
void Remove(HANDLE file,const char* message)
{
    FILE_DISPOSITION_INFO remove{TRUE};
    Check(SetFileInformationByHandle(file,FileDispositionInfo,&remove,sizeof(remove))!=FALSE,message);
}
void Flush(HANDLE handle){Check(FlushFileBuffers(handle)!=FALSE,"Cannot flush Windows move recovery");}
void Write(HANDLE handle,const void* bytes,DWORD size)
{
    DWORD done=0;
    Check(WriteFile(handle,bytes,size,&done,nullptr)&&done==size,"Cannot write Windows move recovery");
}
void Read(HANDLE handle,void* bytes,DWORD size)
{
    DWORD done=0;
    Check(ReadFile(handle,bytes,size,&done,nullptr)&&done==size,"Cannot read Windows move recovery");
}
uint32_t Crc(const Bytes& bytes){return crc32(0,bytes.data(),uInt(bytes.size()));}
void WriteFrame(HANDLE handle,const Data& data)
{
    const uint32_t header[]={uint32_t(data.bytes.size()),Crc(data.bytes)};
    Write(handle,header,sizeof(header));
    Write(handle,data.bytes.data(),header[0]);
}
void Frame(HANDLE handle,const Data& data)
{
    WriteFrame(handle,data);
    Flush(handle);
}
enum class Op:uint64_t {Stage=1,Rename,Dates,Done,Owned,TreeEntry};
uint64_t FileLength(const BY_HANDLE_FILE_INFORMATION& info)
{
    return (uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
}
struct Record {
    Op op;
    std::wstring from,to;
    Identity object,fromParent,toParent;
    uint64_t creation=0,modified=0,length=0;
    bool directory=false;
};
struct StagedEntry {
    Identity identity;
    std::optional<uint64_t> modified;
    std::optional<uint64_t> length;
};
struct Loaded {
    std::wstring root;
    Identity rootIdentity,stageIdentity;
    std::vector<Record> records;
    std::map<std::wstring,StagedEntry> owned;
    bool done=false;
    uint64_t end=0;
};
void AddOwned(Loaded& log,const std::wstring& stage,const std::wstring& path,StagedEntry entry,bool nested=false)
{
    Check(Under(path,stage)&&(Parent(path)!=stage)==nested,"Invalid Windows staging entry");
    const auto [known,inserted]=log.owned.try_emplace(path,entry);
    Check(inserted||(known->second.identity==entry.identity&&!nested),"Conflicting Windows staging identities");
}
Loaded Load(HANDLE handle,const std::string& id,bool exportOnly)
{
    LARGE_INTEGER length{};
    Check(GetFileSizeEx(handle,&length)!=FALSE&&length.QuadPart>=0,"Cannot size Windows move recovery");
    const auto size=uint64_t(length.QuadPart);
    Loaded log;
    auto remaining=size;
    while(remaining>=8){
        uint32_t prefix[2]{};
        Read(handle,prefix,sizeof(prefix));
        remaining-=sizeof(prefix);
        Check(prefix[0]&&prefix[0]<=512*1024,"Invalid Windows move recovery record length");
        if(prefix[0]>remaining)break;
        Data data;
        data.bytes.resize(prefix[0]);
        Read(handle,data.bytes.data(),prefix[0]);
        remaining-=prefix[0];
        Check(!log.done&&Crc(data.bytes)==prefix[1],"Damaged Windows move recovery record");
        if(log.root.empty()){
            Check(data.text()==Magic&&data.text()==id,"Invalid Windows move recovery header");
            log.root=data.wide();
            log.rootIdentity=Identity::Decode(data);
            Check(log.root.size()>=7&&log.root.compare(0,4,L"\\\\?\\")==0&&log.root[5]==L':'&&log.root[6]==L'\\'&&
                  log.root.find(L'\0')==log.root.npos&&log.rootIdentity.file,"Invalid Windows recovery folder");
        }else{
            Record record{};
            record.op=Op(data.get());
            const auto stage=Stage(log.root,id,exportOnly);
            if(record.op==Op::Stage){
                Check(!log.stageIdentity.file&&log.records.empty(),"Unexpected Windows move staging record");
                log.stageIdentity=Identity::Decode(data);
                Check(log.stageIdentity.file!=0,"Invalid Windows move staging identity");
            }else if(record.op==Op::Done){
                log.done=true;
            }else{
                Check(log.stageIdentity.file!=0,"Windows move lacks a staging record");
                record.from=data.wide();
                record.object=Identity::Decode(data);
                Check(record.object.file!=0,"Invalid Windows move file identity");
                if(record.op==Op::Rename){
                    record.to=data.wide();
                    record.fromParent=Identity::Decode(data);
                    record.toParent=Identity::Decode(data);
                    record.directory=data.flag();
                    record.modified=data.get();
                    record.length=data.get();
                    Paths(log.root,stage,record.from,record.to);
                    AddOwned(log,stage,Under(record.from,stage)?record.from:record.to,{record.object});
                }else if(record.op==Op::Owned){
                    AddOwned(log,stage,record.from,{record.object});
                }else if(record.op==Op::TreeEntry){
                    StagedEntry entry{record.object};
                    if(!data.flag()){
                        entry.modified=data.get();
                        entry.length=data.get();
                    }
                    AddOwned(log,stage,record.from,entry,true);
                }else if(record.op==Op::Dates){
                    Check(Under(record.from,log.root)&&!Under(record.from,stage),"Invalid Windows folder timestamp record");
                    record.creation=data.get();
                    record.modified=data.get();
                }else throw HfsPlus::Error("Unknown Windows move recovery record");
                log.records.push_back(std::move(record));
            }
        }
        data.end();
        log.end=size-remaining;
    }
    return log;
}
void UndoRename(const std::wstring& stage,const Record& record)
{
    const auto original=Identify(record.from);
    if(original==record.object)return;
    const bool published=Under(record.from,stage);
    auto file=Open(record.to,FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ);
    if(!file){
        const auto error=GetLastError();
        // A previous replay can already have removed an empty new folder.
        if(published&&record.directory&&Missing(error)&&!original.file)return;
        throw HfsPlus::Error("Windows move recovery could not reopen a moved entry");
    }
    const auto info=Info(file.value);
    Check(!original.file&&IdentityOf(info)==record.object&&
          Identify(Parent(record.from))==record.fromParent&&Identify(Parent(record.to))==record.toParent,
          "Windows move recovery found a changed file or occupied name; both versions were retained");
    if(published&&record.directory){
        // Delete only an empty directory and bind deletion to the verified ID.
        // Unrelated files created in it since the crash must remain untouched.
        Remove(file.value,"Windows move recovery could not remove a new folder; its contents were retained");
    }else{
        Check(!published||(Date(info.ftLastWriteTime)==record.modified&&FileLength(info)==record.length),
              "A Windows move output was edited before recovery; both versions were retained");
        if(published){
            // Classic HFS exports publish the unnamed file and an AppleDouble
            // companion. A new NTFS stream is an edit even if the file's
            // ordinary size and last-write timestamp still match the journal.
            WIN32_FIND_STREAM_DATA stream{};
            const auto search=FindFirstStreamW(record.to.c_str(),FindStreamInfoStandard,&stream,0);
            if(search!=INVALID_HANDLE_VALUE){
                bool unexpected=false;
                do{if(wcscmp(stream.cStreamName,L"::$DATA")){unexpected=true;break;}}
                while(FindNextStreamW(search,&stream));
                const auto error=GetLastError();
                FindClose(search);
                Check(!unexpected,"A Windows move output has alternate streams; both versions were retained");
                Check(error==ERROR_HANDLE_EOF,"Cannot verify the Windows move output streams");
            }else Check(GetLastError()==ERROR_HANDLE_EOF,"Cannot inspect the Windows move output streams");
        }
        Move(file.value,record.from);
    }
}
std::vector<std::wstring> Children(const std::wstring& folder)
{
    WIN32_FIND_DATAW found{};
    const auto search=FindFirstFileW(Child(folder,L"*").c_str(),&found);
    if(search==INVALID_HANDLE_VALUE){
        Check(GetLastError()==ERROR_FILE_NOT_FOUND,"Cannot inspect Windows move staging");
        return {};
    }
    struct Search{HANDLE handle;~Search(){FindClose(handle);}} guard{search};
    std::vector<std::wstring> names;
    do{
        std::wstring name=found.cFileName;
        if(name!=L"."&&name!=L"..")names.push_back(std::move(name));
    }while(FindNextFileW(search,&found));
    Check(GetLastError()==ERROR_NO_MORE_FILES,"Cannot inspect Windows move staging");
    return names;
}
void CheckStagedEntry(const Loaded& log,const std::wstring& path,
                      const BY_HANDLE_FILE_INFORMATION& info)
{
    const auto known=log.owned.find(path);
    Check(known!=log.owned.end()&&known->second.identity==IdentityOf(info)&&
          (!known->second.modified||*known->second.modified==Date(info.ftLastWriteTime))&&
          (!known->second.length||*known->second.length==FileLength(info)),
          "Windows move staging contains an unrelated or changed entry; it was retained");
}
void CheckStage(const std::wstring& stage,const Loaded& log)
{
    for(const auto& name:Children(stage)){
        const auto path=Child(stage,name);
        auto file=Open(path,FILE_READ_ATTRIBUTES);
        Check(bool(file),"Cannot inspect a Windows staging entry");
        CheckStagedEntry(log,path,Info(file.value));
    }
}
void ClearStagedEntries(const Loaded& log,const std::wstring& folder,unsigned depth=0)
{
    Check(depth<=128,"Windows staging tree is too deep");
    for(const auto& name:Children(folder)){
        const auto path=Child(folder,name);
        auto file=Open(path,FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ);
        if(!file){Check(Missing(GetLastError()),"Cannot protect a Windows staging entry");continue;}
        const auto info=Info(file.value);
        CheckStagedEntry(log,path,info);
        if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)ClearStagedEntries(log,path,depth+1);
        Remove(file.value,"Cannot remove Windows staging entry; remaining contents were retained");
    }
}
size_t RecordStagedTree(HANDLE journal,const std::wstring& folder,const std::wstring& slot,
                        const std::map<std::wstring,MacBrowser::ObjectIdentity>& expected,unsigned depth=0)
{
    Check(depth<=128,"Windows staging tree is too deep");
    size_t recorded=0;
    for(const auto& name:Children(folder)){
        const auto path=Child(folder,name),staged=Child(slot,name);
        Check(Under(staged,slot),"Invalid name in Windows move staging");
        auto file=Open(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
        Check(bool(file),"Cannot protect an entry in the Windows move source");
        const auto info=Info(file.value);
        const auto known=expected.find(path);
        Check(known!=expected.end()&&MacBrowser::WindowsIdentity(info)==known->second,
              "The Windows source folder changed after copying; it was retained");
        ++recorded;
        const bool directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        Data record;
        record.put(uint64_t(Op::TreeEntry));
        record.wide(staged);
        IdentityOf(info).Encode(record);
        record.put(uint64_t(directory));
        if(!directory){
            record.put(Date(info.ftLastWriteTime));
            record.put(FileLength(info));
        }
        // The rename frame flushes the full inventory before the folder moves.
        WriteFrame(journal,record);
        if(directory)recorded+=RecordStagedTree(journal,path,staged,expected,depth+1);
    }
    return recorded;
}
thread_local Session* current=nullptr;
void RecoverJournal(const std::string& id,bool committed,const std::wstring* exportRoot,bool skipBusy);
}

bool Supported(const std::wstring& folder)
{
    auto h=Open(folder,FILE_READ_ATTRIBUTES);
    wchar_t fs[16]{},path[8]{};
    if(!h)return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h.value,&info)||
       (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))!=FILE_ATTRIBUTE_DIRECTORY)return false;
    if(folder.size()<7||folder[5]!=L':'||folder[6]!=L'\\')return false;
    path[0]=folder[4];path[1]=L':';path[2]=L'\\';
    return GetDriveTypeW(path)==DRIVE_FIXED&&
           GetVolumeInformationByHandleW(h.value,nullptr,0,nullptr,nullptr,nullptr,fs,16)&&!_wcsicmp(fs,L"NTFS");
}

struct Session::State {
    std::wstring root,stage;
    std::string decision;
    bool exportOnly;
    Handle journal;
    Handle rootLease;
    Handle stageLease{INVALID_HANDLE_VALUE};
    uint64_t next=0;
    std::set<std::wstring> dates;
    State(const std::wstring& folder,const std::string& id,bool standalone)
     : root(Strip(folder)),stage(Stage(root,id,standalone)),decision(id),exportOnly(standalone),
       journal(CreateFileW(Journal(id,true,standalone).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr)),
       rootLease(Open(root,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE)){
        Check(journal&&rootLease,"Cannot start durable Windows folder move");
        auto info=Info(rootLease.value);
        Check((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&Supported(root),"Durable Windows moves require a local NTFS folder");
        Data header;
        header.put(std::string(Magic));header.put(id);header.wide(root);IdentityOf(info).Encode(header);
        Frame(journal.value,header);
        Check(CreateDirectoryW(stage.c_str(),nullptr)!=FALSE,"Cannot create Windows move staging folder");
        SetFileAttributesW(stage.c_str(),FILE_ATTRIBUTE_HIDDEN);
        stageLease.value=CreateFileW(stage.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        Check(bool(stageLease),"Cannot pin the Windows move staging folder");
        Data recorded;
        recorded.put(uint64_t(Op::Stage));IdentityOf(Info(stageLease.value)).Encode(recorded);
        Frame(journal.value,recorded);
    }
};
Session::Session(const std::wstring& root,const std::string& decision,bool exportOnly)
{
    Check(!current,"Windows move is already active");
    state_=std::make_unique<State>(root,decision,exportOnly);
    current=this;
}
Session::~Session(){if(current==this)current=nullptr;}
Session* Session::Current(){return current;}
std::wstring Session::Slot(){return Child(state_->stage,std::to_wstring(++state_->next));}
std::wstring Session::StageTemporary(const std::wstring& path,HANDLE created)
{
    const auto staged=Slot();
    Paths(state_->root,state_->stage,path,staged);
    Handle directory(created==INVALID_HANDLE_VALUE?Open(path,FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ).Release():INVALID_HANDLE_VALUE);
    const auto file=directory?directory.value:created;
    Check(file!=INVALID_HANDLE_VALUE,"Cannot protect a Windows temporary entry");
    const auto info=Info(file);
    Check(!directory||((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&Children(path).empty()),
          "The temporary Windows folder was replaced or is not empty");
    const auto id=IdentityOf(info);
    Check(id.file&&!Identify(staged).file,"The Windows staging name is occupied");
    Data record;
    record.put(uint64_t(Op::Owned));
    record.wide(staged);
    id.Encode(record);
    try{
        Frame(state_->journal.value,record);
        Move(file,staged);
    }catch(...){
        if(!directory){
            FILE_DISPOSITION_INFO remove{TRUE};
            SetFileInformationByHandle(file,FileDispositionInfo,&remove,sizeof(remove));
        }
        throw;
    }
    return staged;
}
void Session::Rename(const std::wstring& from,const std::wstring& to,
                     const MacBrowser::ObjectIdentity* expected)
{
    RenameChecked(from,to,expected,nullptr);
}
void Session::StageCut(const MacBrowser::WindowsCutPlan& plan)
{
    Check(!plan.empty(),"Cannot stage a Windows cut source without a snapshot");
    const auto& root=plan.front();
    const auto companion=[](const std::wstring& path){
        return Child(Parent(path),L"%"+path.substr(path.find_last_of(L'\\')+1));
    };
    std::map<std::wstring,MacBrowser::ObjectIdentity> tree;
    for(auto i=plan.begin()+1;i!=plan.end();++i){
        Check(Under(i->path,root.path)&&tree.emplace(i->path,i->identity.primary).second,
              "Invalid Windows cut source snapshot");
        if(!i->identity.companion.key.empty())
            Check(tree.emplace(companion(i->path),i->identity.companion).second,"Conflicting Windows cut companion");
    }
    if(!root.identity.companion.key.empty())
        Rename(companion(root.path),Slot(),&root.identity.companion);
    RenameChecked(root.path,Slot(),&root.identity.primary,root.directory?&tree:nullptr);
}
void Session::RenameChecked(const std::wstring& from,const std::wstring& to,
                            const MacBrowser::ObjectIdentity* expected,
                            const std::map<std::wstring,MacBrowser::ObjectIdentity>* tree)
{
    Paths(state_->root,state_->stage,from,to);
    auto handle=Open(from,FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ);
    Check(bool(handle),"Cannot protect Windows move entry");
    const auto info=Info(handle.value);
    Check(!expected||MacBrowser::WindowsIdentity(info)==*expected,
          "A Windows move participant changed before publication; it was retained");
    Check(Under(from,state_->stage)||!(info.dwFileAttributes&FILE_ATTRIBUTE_READONLY),
          "Cannot stage a read-only Windows move entry");
    const auto id=IdentityOf(info);
    const auto fromParent=Identify(Parent(from)),toParent=Identify(Parent(to));
    Check(id.file&&fromParent.file&&toParent.file&&id.serial==fromParent.serial&&
          id.serial==toParent.serial&&!Identify(to).file,"Windows move path changed");
    if((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&Under(to,state_->stage))
        Check(tree&&RecordStagedTree(state_->journal.value,from,to,*tree)==tree->size(),
              "The Windows source folder changed after copying; it was retained");
    Data record;
    record.put(uint64_t(Op::Rename));
    record.wide(from);id.Encode(record);record.wide(to);fromParent.Encode(record);toParent.Encode(record);
    record.put(uint64_t((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0));
    record.put(Date(info.ftLastWriteTime));record.put(FileLength(info));
    Frame(state_->journal.value,record);
    Move(handle.value,to);
    Check(Identify(to)==id,"Windows move did not preserve entry identity");
}
void Session::PreserveDirectoryDates(const std::wstring& path)
{
    if(!Under(path,state_->root)||!state_->dates.insert(path).second)return;
    auto h=Open(path,FILE_READ_ATTRIBUTES);
    Check(bool(h),"Cannot protect Windows folder timestamps");
    const auto info=Info(h.value);
    Data record;
    record.put(uint64_t(Op::Dates));record.wide(path);IdentityOf(info).Encode(record);
    record.put(Date(info.ftCreationTime));record.put(Date(info.ftLastWriteTime));
    Frame(state_->journal.value,record);
}
void Session::Finish()
{
    const auto id=state_->decision;
    const auto root=state_->root;
    const bool exportOnly=state_->exportOnly;
    current=nullptr;
    state_.reset();
    if(exportOnly)RecoverJournal(id,false,&root,false);
    else Recover(id,ClassicHfsRecovery::MoveIsCommitted(id));
}

namespace {
void RecoverJournal(const std::string& id,bool committed,const std::wstring* exportRoot,bool skipBusy)
{
    const bool exportOnly=exportRoot!=nullptr;
    const auto path=Journal(id,false,exportOnly);
    {
        Handle journal(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!journal){
            const auto error=GetLastError();
            if(Missing(error))return;
            if(skipBusy&&error==ERROR_SHARING_VIOLATION)return;
            Check(error==ERROR_SHARING_VIOLATION,"Cannot reopen the Windows move recovery journal");
            throw HfsPlus::Error("The Windows move is still active; try reopening the image after it finishes");
        }
        Info(journal.value);
        const auto log=Load(journal.value,id,exportOnly);
        if(exportOnly){
            Check(!log.root.empty()&&_wcsicmp(log.root.c_str(),exportRoot->c_str())==0,
                  "The Windows export journal belongs to another folder");
            // Read the decision only after taking the journal lock. A worker
            // may still be publishing the decision while another window browses.
            committed=ClassicHfsRecovery::MoveIsCommitted(id);
        }
        if(!log.root.empty()){
            // A live path alone is insufficient after a failed or interrupted move.
            auto root=Open(log.root,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
            Check(root&&IdentityOf(Info(root.value))==log.rootIdentity,"The original Windows move folder is unavailable");
            const auto stage=Stage(log.root,id,exportOnly);
            if(log.stageIdentity.file){
                auto directory=Open(stage,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
                if(!directory)Check(log.done&&Missing(GetLastError()),"Cannot protect the Windows move staging folder");
                else {
                    Check(IdentityOf(Info(directory.value))==log.stageIdentity,
                          "The Windows move staging folder was replaced");
                    CheckStage(stage,log);
                }
                if(!log.done){
                    if(!committed){
                        for(auto i=log.records.rbegin();i!=log.records.rend();++i){
                            if(i->op==Op::Rename)UndoRename(stage,*i);
                        }
                    }
                    LARGE_INTEGER at{};at.QuadPart=LONGLONG(log.end);
                    Check(SetFilePointerEx(journal.value,at,nullptr,FILE_BEGIN)&&SetEndOfFile(journal.value),
                          "Cannot finish the Windows move recovery record");
                    Data done;done.put(uint64_t(Op::Done));Frame(journal.value,done);
                }
                if(!committed){
                    for(const auto& record:log.records){
                        if(record.op!=Op::Dates)continue;
                        auto h=Open(record.from,FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES);
                        Check(h&&IdentityOf(Info(h.value))==record.object,"Windows folder changed during move recovery");
                        auto creation=Time(record.creation),modified=Time(record.modified);
                        Check(SetFileTime(h.value,&creation,nullptr,&modified)!=FALSE,"Cannot restore Windows folder timestamps");
                    }
                }
                if(directory){
                    // Keeping a DELETE handle open while renaming children
                    // blocks NTFS from reopening their destination directory.
                    directory.Close();
                    auto cleanup=Open(stage,FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ);
                    Check(cleanup&&IdentityOf(Info(cleanup.value))==log.stageIdentity,
                          "The Windows move staging folder was replaced before cleanup");
                    ClearStagedEntries(log,stage);
                    Remove(cleanup.value,"Windows move staging is not empty; unrelated files were retained");
                }
            }
        }
        FILE_DISPOSITION_INFO remove{TRUE};
        Check(SetFileInformationByHandle(journal.value,FileDispositionInfo,&remove,sizeof(remove))!=FALSE,
              "Cannot clear the Windows move journal");
    }
    if(exportOnly)ClassicHfsRecovery::ForgetMoveDecision(id);
}
}

void Recover(const std::string& id,bool committed)
{
    RecoverJournal(id,committed,nullptr,false);
}

void RecoverExports(const std::wstring& folder)
{
    const auto root=Strip(folder);
    WIN32_FIND_DATAW data{};
    const auto search=FindFirstFileW(Child(root,std::wstring(ExportPrefix)+L"*").c_str(),&data);
    if(search==INVALID_HANDLE_VALUE)return;
    struct Search {HANDLE handle;~Search(){FindClose(handle);}} guard{search};
    do{
        std::wstring name=data.cFileName;
        if((data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))!=FILE_ATTRIBUTE_DIRECTORY)
            continue;
        name.erase(0,ARRAYSIZE(ExportPrefix)-1);
        if(name.size()!=ClassicHfsRecovery::MoveIdSize||name.find_first_not_of(L"0123456789abcdef")!=name.npos)continue;
        std::string id;
        for(const auto digit:name)id.push_back(char(digit));
        RecoverJournal(id,false,&root,true);
    }while(FindNextFileW(search,&data));
    Check(GetLastError()==ERROR_NO_MORE_FILES,"Cannot enumerate Windows export recovery");
}

void RunExport(const std::wstring& folder,const std::function<void()>& work)
{
    RecoverExports(folder);
    ClassicHfsRecovery::MoveDecision decision;
    Session transaction(folder,decision.Id(),true);
    std::exception_ptr failure;
    bool deciding=false;
    try{
        work();
        deciding=true;
        decision.Commit();
    }catch(...){failure=std::current_exception();}
    try{transaction.Finish();}
    catch(...){Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Unverified);throw;}
    if(failure){
        if(!deciding)Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Restored);
        std::rethrow_exception(failure);
    }
}
}
