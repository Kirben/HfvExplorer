/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stdafx.h"
#include "ClassicHfsRecovery.h"
#include "WindowsMoveUndo.h"
#include "HfsPlus.h"
#include <io.h>
#include <shlobj.h>
#include <objbase.h>
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"
#include <unordered_set>

namespace {
using ClassicHfsRecovery::Transaction;
using ClassicHfsRecovery::ImageRange;
constexpr wchar_t Stream[] = L":HFVExplorer.ClassicUndo";
constexpr wchar_t NdifStream[] = L":HFVExplorer.NdifClassicUndo";
constexpr wchar_t Sibling[] = L".hfv-classic-undo";
constexpr wchar_t NdifSibling[] = L".hfv-ndif-classic-undo";
constexpr uint64_t Committed = UINT64_MAX;
constexpr uint64_t Sector = 512;
constexpr char Magic[8] = {'H','F','V','H','F','S','U','1'};
constexpr char MoveMagic[8] = {'H','F','V','H','F','S','U','2'};
using ClassicHfsRecovery::MoveIdSize;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};

struct Header {
    char magic[8];
    uint64_t size;
    DWORD serial, fileHigh, fileLow;
    FILETIME created;
    uint32_t crc;
};
struct NdifRegion {
    uint64_t offset,length;
    uint32_t reserved;
    uint32_t crc;
};
struct Record {
    uint64_t offset;
    BYTE bytes[Sector];
    uint32_t reserved;
    uint32_t crc;
};
static_assert(sizeof(Header)==40 && sizeof(NdifRegion)==24 && sizeof(Record)==528);

uint32_t Crc(const void* bytes, unsigned length)
{
    return crc32(0,static_cast<const Bytef*>(bytes),length);
}
void Check(bool ok,const char* message)
{
    if (!ok) throw HfsPlus::Error(message);
}
HANDLE ImageHandle(int fd)
{
    return fd >= 0 && fd < 0x10000000
        ? reinterpret_cast<HANDLE>(_get_osfhandle(fd)) : INVALID_HANDLE_VALUE;
}
struct JournalPath {
    std::wstring name;
    bool sibling=false;

    DWORD OpenFlags() const {
        return sibling?FILE_FLAG_OPEN_REPARSE_POINT:0;
    }
};

bool OrdinaryImage(HANDLE image)
{
    return image!=INVALID_HANDLE_VALUE && GetFileType(image)==FILE_TYPE_DISK;
}
JournalPath LocateJournal(HANDLE image,bool ndif)
{
    if (!OrdinaryImage(image)) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(image,&info)!=FALSE,"Cannot identify the classic HFS recovery image");
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)return {};
    const auto required = GetFinalPathNameByHandleW(image,nullptr,0,FILE_NAME_OPENED);
    Check(required!=0,"Cannot locate the classic HFS recovery journal");
    std::wstring path(required,0);
    const auto used = GetFinalPathNameByHandleW(image,path.data(),required,FILE_NAME_OPENED);
    Check(used && used<required,"Cannot locate the classic HFS recovery journal");
    path.resize(used);
    const auto leaf=path.find_last_of(L"\\/");
    if(leaf==path.npos||path.find(L':',leaf+1)!=path.npos)return {};

    DWORD flags=0;
    Check(GetVolumeInformationByHandleW(image,nullptr,0,nullptr,nullptr,&flags,nullptr,0)!=FALSE,
          "Cannot determine how to store the classic HFS recovery journal");
    bool sibling=(flags&FILE_NAMED_STREAMS)==0;
#ifdef HFV_TESTING
    if(GetEnvironmentVariableW(L"HFV_TEST_CLASSIC_SIBLING_UNDO",nullptr,0))sibling=true;
