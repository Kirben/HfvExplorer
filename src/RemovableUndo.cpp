// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "RemovableUndo.h"
#include "OperationCompletion.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <array>
#include <map>
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"
#pragma comment(lib,"bcrypt.lib")

namespace RemovableUndo {
namespace {
using Bytes=std::vector<BYTE>;
using Digest=std::array<BYTE,32>;
constexpr DWORD Batch=65536;
constexpr uint64_t CommitOffset=UINT64_MAX;
constexpr char Magic[8]={'H','F','V','R','U','N','D','1'};
struct Header {
    char magic[8];
    uint64_t offset,length;
    DWORD sector;
    Digest original;
    DWORD crc;
};
struct Record {uint64_t offset;DWORD count,crc;};
static_assert(sizeof(Header)==64&&sizeof(Record)==16);

void Check(bool passed,const char* message)
{
    if(!passed)throw HfsPlus::Error(message);
}
struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit File(const std::wstring& path,DWORD access,DWORD disposition)
      :handle(CreateFileW(path.c_str(),access,0,nullptr,disposition,FILE_FLAG_OPEN_REPARSE_POINT,nullptr))
    {
        if(handle==INVALID_HANDLE_VALUE)return;
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle,&info)||info.nNumberOfLinks!=1||
           (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){
            CloseHandle(handle);
            handle=INVALID_HANDLE_VALUE;
            SetLastError(ERROR_INVALID_DATA);
        }
    }
    ~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
struct Buffer {
    BYTE* value=static_cast<BYTE*>(VirtualAlloc(nullptr,Batch,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Buffer(){Check(value!=nullptr,"Cannot allocate an aligned recovery buffer");}
    ~Buffer(){VirtualFree(value,0,MEM_RELEASE);}
};
class Hash {
    BCRYPT_ALG_HANDLE algorithm_=nullptr;
    BCRYPT_HASH_HANDLE hash_=nullptr;
    Bytes work_;
public:
    Hash(){
        Check(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,
              "Cannot initialize removable-media hashing");
        try{
            DWORD size=0,got=0;
            Check(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<BYTE*>(&size),sizeof(size),&got,0)>=0,
                  "Cannot size removable-media hashing workspace");
            work_.resize(size);
            Check(BCryptCreateHash(algorithm_,&hash_,work_.data(),size,nullptr,0,0)>=0,
                  "Cannot start removable-media hashing");
        }catch(...){BCryptCloseAlgorithmProvider(algorithm_,0);throw;}
    }
    ~Hash(){BCryptDestroyHash(hash_);BCryptCloseAlgorithmProvider(algorithm_,0);}
    void Add(const void* data,DWORD size){
        Check(BCryptHashData(hash_,const_cast<BYTE*>(static_cast<const BYTE*>(data)),size,0)>=0,
              "Cannot hash removable-media data");
    }
    Digest Finish(){
        Digest result{};
        Check(BCryptFinishHash(hash_,result.data(),DWORD(result.size()),0)>=0,
              "Cannot finish removable-media hashing");
        return result;
    }
};
void MakeDirectory(const std::wstring& path)
{
    if(CreateDirectoryW(path.c_str(),nullptr))return;
    const auto attr=GetFileAttributesW(path.c_str());
    Check(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY)&&!(attr&FILE_ATTRIBUTE_REPARSE_POINT),
          "Cannot create the local removable-media recovery folder");
}
std::wstring JournalPath(const std::wstring& device,bool create)
{
    wchar_t base[MAX_PATH]{};
    std::wstring folder;
#ifdef HFV_TESTING
    const auto overrideSize=GetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_UNDO_DIR",base,MAX_PATH);
    if(overrideSize&&overrideSize<MAX_PATH)folder=base;
    else {
        Check(GetTempPathW(MAX_PATH,base)>0,"Cannot find test recovery folder");
        folder=std::wstring(base)+L"HFVExplorer-Undo-Test-"+std::to_wstring(GetCurrentProcessId());
    }
#else
    Check(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,base)),
          "Cannot find the local removable-media recovery folder");
    folder=std::wstring(base)+L"\\HFVExplorer";
