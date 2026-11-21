/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stdafx.h"
#include "ClassicHfsRecovery.h"
#include "HfsPlus.h"
#include <io.h>
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"
#include <unordered_set>

namespace {
using ClassicHfsRecovery::Overwrite;
constexpr wchar_t Stream[] = L":HFVExplorer.ClassicUndo";
constexpr uint64_t Committed = UINT64_MAX;
constexpr uint64_t Sector = 512;
constexpr char Magic[8] = {'H','F','V','H','F','S','U','1'};

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
struct Record {
    uint64_t offset;
    BYTE bytes[Sector];
    uint32_t reserved;
    uint32_t crc;
};
static_assert(sizeof(Header)==40 && sizeof(Record)==528);

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
std::wstring StreamPath(const std::wstring& path)
{
    const size_t leaf = path.find_last_of(L"\\/");
    if ((leaf == path.npos && path.find(L':') != path.npos) ||
        (leaf != path.npos && path.find(L':',leaf+1) != path.npos))
        return {};
    return path + Stream;
}
bool SupportsRecovery(HANDLE image)
{
    DWORD flags=0;
    return image!=INVALID_HANDLE_VALUE && GetFileType(image)==FILE_TYPE_DISK &&
           GetVolumeInformationByHandleW(image,nullptr,0,nullptr,nullptr,&flags,nullptr,0) &&
           (flags & FILE_NAMED_STREAMS)!=0;
}
std::wstring StreamPath(HANDLE image)
{
    if (image==INVALID_HANDLE_VALUE || GetFileType(image)!=FILE_TYPE_DISK) return {};
    const auto required = GetFinalPathNameByHandleW(image,nullptr,0,FILE_NAME_OPENED);
    Check(required!=0,"Cannot locate the classic HFS image recovery stream");
    std::wstring path(required,0);
    const auto used = GetFinalPathNameByHandleW(image,path.data(),required,FILE_NAME_OPENED);
    Check(used && used<required,"Cannot locate the classic HFS image recovery stream");
    path.resize(used);
    return StreamPath(path);
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
void SameFile(HANDLE stream,const Header& image)
{
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(stream,&info)&&info.dwVolumeSerialNumber==image.serial&&
          info.nFileIndexHigh==image.fileHigh&&info.nFileIndexLow==image.fileLow,
          "The classic HFS recovery stream belongs to a different image");
}
bool Missing(DWORD error)
{
    return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||
           error==ERROR_INVALID_NAME||error==ERROR_NOT_SUPPORTED;
}
bool ValidSector(uint64_t offset,uint64_t length)
{
    return offset%Sector==0 && offset<=length && length-offset>=Sector;
}

struct Journal {
    int fd;
    HANDLE image;
    Handle stream;
    Header header;
    uint64_t end=sizeof(Header);
    std::unordered_set<uint64_t> recorded;
    Journal* previous=nullptr;
    bool faulted=false;
    bool attached=false;
    bool committing=false;
    Journal(int imageFd,HANDLE imageHandle,const std::wstring& path)
        : fd(imageFd),image(imageHandle),
          stream(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr)),
          header(Identify(image))
    {
        Check(bool(stream),"Cannot open the classic HFS overwrite recovery stream");
        SameFile(stream.value,header);
        Check(Length(stream.value)==0,"A classic HFS overwrite already needs recovery");
        WriteAt(stream.value,0,&header,sizeof(header));
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
        Check(!faulted&&ValidSector(offset,header.size),"Classic HFS recovery cannot log this sector");
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
int Recover(HANDLE image,int writable)
{
    const auto path=StreamPath(image);
    if (path.empty()) return 0;
    Handle stream(CreateFileW(path.c_str(),GENERIC_READ|(writable?GENERIC_WRITE:0),
                              writable?0:(FILE_SHARE_READ|FILE_SHARE_WRITE),nullptr,OPEN_EXISTING,0,nullptr));
    if (!stream && Missing(GetLastError())) return 0;
    Check(bool(stream),"Cannot open pending classic HFS recovery stream");
    const auto size=Length(stream.value);
    if (!size) return 0;
    const auto identity=Identify(image);
    SameFile(stream.value,identity);
    Header header{};
    if (size<sizeof(header)) {
        // No image write can occur while the initial header is incomplete.
        if (writable) Check(Clear(stream.value),"Cannot clear an incomplete classic HFS recovery header");
        return 0;
    }
    ReadAt(stream.value,0,&header,sizeof(header));
    Check(memcmp(&identity,&header,sizeof(header))==0,"Classic HFS recovery header or image identity is invalid");
    const auto count=(size-sizeof(header))/sizeof(Record);
    Check(count<=header.size/Sector+1,"Classic HFS recovery contains too many sector records");
    bool committed=false;
    for (uint64_t i=0;i<count;++i) {
        Record record{};
        ReadAt(stream.value,sizeof(header)+i*sizeof(record),&record,sizeof(record));
        Check(record.crc==Crc(&record,offsetof(Record,crc)),"Classic HFS recovery record is damaged");
        if (record.offset==Committed) {
            Check(size==sizeof(header)+(i+1)*sizeof(record),"Invalid classic HFS recovery commit record");
            committed=true;
        } else {
            Check(ValidSector(record.offset,header.size),"Classic HFS recovery sector lies outside the image");
        }
    }
    if (committed && !writable) return 0;
    Check(writable!=0,"Classic HFS overwrite recovery requires writable access. Close other views and reopen the image.");
    if (!committed) {
        // A partial tail never preceded an image write. Validate the complete
        // records first, then replay; a second interruption can replay again.
        for (uint64_t i=count;i>0;--i) {
            Record record{};
            ReadAt(stream.value,sizeof(header)+(i-1)*sizeof(record),&record,sizeof(record));
            WriteAt(image,record.offset,record.bytes,sizeof(record.bytes));
#ifdef HFV_TESTING
            if (crashPhase==5&&FlushFileBuffers(image)) Crash(5);
#endif
        }
        Check(FlushFileBuffers(image)!=FALSE,"Cannot flush recovered classic HFS sectors");
    }
    Check(Clear(stream.value),"Cannot clear the classic HFS recovery stream");
    return 1;
}
}

struct Overwrite::State : Journal { using Journal::Journal; };
Overwrite::Overwrite(int fd)
{
    const auto image=ImageHandle(fd);
    if (!SupportsRecovery(image)) return;
    const auto path=StreamPath(image);
    if (path.empty()) return;
    Check(!ForFd(fd),"A classic HFS overwrite is already active");
    state_=std::make_unique<State>(fd,image,path);
    state_->previous=active;
    state_->attached=true;
    active=state_.get();
}
Overwrite::~Overwrite() { Detach(); }
bool Overwrite::Active() const { return bool(state_); }
bool Overwrite::CommitStarted() const { return state_ && state_->committing; }
void Overwrite::Detach()
{
    if (!state_) return;
    if (state_->attached) active=state_->previous;
    state_.reset();
}
void Overwrite::Commit()
{
    if (!state_) return;
    Check(!state_->faulted&&FlushFileBuffers(state_->image),"Cannot flush the classic HFS overwrite");
    Crash(2);
    Record record{};
    record.offset=Committed;
    state_->committing=true;
    state_->Append(record);
    Crash(3);
    Clear(state_->stream.value);
    Detach();
}
bool ClassicHfsRecovery::Pending(const std::wstring& path)
{
    const auto name=StreamPath(path);
    if (name.empty()) return false;
    Handle stream(CreateFileW(name.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                              nullptr,OPEN_EXISTING,0,nullptr));
    if (!stream && Missing(GetLastError())) return false;
    if (!stream && GetLastError()==ERROR_DIRECTORY) return false;
    if (!stream) return false; // The normal image opener reports host access errors.
    return Length(stream.value)!=0;
}
#ifdef HFV_TESTING
void Overwrite::CrashAt(unsigned phase) { crashPhase=phase; }
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
