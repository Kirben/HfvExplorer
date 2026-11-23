// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "LauncherWriteBack.h"
#include "BrowserSession.h"
#include "StorageJobDialog.h"
#include "WindowsEntryIdentity.h"
#include "CopyWorkerProtocol.h"
#include "ClassicHfsRecovery.h"
#include <shlobj.h>

namespace {
constexpr char RecordName[]="\\.hfv-pending-edit";
constexpr uint64_t RecordVersion=1;
constexpr DWORD MaximumRecord=64*1024;

bool PlainDirectory(const CString& path)
{
    const auto flags=GetFileAttributes(path);
    return flags!=INVALID_FILE_ATTRIBUTES&&
           (flags&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))==FILE_ATTRIBUTE_DIRECTORY;
}
}

CString LauncherPendingRoot()
{
    char profile[MAX_PATH]{};
    if(FAILED(SHGetFolderPathA(nullptr,CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE,nullptr,
                               SHGFP_TYPE_CURRENT,profile)))return {};
    CString root=CString(profile)+"\\HFVExplorer";
    if(!CreateDirectory(root,nullptr)&&!PlainDirectory(root))return {};
    root+="\\PendingEdits";
    if(!CreateDirectory(root,nullptr)&&!PlainDirectory(root))return {};
    return root;
}

LauncherTemporaryDocument::~LauncherTemporaryDocument()
{
    if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
}

Storage::Result LauncherTemporaryDocument::Open(const wchar_t* path,bool cleanup)
{
    const DWORD access=GENERIC_READ|(cleanup?(DELETE|FILE_WRITE_ATTRIBUTES):0);
    file_=CreateFileW(path,access,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file_==INVALID_HANDLE_VALUE)return {1,GetLastError(),"Cannot protect the temporary document"};
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file_,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))
        return {1,ERROR_INVALID_DATA,"Invalid temporary document"};
    return {};
}

Storage::Result LauncherTemporaryDocument::CheckWriteBackStreams() const
{
    DWORD flags=0;
    if(!GetVolumeInformationByHandleW(file_,nullptr,0,nullptr,nullptr,&flags,nullptr,0))
        return {1,GetLastError(),"Cannot determine whether the temporary document supports named streams."};
    if(!(flags&FILE_NAMED_STREAMS))return {};

    std::vector<BYTE> buffer(4096);
    while(!GetFileInformationByHandleEx(file_,FileStreamInfo,buffer.data(),DWORD(buffer.size()))){
        const auto error=GetLastError();
        if(error==ERROR_HANDLE_EOF)return {};
        if((error!=ERROR_MORE_DATA&&error!=ERROR_INSUFFICIENT_BUFFER)||buffer.size()>=1024*1024)
            return {1,error,"Cannot verify the temporary document's named streams."};
        buffer.resize(buffer.size()*2);
    }
    constexpr auto header=offsetof(FILE_STREAM_INFO,StreamName);
    size_t offset=0;
    for(;;){
        const auto remaining=buffer.size()-offset;
        if(remaining<header)break;
        const auto& stream=*reinterpret_cast<const FILE_STREAM_INFO*>(buffer.data()+offset);
        const auto span=stream.NextEntryOffset?stream.NextEntryOffset:remaining;
        if(span>remaining||span<header||stream.StreamNameLength>span-header||
           stream.StreamNameLength%sizeof(wchar_t))break;
        const std::wstring_view name(stream.StreamName,stream.StreamNameLength/sizeof(wchar_t));
        if(name!=L"::$DATA")
            return {1,ERROR_NOT_SUPPORTED,"The temporary document contains named data streams that cannot be written back."};
        if(!stream.NextEntryOffset)return {};
        if(stream.NextEntryOffset%alignof(FILE_STREAM_INFO))break;
        offset+=stream.NextEntryOffset;
    }
    return {1,ERROR_INVALID_DATA,"Cannot verify the temporary document's named streams."};
}