#endif
    path+=sibling?(ndif?NdifSibling:Sibling):(ndif?NdifStream:Stream);
    return {std::move(path),sibling};
}
uint64_t Length(HANDLE file)
{
    LARGE_INTEGER size{};
    Check(GetFileSizeEx(file,&size)!=FALSE && size.QuadPart>=0,"Cannot read classic HFS recovery file length");
    return uint64_t(size.QuadPart);
}
Header Identify(HANDLE image)
{
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(image,&info)!=FALSE,"Cannot identify the classic HFS image for recovery");
    Header result{};
    memcpy(result.magic,Magic,sizeof(Magic));
    result.size=Length(image);
    result.serial=info.dwVolumeSerialNumber;
    result.fileHigh=info.nFileIndexHigh;
    result.fileLow=info.nFileIndexLow;
    result.created=info.ftCreationTime;
    result.crc=Crc(&result,offsetof(Header,crc));
    return result;
}
bool Seek(HANDLE file,uint64_t offset)
{
    LARGE_INTEGER position{};
    position.QuadPart=LONGLONG(offset);
    return SetFilePointerEx(file,position,nullptr,FILE_BEGIN)!=FALSE;
}
void ReadAt(HANDLE file,uint64_t offset,void* bytes,DWORD length)
{
    DWORD count=0;
    Check(Seek(file,offset)&&ReadFile(file,bytes,length,&count,nullptr)&&count==length,
          "Cannot read classic HFS recovery data");
}
void WriteAt(HANDLE file,uint64_t offset,const void* bytes,DWORD length)
{
    DWORD count=0;
    Check(Seek(file,offset)&&WriteFile(file,bytes,length,&count,nullptr)&&count==length,
          "Cannot write classic HFS recovery data");
}
bool Clear(HANDLE stream)
{
    return Seek(stream,0)&&SetEndOfFile(stream)&&FlushFileBuffers(stream);
}
void CheckJournalFile(HANDLE stream,const Header& image,bool sibling)
{
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(stream,&info)&&info.dwVolumeSerialNumber==image.serial,
          "The classic HFS recovery journal is on a different volume");
    const bool same=info.nFileIndexHigh==image.fileHigh&&info.nFileIndexLow==image.fileLow;
    if(sibling){
        // Refuse to replay if this host cannot uniquely identify the image
        // that was present when the sibling journal was created.
        Check((image.fileHigh||image.fileLow)&&(image.created.dwHighDateTime||image.created.dwLowDateTime),
              "This host cannot identify a classic HFS image for sibling-file recovery");
        Check(!same&&info.nNumberOfLinks==1&&
              !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),
              "The classic HFS recovery sibling must be a separate ordinary file");
    }else{
        Check(same,"The classic HFS recovery stream belongs to a different image");
    }
}
bool Missing(DWORD error)
{
    return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||
           error==ERROR_INVALID_NAME||error==ERROR_NOT_SUPPORTED;
}
std::wstring DecisionPath(const std::string& id,bool create=false)
{
    Check(id.size()==MoveIdSize&&id.find_first_not_of("0123456789abcdef")==std::string::npos,
          "Invalid classic HFS move decision ID");
    wchar_t root[MAX_PATH]{};
#ifdef HFV_TESTING
    const auto bytes=GetEnvironmentVariableW(L"HFV_TEST_CLASSIC_MOVE_DIR",root,MAX_PATH);
    if(!bytes||bytes>=MAX_PATH)
#endif
        Check(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,root)),
              "Cannot locate the classic HFS move decisions");
    auto folder=std::wstring(root);
    for(const auto* name:{L"HFVExplorer",L"ClassicMoves"}){
        folder+=L"\\";
        folder+=name;
        if(create&&!CreateDirectoryW(folder.c_str(),nullptr)){
            const auto attributes=GetFileAttributesW(folder.c_str());
            Check(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&
                  !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Cannot create classic HFS move decisions folder");
        }
    }
    return folder+L"\\"+std::wstring(id.begin(),id.end())+L".move";
}

