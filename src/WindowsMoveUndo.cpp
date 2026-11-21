// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "WindowsMoveUndo.h"
#include "ClassicHfsRecovery.h"
#include "CopyWorkerProtocol.h"
#include "StorageWorker.h"
#include <set>
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"

extern int DeleteHostMoveStaging(const std::wstring&);
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
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
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
std::wstring Stage(const std::wstring& root,const std::string& id)
{
    return Child(root,L".hfv-move-"+std::wstring(id.begin(),id.end()));
}
std::wstring Journal(const std::string& id,bool create=false){return ClassicHfsRecovery::MoveDecisionPath(id,create)+L".windows";}
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
void Move(const std::wstring& from,const std::wstring& to)
{
    Check(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot rename a Windows move participant");
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
void Frame(HANDLE handle,const Data& data)
{
    const uint32_t header[]={uint32_t(data.bytes.size()),Crc(data.bytes)};
    Write(handle,header,sizeof(header));
    Write(handle,data.bytes.data(),header[0]);
    Flush(handle);
}
enum class Op:uint64_t {Stage=1,Rename,Dates,Done};
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
struct Loaded {
    std::wstring root;
    Identity rootIdentity,stageIdentity;
    std::vector<Record> records;
    bool done=false;
    uint64_t end=0;
};
Loaded Load(HANDLE handle,const std::string& id)
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
            const auto stage=Stage(log.root,id);
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
    auto file=Open(record.to,FILE_READ_ATTRIBUTES|(published&&record.directory?DELETE:0),
                   FILE_SHARE_READ|FILE_SHARE_DELETE);
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
        FILE_DISPOSITION_INFO remove{TRUE};
        Check(SetFileInformationByHandle(file.value,FileDispositionInfo,&remove,sizeof(remove))!=FALSE,
              "Windows move recovery could not remove a new folder; its contents were retained");
    }else{
        Check(!published||(Date(info.ftLastWriteTime)==record.modified&&FileLength(info)==record.length),
              "A Windows move output was edited before recovery; both versions were retained");
        Move(record.to,record.from);
    }
}
thread_local Session* current=nullptr;
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
    Handle journal;
    Handle rootLease;
    uint64_t next=0;
    std::set<std::wstring> dates;
    State(const std::wstring& folder,const std::string& id)
     : root(Strip(folder)),stage(Stage(root,id)),decision(id),
       journal(CreateFileW(Journal(id,true).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr)),
       rootLease(Open(root,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE)){
        Check(journal&&rootLease,"Cannot start durable Windows folder move");
        auto info=Info(rootLease.value);
        Check((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&Supported(root),"Durable Windows moves require a local NTFS folder");
        Data header;
        header.put(std::string(Magic));header.put(id);header.wide(root);IdentityOf(info).Encode(header);
        Frame(journal.value,header);
        Check(CreateDirectoryW(stage.c_str(),nullptr)!=FALSE,"Cannot create Windows move staging folder");
        SetFileAttributesW(stage.c_str(),FILE_ATTRIBUTE_HIDDEN);
        Data recorded;
        recorded.put(uint64_t(Op::Stage));Identify(stage).Encode(recorded);
        Frame(journal.value,recorded);
    }
};
Session::Session(const std::wstring& root,const std::string& decision)
{
    Check(!current,"Windows move is already active");
    state_=std::make_unique<State>(root,decision);
    current=this;
}
Session::~Session(){if(current==this)current=nullptr;}
Session* Session::Current(){return current;}
std::wstring Session::Slot(){return Child(state_->stage,std::to_wstring(++state_->next));}
void Session::Rename(const std::wstring& from,const std::wstring& to)
{
    Paths(state_->root,state_->stage,from,to);
    auto handle=Open(from,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE);
    Check(bool(handle),"Cannot protect Windows move entry");
    const auto info=Info(handle.value);
    Check(Under(from,state_->stage)||!(info.dwFileAttributes&FILE_ATTRIBUTE_READONLY),
          "Cannot stage a read-only Windows move entry");
    const auto id=IdentityOf(info);
    const auto fromParent=Identify(Parent(from)),toParent=Identify(Parent(to));
    Check(id.file&&fromParent.file&&toParent.file&&id.serial==fromParent.serial&&
          id.serial==toParent.serial&&!Identify(to).file,"Windows move path changed");
    Data record;
    record.put(uint64_t(Op::Rename));
    record.wide(from);id.Encode(record);record.wide(to);fromParent.Encode(record);toParent.Encode(record);
    record.put(uint64_t((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0));
    record.put(Date(info.ftLastWriteTime));record.put(FileLength(info));
    Frame(state_->journal.value,record);
    Move(from,to);
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
    current=nullptr;
    state_.reset();
    Recover(id,ClassicHfsRecovery::MoveIsCommitted(id));
}

void Recover(const std::string& id,bool committed)
{
    const auto path=Journal(id);
    {
        Handle journal(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!journal){
            const auto error=GetLastError();
            if(Missing(error))return;
            Check(error==ERROR_SHARING_VIOLATION,"Cannot reopen the Windows move recovery journal");
            throw HfsPlus::Error("The Windows move is still active; try reopening the image after it finishes");
        }
        Info(journal.value);
        const auto log=Load(journal.value,id);
        if(!log.root.empty()){
            // A live path alone is insufficient after a failed or interrupted move.
            auto root=Open(log.root,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
            Check(root&&IdentityOf(Info(root.value))==log.rootIdentity,"The original Windows move folder is unavailable");
            const auto stage=Stage(log.root,id);
            const auto stageIdentity=Identify(stage);
            if(log.stageIdentity.file){
                Check((!stageIdentity.file&&log.done)||stageIdentity==log.stageIdentity,
                      "The Windows move staging folder was replaced");
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
                if(stageIdentity.file)
                    Check(DeleteHostMoveStaging(stage)!=0,"Cannot clear the Windows move staging folder");
                if(!committed){
                    for(const auto& record:log.records){
                        if(record.op!=Op::Dates)continue;
                        auto h=Open(record.from,FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES);
                        Check(h&&IdentityOf(Info(h.value))==record.object,"Windows folder changed during move recovery");
                        auto creation=Time(record.creation),modified=Time(record.modified);
                        Check(SetFileTime(h.value,&creation,nullptr,&modified)!=FALSE,"Cannot restore Windows folder timestamps");
                    }
                }
            }
        }
        FILE_DISPOSITION_INFO remove{TRUE};
        Check(SetFileInformationByHandle(journal.value,FileDispositionInfo,&remove,sizeof(remove))!=FALSE,
              "Cannot clear the Windows move journal");
    }
}
}