bool LauncherTemporaryDocument::Remove() const
{
    if(CheckWriteBackStreams().code)return false;
    FILE_BASIC_INFO original{};
    if(!GetFileInformationByHandleEx(file_,FileBasicInfo,&original,sizeof(original)))return false;
    if(original.FileAttributes&FILE_ATTRIBUTE_READONLY){
        auto writable=original;
        writable.FileAttributes&=~FILE_ATTRIBUTE_READONLY;
        if(!writable.FileAttributes)writable.FileAttributes=FILE_ATTRIBUTE_NORMAL;
        if(!SetFileInformationByHandle(file_,FileBasicInfo,&writable,sizeof(writable)))return false;
    }
    FILE_DISPOSITION_INFO remove{TRUE};
    if(SetFileInformationByHandle(file_,FileDispositionInfo,&remove,sizeof(remove)))return true;
    if(original.FileAttributes&FILE_ATTRIBUTE_READONLY)
        SetFileInformationByHandle(file_,FileBasicInfo,&original,sizeof(original));
    return false;
}

Storage::Result LauncherTemporaryDocument::ReadVersion(uint64_t& result,Storage::Job& job) const
{
    LARGE_INTEGER start{};
    if(!SetFilePointerEx(file_,start,nullptr,FILE_BEGIN))return {1,GetLastError(),"Cannot seek the temporary document"};
    std::vector<BYTE> buffer(65536);
    uint64_t hash=14695981039346656037ULL;
    for(;;){
        if(job.cancelled())throw Storage::Cancelled();
        DWORD length=0;
        if(!ReadFile(file_,buffer.data(),DWORD(buffer.size()),&length,nullptr))
            return {1,GetLastError(),"Cannot read the temporary document"};
        if(!length)break;
        for(DWORD i=0;i<length;++i){hash^=buffer[i];hash*=1099511628211ULL;}
    }
    result=hash;
    return {};
}