bool ReadDecision(HANDLE file,const std::string& id)
{
    Check(file!=INVALID_HANDLE_VALUE,
          "The original classic HFS move decision is unavailable. Keep both images and reopen them on the original PC.");
    const auto size=Length(file);
    Check(size>=id.size()&&size<=id.size()*2,"Invalid classic HFS move decision length");
    char data[MoveIdSize*2]{};
    ReadAt(file,0,data,DWORD(size));
    Check(memcmp(data,id.data(),id.size())==0&&
          memcmp(data+id.size(),id.data(),size_t(size)-id.size())==0,
          "Classic HFS move decision is damaged");
    return size==id.size()*2;
}

bool MoveCommitted(const std::string& id)
{
    const auto path=DecisionPath(id);
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    return ReadDecision(file.value,id);
}
bool ValidSector(uint64_t offset,const ImageRange& range)
{
    if(offset<range.offset)return false;
    const auto relative=offset-range.offset;
    return relative%Sector==0 && relative<=range.length && range.length-relative>=Sector;
}
ImageRange CheckedRange(HANDLE image,uint64_t size,const ImageRange* ndif)
{
    if(!ndif)return {reinterpret_cast<uintptr_t>(image),0,size};
    Check(ndif->offset<=size&&ndif->length<=size-ndif->offset,
          "Classic HFS recovery range lies outside the NDIF container");
    return *ndif;
}

struct Journal {
    int fd;
    HANDLE image;
    Handle stream;
    Header header;
    ImageRange range;
    const std::string move;
    uint64_t end=sizeof(Header);
    std::unordered_set<uint64_t> recorded;
    Journal* previous=nullptr;
    bool faulted=false;
    bool attached=false;
    bool committing=false;
    Journal(int imageFd,HANDLE imageHandle,const JournalPath& path,const ImageRange* ndif,const std::string& moveId)
        : fd(imageFd),image(imageHandle),
          stream(CreateFileW(path.name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,path.OpenFlags(),nullptr)),
          header(Identify(image)),range(CheckedRange(image,header.size,ndif)),move(moveId)
    {
        Check(bool(stream),"Cannot open the classic HFS recovery journal");
        CheckJournalFile(stream.value,header,path.sibling);
        Check(Length(stream.value)==0,"A classic HFS operation already needs recovery");
        if(!move.empty()){
            Check(!MoveCommitted(move),"Classic HFS move decision has already been committed");
            memcpy(header.magic,MoveMagic,sizeof(MoveMagic));
            header.crc=Crc(&header,offsetof(Header,crc));
        }
        WriteAt(stream.value,0,&header,sizeof(header));
        if(ndif){
            NdifRegion region{range.offset,range.length,0,0};
            region.crc=Crc(&region,offsetof(NdifRegion,crc));
            WriteAt(stream.value,end,&region,sizeof(region));
            end+=sizeof(region);
        }
        if(!move.empty()){
            WriteAt(stream.value,end,move.data(),DWORD(move.size()));
            const auto crc=Crc(move.data(),DWORD(move.size()));
            WriteAt(stream.value,end+move.size(),&crc,sizeof(crc));
            end+=move.size()+sizeof(crc);
        }
        Check(FlushFileBuffers(stream.value)!=FALSE,"Cannot flush the classic HFS recovery header");
    }
    void Append(Record& record)
    {
        record.crc=Crc(&record,offsetof(Record,crc));
        WriteAt(stream.value,end,&record,sizeof(record));
        Check(FlushFileBuffers(stream.value)!=FALSE,"Cannot flush the classic HFS recovery record");
        end+=sizeof(record);
    }
    void Before(uint64_t offset)
    {
        Check(!faulted&&offset<=range.length&&range.length-offset>=Sector&&offset%Sector==0,
              "Classic HFS recovery cannot log this sector");
        offset+=range.offset;
        if (!recorded.insert(offset).second) return;
        Record record{};
        record.offset=offset;
        ReadAt(image,offset,record.bytes,sizeof(record.bytes));
        Append(record);
    }
};

