// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "LauncherWriteBack.h"
#include "BrowserSession.h"
#include "StorageJobDialog.h"
#include "utils.h"
#include "WindowsEntryIdentity.h"

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
    if(process_)CloseHandle(process_);
}

bool LauncherWriteBack::Ready() const
{
    if(!process_)return true;
    if(WaitForSingleObject(process_,0)==WAIT_TIMEOUT)return false;
    if(GetTickCount()-started_<2500)return true;
    return GetFileAttributes(temporary_)==INVALID_FILE_ATTRIBUTES||can_have_exclusive_access(temporary_);
}

void LauncherWriteBack::Complete(BrowserSession& session)
{
    auto owner=session.Owner();
    auto retain=[&](const CString& reason) {
        owner->MessageBox(reason+" The temporary document is still available at "+temporary_,
                          "HFVExplorer",MB_OK|MB_ICONWARNING);
    };
    DWORD code=STILL_ACTIVE;
    if(!process_||!GetExitCodeProcess(process_,&code)||code==STILL_ACTIVE||GetTickCount()-started_<2500){
        retain("Could not reliably track the application's editing session.");
        return;
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
        return;
    }
    if(current==baseline_){
        if(!document.Remove())retain("Could not remove the temporary copy.");
        return;
    }
    if(version_.empty()){
        retain("Edits to this temporary copy cannot be written back to the Mac volume.");
        return;
    }
    if(owner->MessageBox("Write edits to the original Mac document? The temporary file is "+temporary_,
                         "HFVExplorer",MB_YESNO|MB_ICONQUESTION)!=IDYES)
        return;
    auto result=ShowStorageJob([&](Storage::Job& job){
        return FileOperations::ApplyLaunchEdit(original_,version_,document.Handle(),job);
    },"Writing back the edited document",!owner->IsWindowVisible(),true,true,owner);
    if(result.code)
        retain(CString(result.message.c_str()));
    else
        if(!document.Remove())retain("Could not remove the temporary copy.");
}