Storage::Result LauncherHandoff::Protect(const wchar_t* path,const MacBrowser::ObjectIdentity& expected,bool readOnly)
{
    file_=CreateFileW(path,GENERIC_READ|FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
                      nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file_==INVALID_HANDLE_VALUE)return {1,GetLastError(),"Cannot protect the exported document"};
    BY_HANDLE_FILE_INFORMATION actual{};
    if(!GetFileInformationByHandle(file_,&actual)||
       (actual.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
       actual.nNumberOfLinks!=1||expected.key.empty()||MacBrowser::WindowsIdentity(actual)!=expected)
        return {1,ERROR_FILE_INVALID,"The exported document changed before launch"};
    FILE_BASIC_INFO attributes{};
    attributes.FileAttributes=(actual.dwFileAttributes&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY;
    if(readOnly)attributes.FileAttributes|=FILE_ATTRIBUTE_READONLY;
    if(!SetFileInformationByHandle(file_,FileBasicInfo,&attributes,sizeof(attributes)))
        return {1,GetLastError(),"Cannot set the exported document attributes"};
    return {};
}

void LauncherHandoff::Close()
{
    if(file_!=INVALID_HANDLE_VALUE)CloseHandle(std::exchange(file_,INVALID_HANDLE_VALUE));
}

LauncherWriteBack::LauncherWriteBack(FileOperations::Location original,std::string version,CString temporary)
    : original_(std::move(original)),version_(std::move(version)),temporary_(std::move(temporary))
{}

bool LauncherWriteBack::CaptureBaseline(CWnd* owner)
{
    const auto path=VolumeCoord::Wide(static_cast<LPCSTR>(temporary_));
    auto result=ShowStorageWaitJob([&](Storage::Job& job){
        LauncherTemporaryDocument document;
        auto opened=document.Open(path.c_str(),false);
        return opened.code?opened:document.ReadVersion(baseline_,job);
    },!owner->IsWindowVisible(),owner);
    if(result.code)
        owner->MessageBox(CString(result.message.c_str())+" Inspect the staged copy at "+temporary_,
                          "HFVExplorer",MB_OK|MB_ICONERROR);
    return !result.code;
}

LauncherWriteBack::~LauncherWriteBack()
{
    if(pending_!=INVALID_HANDLE_VALUE)CloseHandle(pending_);
}

bool LauncherWriteBack::SavePending()
{
    const int separator=temporary_.ReverseFind('\\');
    if(separator<0||!original_.IsClassic()||version_.empty())return false;
    CopyWorkerProtocol::Data record;
    record.put(RecordVersion);
    record.put(std::string(static_cast<LPCSTR>(temporary_.Mid(separator+1))));
    record.put(version_);
    record.put(baseline_);
    CopyWorkerProtocol::EncodeEndpoint(record,{original_,{},{}});
    if(record.bytes.size()+ClassicHfsRecovery::MoveIdSize>MaximumRecord)return false;
    decisionOffset_=DWORD(record.bytes.size());
    pending_=CreateFile(temporary_.Left(separator)+RecordName,GENERIC_WRITE|DELETE,0,nullptr,
                        CREATE_NEW,FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(pending_==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;
    if(WriteFile(pending_,record.bytes.data(),DWORD(record.bytes.size()),&written,nullptr)&&
       written==record.bytes.size()&&FlushFileBuffers(pending_))return true;
    Forget();
    return false;
}

std::vector<std::unique_ptr<LauncherWriteBack>> LauncherWriteBack::LoadPending(const CString& root)
{
    std::vector<std::unique_ptr<LauncherWriteBack>> edits;
    if(root.IsEmpty()||!PlainDirectory(root))return edits;
    WIN32_FIND_DATA info{};
    const auto search=FindFirstFile(root+"\\hfv*.tmp.dir",&info);
    if(search==INVALID_HANDLE_VALUE)return edits;
    struct FindLease { HANDLE handle; ~FindLease(){FindClose(handle);} } searchLease{search};
    do{
        if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))!=FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const CString directory=root+"\\"+info.cFileName;
        const auto handle=CreateFile(directory+RecordName,GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)continue; // Another browser can already own this edit.
        struct FileLease {
            HANDLE handle;
            ~FileLease(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
        } lease{handle};
        BY_HANDLE_FILE_INFORMATION file{};
        if(!GetFileInformationByHandle(handle,&file)||
           (file.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
           file.nNumberOfLinks!=1||file.nFileSizeHigh||!file.nFileSizeLow||file.nFileSizeLow>MaximumRecord)
            continue;
        try{
            CopyWorkerProtocol::Data record;
            record.bytes.resize(file.nFileSizeLow);
            DWORD received=0;
            if(!ReadFile(handle,record.bytes.data(),file.nFileSizeLow,&received,nullptr)||received!=file.nFileSizeLow||
               record.get()!=RecordVersion)continue;
            auto leaf=record.text(),version=record.text();
            const auto baseline=record.get();
            auto original=CopyWorkerProtocol::ParseEndpoint(record,false).location;
            const auto decisionOffset=record.at;
            const auto tail=record.bytes.size()-decisionOffset;
            if(tail>ClassicHfsRecovery::MoveIdSize)continue;
            std::string decision;
            if(tail==ClassicHfsRecovery::MoveIdSize){
                decision.assign(record.bytes.begin()+decisionOffset,record.bytes.end());
                if(decision.find_first_not_of("0123456789abcdef")!=decision.npos)continue;
            }
            if(leaf.empty()||leaf=="."||leaf==".."||leaf.find_first_of("\\/:")!=leaf.npos||
               leaf.find('\0')!=leaf.npos||version.empty()||!original.IsClassic()||original.directory||original.cut)
                continue;
            // A physical drive generation counter cannot be trusted across sessions.
            if(original.IsPhysical())version.clear();
            auto edit=std::make_unique<LauncherWriteBack>(std::move(original),std::move(version),
                                                          directory+"\\"+leaf.c_str());
            edit->baseline_=baseline;
            edit->decisionOffset_=DWORD(decisionOffset);
            edit->decisionId_=std::move(decision);
            edit->pending_=handle;
            lease.handle=INVALID_HANDLE_VALUE;
            edits.push_back(std::move(edit));
        }catch(const std::exception&){
            // Preserve malformed records and the accompanying documents for manual recovery.
        }
    }while(FindNextFile(search,&info));
    return edits;
}

bool LauncherWriteBack::Committed() const
{
    return !decisionId_.empty()&&ClassicHfsRecovery::MoveIsCommitted(decisionId_);
}

Storage::Result LauncherWriteBack::ApplyPending(const LauncherTemporaryDocument& document,Storage::Job& job)
{
    const auto streams=document.CheckWriteBackStreams();
    if(streams.code)return streams;
    const auto file=document.Handle();
    if(original_.IsPhysical())
        return FileOperations::ApplyLaunchEdit(original_,version_,file,job);
    if(Committed())return {1,ERROR_ALREADY_EXISTS,"The Mac document was already updated."};
    if(pending_==INVALID_HANDLE_VALUE||!decisionOffset_)
        return {1,ERROR_INVALID_DATA,"The pending edit has no recovery record."};
    ClassicHfsRecovery::MoveDecision decision;
    LARGE_INTEGER offset{};
    offset.QuadPart=decisionOffset_;
    DWORD written=0;
    if(!SetFilePointerEx(pending_,offset,nullptr,FILE_BEGIN)||
       !WriteFile(pending_,decision.Id().data(),DWORD(decision.Id().size()),&written,nullptr)||
       written!=decision.Id().size()||!FlushFileBuffers(pending_))
        return {1,ERROR_WRITE_FAULT,"Cannot save the write-back recovery decision."};
    decisionId_=decision.Id();
    return FileOperations::ApplyLaunchEdit(original_,version_,file,job,&decision);
}

bool LauncherWriteBack::Forget()
{
    if(pending_==INVALID_HANDLE_VALUE)return true;
    FILE_DISPOSITION_INFO remove{TRUE};
    if(!SetFileInformationByHandle(pending_,FileDispositionInfo,&remove,sizeof(remove)))return false;
    CloseHandle(std::exchange(pending_,INVALID_HANDLE_VALUE));
    return true;
}

bool LauncherWriteBack::Complete(BrowserSession& session)
{
    auto owner=session.Owner();
    auto retain=[&](const CString& reason) {
        owner->MessageBox(reason+" The temporary document is still available at "+temporary_,
                          "HFVExplorer",MB_OK|MB_ICONWARNING);
    };
    if(Committed()){
        if(!RetireCommitted()){retain("The Mac document was already updated. Could not retire the pending edit record.");return false;}
        // Recovery of the Mac image can still need the shared commit marker.
        retain("The Mac document was already updated.");
        return true;
    }
    const auto path=VolumeCoord::Wide(static_cast<LPCSTR>(temporary_));
    LauncherTemporaryDocument document;
    uint64_t current=0;
    auto read=ShowStorageWaitJob([&](Storage::Job& job){
        auto opened=document.Open(path.c_str(),true);
        if(opened.code)return opened;
        const auto streams=document.CheckWriteBackStreams();
        return streams.code?streams:document.ReadVersion(current,job);
    },!owner->IsWindowVisible(),owner);
    if(read.code){
        retain(CString(read.message.c_str()));
        return false;
    }
    if(current==baseline_){
        if(!Forget()){retain("Could not retire the pending edit record.");return false;}
        if(document.Remove())return true;
        retain("Could not remove the temporary copy.");
        return false;
    }
    if(version_.empty()){
        retain("Edits to this temporary copy cannot be written back to the Mac volume.");
        return false;
    }
    if(owner->MessageBox("Write edits to the original Mac document? The temporary file is "+temporary_,
                         "HFVExplorer",MB_YESNO|MB_ICONQUESTION)!=IDYES)
        return false;
    auto result=ShowStorageJob([&](Storage::Job& job){
        return ApplyPending(document,job);
    },"Writing back the edited document",!owner->IsWindowVisible(),true,true,owner);
    if(result.code){
        retain(CString(result.message.c_str()));
        return false;
    }
    if(!Forget()){retain("Could not retire the pending edit record.");return false;}
    if(!decisionId_.empty())ClassicHfsRecovery::ForgetMoveDecision(decisionId_);
    if(!document.Remove())retain("Could not remove the temporary copy.");
    return true;
}