thread_local Journal* active=nullptr;
#ifdef HFV_TESTING
thread_local unsigned crashPhase=0;
#endif
void Crash(unsigned phase)
{
#ifdef HFV_TESTING
    if (crashPhase==phase) ExitProcess(90+phase);
#else
    (void)phase;
#endif
}
Journal* ForFd(int fd)
{
    for (auto* journal=active;journal;journal=journal->previous)
        if (journal->fd==fd) return journal;
    return nullptr;
}
int Recover(HANDLE image,bool writable,const ImageRange* ndif=nullptr,bool keepJournal=false)
{
    const auto path=LocateJournal(image,ndif!=nullptr);
    if (path.name.empty()) return 0;
    Handle stream(CreateFileW(path.name.c_str(),GENERIC_READ|(writable?GENERIC_WRITE:0),
                              writable?0:(FILE_SHARE_READ|FILE_SHARE_WRITE),nullptr,OPEN_EXISTING,path.OpenFlags(),nullptr));
    if (!stream && (path.sibling?GetLastError()==ERROR_FILE_NOT_FOUND:Missing(GetLastError()))) return 0;
    Check(bool(stream),"Cannot open pending classic HFS recovery journal");
    auto identity=Identify(image);
    CheckJournalFile(stream.value,identity,path.sibling);
    const auto size=Length(stream.value);
    if (!size) return 0;
    auto range=CheckedRange(image,identity.size,ndif);
    Header header{};
    uint64_t prefix=sizeof(header)+(ndif?sizeof(NdifRegion):0);
    bool shared=false;
    if (size>=sizeof(header)) {
        ReadAt(stream.value,0,&header,sizeof(header));
        shared=memcmp(header.magic,MoveMagic,sizeof(MoveMagic))==0;
        if(shared){
            memcpy(identity.magic,MoveMagic,sizeof(MoveMagic));
            identity.crc=Crc(&identity,offsetof(Header,crc));
        }
        Check(memcmp(&identity,&header,sizeof(header))==0,"Classic HFS recovery header or image identity is invalid");
    }else if(path.sibling){
        // An interrupted initial header can only be discarded if its
        // written prefix belongs to this image.
        ReadAt(stream.value,0,&header,DWORD(size));
        if(size>=sizeof(MoveMagic)&&memcmp(header.magic,MoveMagic,sizeof(MoveMagic))==0){
            memcpy(identity.magic,MoveMagic,sizeof(MoveMagic));
            identity.crc=Crc(&identity,offsetof(Header,crc));
        }
        Check(memcmp(&identity,&header,size_t(size))==0,"Invalid classic HFS recovery sibling header");
    }
    if (size<prefix) {
        // No image write can occur while the initial header is incomplete.
        if (writable&&!keepJournal) Check(Clear(stream.value),"Cannot clear an incomplete classic HFS recovery header");
        return 0;
    }
    if(ndif){
        NdifRegion region{};
        ReadAt(stream.value,sizeof(header),&region,sizeof(region));
        Check(region.crc==Crc(&region,offsetof(NdifRegion,crc))&&
              region.offset>=range.offset&&(region.offset-range.offset)%Sector==0&&
              region.offset-range.offset<=range.length&&
              region.length<=range.length-(region.offset-range.offset),
              "Classic HFS recovery partition lies outside the stored NDIF payload");
        range.offset=region.offset;
        range.length=region.length;
    }
    bool sharedCommit=false;
    if(shared){
        if(size-prefix<MoveIdSize+sizeof(uint32_t)){
            if(writable&&!keepJournal)Check(Clear(stream.value),"Cannot clear incomplete classic HFS move header");
            return 0;
        }
        std::string move(MoveIdSize,0);
        uint32_t crc=0;
        ReadAt(stream.value,prefix,move.data(),DWORD(move.size()));
        ReadAt(stream.value,prefix+move.size(),&crc,sizeof(crc));
        Check(crc==Crc(move.data(),DWORD(move.size())),"Classic HFS move header is damaged");
        sharedCommit=MoveCommitted(move);
        if(writable&&!keepJournal)WindowsMoveUndo::Recover(move,sharedCommit);
        prefix+=move.size()+sizeof(crc);
    }
    const auto count=(size-prefix)/sizeof(Record);
    Check(count<=range.length/Sector+1,"Classic HFS recovery contains too many sector records");
    bool committed=sharedCommit;
    for (uint64_t i=0;i<count;++i) {
        Record record{};
        ReadAt(stream.value,prefix+i*sizeof(record),&record,sizeof(record));
        Check(record.crc==Crc(&record,offsetof(Record,crc)),"Classic HFS recovery record is damaged");
        if (record.offset==Committed) {
            Check((!shared||sharedCommit)&&size==prefix+(i+1)*sizeof(record),
                  "Invalid classic HFS recovery commit record");
            committed=true;
        } else {
            Check(ValidSector(record.offset,range),"Classic HFS recovery sector lies outside the image");
        }
    }
    if (committed && !writable) return 0;
    Check(writable!=0,"Classic HFS recovery requires writable access. Close other views and reopen the image.");
    if (!committed) {
        // A partial tail never preceded an image write. Validate the complete
        // records first, then replay; a second interruption can replay again.
        for (uint64_t i=count;i>0;--i) {
            Record record{};
            ReadAt(stream.value,prefix+(i-1)*sizeof(record),&record,sizeof(record));
            WriteAt(image,record.offset,record.bytes,sizeof(record.bytes));
#ifdef HFV_TESTING
            if (crashPhase==5&&FlushFileBuffers(image)) Crash(5);
#endif
        }
        Check(FlushFileBuffers(image)!=FALSE,"Cannot flush recovered classic HFS sectors");
    }
    if(!keepJournal)Check(Clear(stream.value),"Cannot clear the classic HFS recovery stream");
    return 1;
}
}