#endif
    if(create)MakeDirectory(folder);
    folder+=L"\\RemovableUndo";
    if(create)MakeDirectory(folder);
    auto key=device;
    if(!key.empty())CharUpperBuffW(key.data(),DWORD(key.size()));
    Hash hash;
    hash.Add(key.data(),DWORD(key.size()*sizeof(wchar_t)));
    constexpr wchar_t hex[]=L"0123456789abcdef";
    folder+=L"\\";
    for(auto byte:hash.Finish()){folder+=hex[byte>>4];folder+=hex[byte&15];}
    return folder+L".undo";
}
void Seek(HANDLE file,uint64_t offset)
{
    LARGE_INTEGER at{};
    at.QuadPart=LONGLONG(offset);
    Check(SetFilePointerEx(file,at,nullptr,FILE_BEGIN)!=FALSE,"Cannot seek in the removable-media undo log");
}
uint64_t Length(HANDLE file)
{
    LARGE_INTEGER size{};
    Check(GetFileSizeEx(file,&size)!=FALSE&&size.QuadPart>=0,"Cannot read the removable-media undo log size");
    return uint64_t(size.QuadPart);
}
void Load(HANDLE file,uint64_t offset,void* data,DWORD size)
{
    Seek(file,offset);
    DWORD done=0;
    Check(ReadFile(file,data,size,&done,nullptr)&&done==size,"Cannot read the removable-media undo log");
}
void Append(HANDLE file,const void* data,DWORD size)
{
    DWORD done=0;
    Check(WriteFile(file,data,size,&done,nullptr)&&done==size,"Cannot write the removable-media undo log");
}
void Flush(HANDLE file)
{
    Check(FlushFileBuffers(file)!=FALSE,"Cannot flush the removable-media undo log");
}
void Clear(HANDLE file)
{
    Seek(file,0);
    Check(SetEndOfFile(file)!=FALSE,"Cannot clear the removable-media undo log");
    Flush(file);
}
bool ValidRange(const Range& range)
{
    return range.sector>=512&&range.sector<=4096&&!(range.sector&(range.sector-1))&&
           range.length&&range.offset%range.sector==0&&range.length%range.sector==0&&
           range.offset<=INT64_MAX&&range.length<=INT64_MAX-range.offset;
}
bool ValidWrite(const Range& range,uint64_t offset,DWORD count)
{
    return count&&count<=Batch&&count%range.sector==0&&offset>=range.offset&&
           (offset-range.offset)%range.sector==0&&offset-range.offset<=range.length&&
           count<=range.length-(offset-range.offset);
}
DWORD Crc(const void* data,DWORD size,DWORD crc=0)
{
    return crc32(crc,static_cast<const Bytef*>(data),size);
}
Header LoadHeader(HANDLE file)
{
    Header header{};
    Load(file,0,&header,sizeof(header));
    Check(memcmp(header.magic,Magic,sizeof(Magic))==0&&
          header.crc==Crc(&header,offsetof(Header,crc))&&
          ValidRange({header.offset,header.length,header.sector}),"Invalid removable-media undo header");
    return header;
}
bool Absent(DWORD error){return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;}
template<class Visit> Digest Fingerprint(const Range& range,const Read& read,Visit visit)
{
    Hash hash;
    Buffer bytes;
    for(uint64_t at=0;at<range.length;){
        const auto count=DWORD((std::min)(uint64_t(Batch),range.length-at));
        read(range.offset+at,count,bytes.value);
        visit(range.offset+at,count,bytes.value);
        hash.Add(bytes.value,count);
        at+=count;
    }
    return hash.Finish();
}
}