struct Transaction::State : Journal { using Journal::Journal; };
bool Transaction::Supported(int fd,const ImageRange* ndif) {
    return OrdinaryImage(ndif?reinterpret_cast<HANDLE>(ndif->handle):ImageHandle(fd));
}
Transaction::Transaction(int fd,const ImageRange* ndif,const std::string& move)
{
    const auto image=ndif?reinterpret_cast<HANDLE>(ndif->handle):ImageHandle(fd);
    const auto path=LocateJournal(image,ndif!=nullptr);
    if (path.name.empty()) return;
    Check(!ForFd(fd),"A classic HFS recovery transaction is already active");
    state_=std::make_unique<State>(fd,image,path,ndif,move);
    state_->previous=active;
    state_->attached=true;
    active=state_.get();
}
Transaction::~Transaction() { Detach(); }
bool Transaction::Active() const { return bool(state_); }
bool Transaction::CommitStarted() const { return state_ && state_->committing; }
bool Transaction::PublishedMove() const {
    return state_&&!state_->move.empty()&&MoveCommitted(state_->move);
}
void Transaction::Detach()
{
    if (!state_) return;
    if (state_->attached) active=state_->previous;
    state_.reset();
}
void Transaction::Prepare()
{
    Check(state_&&!state_->faulted&&FlushFileBuffers(state_->image),"Cannot flush the classic HFS transaction");
}
void Transaction::Commit()
{
    if (!state_) return;
    Prepare();
    if(!state_->move.empty()){
        Check(MoveCommitted(state_->move),"The classic HFS move decision has not committed");
        // Keep the HFS journal discoverable if Windows cleanup still needs a retry.
        WindowsMoveUndo::Recover(state_->move,true);
    }
    Crash(2);
    Record record{};
    record.offset=Committed;
    state_->committing=true;
    state_->Append(record);
    Crash(3);
    const bool cleared=Clear(state_->stream.value);
    if(!state_->move.empty())Check(cleared,"Cannot clear the committed classic HFS move journal");
    Detach();
}
ClassicHfsRecovery::MoveDecision::MoveDecision()
{
    GUID guid{};
    Check(SUCCEEDED(CoCreateGuid(&guid)),"Cannot allocate a classic HFS move decision ID");
    constexpr char hex[]="0123456789abcdef";
    for(size_t at=0;at<sizeof(guid);++at){
        const auto byte=reinterpret_cast<const unsigned char*>(&guid)[at];
        id_+=hex[byte>>4];
        id_+=hex[byte&15];
    }
    const auto path=DecisionPath(id_,true);
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(bool(file),"Cannot create the classic HFS move decision");
    WriteAt(file.value,0,id_.data(),DWORD(id_.size()));
    Check(FlushFileBuffers(file.value)!=FALSE,"Cannot flush the pending classic HFS move decision");
}
void ClassicHfsRecovery::MoveDecision::Commit()
{
    const auto path=DecisionPath(id_);
    Handle file(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    Check(!ReadDecision(file.value,id_),"Classic HFS move decision has already committed");
    Crash(8);
    WriteAt(file.value,id_.size(),id_.data(),DWORD(id_.size()));
    Check(FlushFileBuffers(file.value)!=FALSE,"Cannot flush the classic HFS move decision");
    Crash(9);
}
void ClassicHfsRecovery::MoveDecision::Forget() noexcept
{
    ForgetMoveDecision(id_);
}
void ClassicHfsRecovery::ForgetMoveDecision(const std::string& id) noexcept
{
    try{DeleteFileW(DecisionPath(id).c_str());}catch(...){}
}
std::wstring ClassicHfsRecovery::MoveDecisionPath(const std::string& id,bool create)
{
    return DecisionPath(id,create);
}
bool ClassicHfsRecovery::MoveIsCommitted(const std::string& id)
{
    return MoveCommitted(id);
}
bool ClassicHfsRecovery::Pending(const std::wstring& path,bool ndif)
{
    Handle image(CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,0,nullptr));
    if(!image)return false;
    const auto journal=LocateJournal(image.value,ndif);
    if(journal.name.empty())return false;
    Handle stream(CreateFileW(journal.name.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                              nullptr,OPEN_EXISTING,journal.OpenFlags(),nullptr));
    if(!stream){
        // A busy journal still needs to be resolved by the owning image mount.
        if(journal.sibling)return GetFileAttributesW(journal.name.c_str())!=INVALID_FILE_ATTRIBUTES;
        return false;
    }
    return Length(stream.value)!=0;
}
bool ClassicHfsRecovery::RecoverNdif(const ImageRange& range,bool writable,bool keepJournal)
{
    return Recover(reinterpret_cast<HANDLE>(range.handle),writable,&range,keepJournal)>0;
}
#ifdef HFV_TESTING
void Transaction::CrashAt(unsigned phase) { crashPhase=phase; }
extern "C" void hfv_hfs_recovery_checkpoint(unsigned phase) { if(active) Crash(phase); }
#endif
extern "C" int hfv_hfs_recover_image(int fd,int writable)
{
    try { return Recover(ImageHandle(fd),writable); }
    catch (...) { errno=EIO;return -1; }
}
extern "C" int hfv_hfs_before_sector_write(int fd,unsigned long long offset)
{
    auto* journal=ForFd(fd);
    if (!journal) return 0;
    try { journal->Before(offset);return 0; }
    catch (...) { journal->faulted=true;errno=EIO;return -1; }
}
extern "C" void hfv_hfs_after_sector_write(int fd)
{
#ifdef HFV_TESTING
    if (crashPhase==1)
        if (auto* journal=ForFd(fd);journal&&FlushFileBuffers(journal->image)) Crash(1);
#else
    (void)fd;
#endif
}