bool Enabled()
{
#ifdef HFV_TESTING
    return GetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_UNDO_DIR",nullptr,0)!=0;
#else
    return true;
#endif
}

std::wstring FloppyKey(int drive){return L"floppy-"+std::to_wstring(drive);}

bool Pending(const std::wstring& device)
{
    if(!Enabled())return false;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if(!GetFileAttributesExW(JournalPath(device,false).c_str(),GetFileExInfoStandard,&info)){
        Check(Absent(GetLastError()),"Cannot check for pending removable-media recovery");
        return false;
    }
    Check(!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),
          "Invalid removable-media recovery file");
    return info.nFileSizeLow||info.nFileSizeHigh;
}

Range PendingRange(const std::wstring& device)
{
    File file(JournalPath(device,false),GENERIC_READ,OPEN_EXISTING);
    Check(file.handle!=INVALID_HANDLE_VALUE,"Cannot open the pending removable-media undo log");
    if(Length(file.handle)<sizeof(Header))return {};
    const auto h=LoadHeader(file.handle);
    return {h.offset,h.length,h.sector};
}

struct Log::State {
    File file;
    Range range;
    uint64_t end=sizeof(Header);
    State(const std::wstring& key,const Range& bounds,const Read& read):
        file(JournalPath(key,true),GENERIC_READ|GENERIC_WRITE,OPEN_ALWAYS),range(bounds){
        Check(file.handle!=INVALID_HANDLE_VALUE,"Cannot open the local removable-media undo log");
        Check(Length(file.handle)==0,"Removable-media recovery is pending");
        Check(ValidRange(range),"Invalid removable-media recovery range");
        Header header{};
        memcpy(header.magic,Magic,sizeof(Magic));
        header.offset=range.offset;
        header.length=range.length;
        header.sector=range.sector;
        header.original=Fingerprint(range,read,[](uint64_t,DWORD,BYTE*){});
        header.crc=Crc(&header,offsetof(Header,crc));
        Append(file.handle,&header,sizeof(header));
        Flush(file.handle);
    }
    void RecordWrite(uint64_t offset,DWORD count,const void* before,const void* after){
        Record entry{offset,count,Crc(&offset,sizeof(offset))};
        entry.crc=Crc(&count,sizeof(count),entry.crc);
        if(count){entry.crc=Crc(before,count,entry.crc);entry.crc=Crc(after,count,entry.crc);}
        Seek(file.handle,end);
        Append(file.handle,&entry,sizeof(entry));
        if(count){Append(file.handle,before,count);Append(file.handle,after,count);}
        Flush(file.handle);
        end+=sizeof(entry)+uint64_t(count)*2;
    }
};

Log::Log(const std::wstring& device,const Range& range,const Read& read)
    :state_(std::make_unique<State>(device,range,read)){}
Log::~Log()=default;
void Log::BeforeWrite(uint64_t offset,DWORD count,const void* before,const void* after)
{
    Check(ValidWrite(state_->range,offset,count),"Write outside the removable-media recovery range");
    state_->RecordWrite(offset,count,before,after);
}
void Log::Commit()
{
#ifdef HFV_TESTING
    TestCheckpoint(4);
#endif
    state_->RecordWrite(CommitOffset,0,nullptr,nullptr);
#ifdef HFV_TESTING
    TestCheckpoint(2);
#endif
    // The durable commit record is sufficient if clearing the log fails.
    try{Clear(state_->file.handle);}catch(const HfsPlus::Error&){}
}

void Recover(const std::wstring& device,const Range& range,const Read& read,const Restore& restore)
{
    File file(JournalPath(device,false),GENERIC_READ|GENERIC_WRITE,OPEN_EXISTING);
    if(file.handle==INVALID_HANDLE_VALUE){
        Check(Absent(GetLastError()),"Cannot lock the pending removable-media undo log");
        return;
    }
    const auto size=Length(file.handle);
    if(!size)return;
    if(size<sizeof(Header)){
        Header prefix{};
        Load(file.handle,0,&prefix,DWORD(size));
        Check(memcmp(prefix.magic,Magic,(std::min)(size,uint64_t(sizeof(Magic))))==0,
              "Invalid partial removable-media undo header");
        Clear(file.handle);
        return;
    }
    const auto header=LoadHeader(file.handle);
    Check(range.offset==header.offset&&range.length==header.length&&range.sector==header.sector,
          "Insert the original removable media before recovery");

    struct Patch {uint64_t first, before, after; bool restore=false;};
    std::map<uint64_t,Patch> sectors;
    Bytes payload(Batch*2);
    for(uint64_t at=sizeof(header);size-at>=sizeof(Record);){
        Record record{};
        Load(file.handle,at,&record,sizeof(record));
        if(record.offset==CommitOffset){
            Check(!record.count&&size-at==sizeof(record)&&
                  record.crc==Crc(&record,offsetof(Record,crc)),"Invalid removable-media commit marker");
            Clear(file.handle);
            return;
        }
        Check(ValidWrite(range,record.offset,record.count),"Invalid removable-media undo extent");
        const uint64_t data=at+sizeof(record),length=uint64_t(record.count)*2;
        if(size-data<length)break;
        Load(file.handle,data,payload.data(),DWORD(length));
        Check(record.crc==Crc(payload.data(),DWORD(length),Crc(&record,offsetof(Record,crc))),
              "Damaged removable-media undo record");
        for(DWORD skip=0;skip<record.count;skip+=range.sector){
            const auto before=data+skip,after=before+record.count;
            auto& patch=sectors.try_emplace(record.offset+skip,Patch{before,before,after}).first->second;
            patch.before=before;
            patch.after=after;
        }
        at=data+length;
    }
    if(sectors.empty()){Clear(file.handle);return;}
    Bytes first(range.sector),previous(range.sector),next(range.sector);
    const auto current=Fingerprint(range,read,[&](uint64_t at,DWORD count,BYTE* bytes){
        for(auto iter=sectors.lower_bound(at);iter!=sectors.end()&&iter->first-at<count;++iter){
            auto& patch=iter->second;
            auto sector=bytes+(iter->first-at);
            Load(file.handle,patch.first,first.data(),range.sector);
            patch.restore=memcmp(sector,first.data(),range.sector)!=0;
            if(patch.restore){
                Load(file.handle,patch.before,previous.data(),range.sector);
                Load(file.handle,patch.after,next.data(),range.sector);
                for(DWORD i=0;i<range.sector;++i)
                    if(sector[i]!=first[i]&&sector[i]!=previous[i]&&sector[i]!=next[i])
                        throw Storage::MediaChanged("This disk does not match the interrupted write. Insert the original media to recover it.");
            }
            memcpy(sector,first.data(),range.sector);
        }
    });
    if(current!=header.original)
        throw Storage::MediaChanged("This disk does not match the interrupted write. Insert the original media to recover it.");
    Buffer aligned;
    for(auto sector=sectors.begin();sector!=sectors.end();){
        if(!sector->second.restore){++sector;continue;}
        const auto offset=sector->first;
        DWORD count=0;
        do{
            Load(file.handle,sector->second.first,aligned.value+count,range.sector);
            ++sector;
            count+=range.sector;
        }while(sector!=sectors.end()&&sector->second.restore&&sector->first==offset+count&&count<Batch);
        restore(offset,count,aligned.value);
#ifdef HFV_TESTING
        TestCheckpoint(3);
#endif
    }
    Clear(file.handle);
}
#ifdef HFV_TESTING
void TestCheckpoint(unsigned phase)
{
    wchar_t value[12]{};
    if(GetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_CRASH",value,12)&&wcstoul(value,nullptr,10)==phase)
        ExitProcess(100+phase);
}
#endif
}
