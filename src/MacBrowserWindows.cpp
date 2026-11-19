// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacBrowserModel.h"
#include "StorageHandoff.h"
#include "VolumeCoordinator.h"
#include "HfsPlusDisk.h"
#include "BrowserPresentation.h"
#include <set>
#include <winioctl.h>
namespace MacBrowser {
#ifdef HFV_TESTING
std::function<void(const char*)> directoryMetadataFault;
std::function<void(const char*)> windowsPublicationFault;
#endif
namespace {
using namespace HfsPlus::Disk;
void Check(bool ok, const char *text) { if (!ok) throw HfsPlus::Error(text); }
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool sparse = false;
    File(const std::wstring &path, DWORD access, DWORD disposition)
        : h(CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, disposition,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
        Check(h != INVALID_HANDLE_VALUE, "Cannot open Windows file");
        BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))){CloseHandle(h);h=INVALID_HANDLE_VALUE;throw HfsPlus::Error("Windows file was replaced by a directory or reparse point");}
    }
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    uint64_t Size() { LARGE_INTEGER n; Check(GetFileSizeEx(h, &n) != FALSE, "Cannot get Windows file size"); return n.QuadPart; }
    void Read(uint64_t offset, void *p, size_t n) {
        LARGE_INTEGER at; at.QuadPart = offset; DWORD got;
        Check(n <= MAXDWORD && SetFilePointerEx(h, at, nullptr, FILE_BEGIN) &&
              ReadFile(h, p, DWORD(n), &got, nullptr) && got == n, "Windows file read failed");
    }
    void Write(const void *p, size_t n) {
        DWORD got; Check(n <= MAXDWORD && WriteFile(h, p, DWORD(n), &got, nullptr) && got == n,
                         "Windows file write failed");
    }
    void EnableSparse() {
        DWORD returned=0;
        sparse=DeviceIoControl(h,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr)!=FALSE;
    }
    void WriteForkBytes(const void* p,size_t size) {
        const auto bytes=static_cast<const uint8_t*>(p);
        // Equal neighbors and a zero first byte imply an all-zero block.
        if(sparse&&size&&bytes[0]==0&&!memcmp(bytes,bytes+1,size-1)){
            LARGE_INTEGER skip;skip.QuadPart=size;
            Check(SetFilePointerEx(h,skip,nullptr,FILE_CURRENT)!=FALSE,"Cannot write sparse companion range");
        }else Write(p,size);
    }
    void SealSparse() {
        if(sparse)Check(SetEndOfFile(h)!=FALSE,"Cannot set the companion file length");
    }
    void Flush() { Check(FlushFileBuffers(h) != FALSE, "Windows file flush failed"); }
    void Dates(const Entry& e){
        if(!e.datesValid)return;
        ULARGE_INTEGER created,modified;created.QuadPart=(uint64_t(e.created)+9561628800ULL)*10000000;
        modified.QuadPart=(uint64_t(e.modified)+9561628800ULL)*10000000;
        FILETIME a{created.LowPart,created.HighPart},b{modified.LowPart,modified.HighPart};
        Check(SetFileTime(h,&a,nullptr,&b)!=FALSE,"Cannot preserve Windows file timestamps");
    }
};
// Keep writers off the captured version. Renames are allowed so our own
// publication can move it, but every moved version is checked by file identity.
struct FileVersion {
 HANDLE handle=INVALID_HANDLE_VALUE;BY_HANDLE_FILE_INFORMATION info{};
 explicit FileVersion(const std::wstring& path){handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);if(handle==INVALID_HANDLE_VALUE){auto error=GetLastError();Check(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND,"Cannot protect destination from competing writes");return;}if(!GetFileInformationByHandle(handle,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;throw HfsPlus::Error("Destination became a directory or reparse point");}}
 ~FileVersion(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
 FileVersion(const FileVersion&)=delete;FileVersion& operator=(const FileVersion&)=delete;
 bool Present()const{return handle!=INVALID_HANDLE_VALUE;}
 bool Matches(const std::wstring& path)const{try{FileVersion other(path);if(Present()!=other.Present())return false;if(!Present())return true;const auto& a=info;const auto& b=other.info;return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime&&a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;}catch(...){return false;}}
 std::string Identity()const{const auto& a=info;return std::to_string(a.dwVolumeSerialNumber)+":"+std::to_string(a.nFileIndexHigh)+":"+std::to_string(a.nFileIndexLow)+":"+std::to_string(a.ftLastWriteTime.dwHighDateTime)+":"+std::to_string(a.ftLastWriteTime.dwLowDateTime);}
 void Verify(const std::wstring& path)const{Check(Matches(path),"Destination changed during staging/publication; replacement was refused");}
};
void CopyNamedStreams(const std::wstring& source,const std::wstring& staged,const Entry& entry,Storage::Job& job)
{
    FileVersion version(source);
    const auto slash=entry.identity.find('/');
    Check(version.Present()&&version.Identity()==entry.identity.substr(0,slash),
          "Windows source changed before copying its alternate data streams");

    WIN32_FIND_STREAM_DATA stream{};
    HANDLE search=FindFirstStreamW(source.c_str(),FindStreamInfoStandard,&stream,0);
    if(search==INVALID_HANDLE_VALUE){
        const auto error=GetLastError();
        Check(error==ERROR_HANDLE_EOF||error==ERROR_INVALID_PARAMETER||error==ERROR_NOT_SUPPORTED,
              "Cannot enumerate Windows alternate data streams");
        return;
    }
    struct Close {HANDLE search;~Close(){FindClose(search);}} close{search};
    std::vector<uint8_t> buffer;
    do{
        if(job.cancelled())throw Storage::Cancelled();
        const std::wstring name=stream.cStreamName;
        if(name==L"::$DATA")continue;
        File input(source+name,GENERIC_READ,OPEN_EXISTING);
        File output(staged+name,GENERIC_WRITE,CREATE_NEW);
        const auto size=input.Size();
        if(buffer.empty())buffer.resize(1024*1024);
        for(uint64_t offset=0;offset<size;){
            if(job.cancelled())throw Storage::Cancelled();
            const auto count=size_t((std::min)(uint64_t(buffer.size()),size-offset));
            input.Read(offset,buffer.data(),count);
            output.Write(buffer.data(),count);
            offset+=count;
        }
        output.Flush();
    }while(FindNextStreamW(search,&stream));
    Check(GetLastError()==ERROR_HANDLE_EOF,"Cannot enumerate Windows alternate data streams");
    version.Verify(source);
}

struct DirectoryLease {
 HANDLE handle;std::string identity;
 explicit DirectoryLease(const std::wstring& path):handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)){Check(handle!=INVALID_HANDLE_VALUE,"Cannot protect destination folder during publication");BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(handle,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){CloseHandle(handle);throw HfsPlus::Error("Destination folder was replaced or became a directory link");}identity=std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow)+":";}
 ~DirectoryLease(){CloseHandle(handle);}
 DirectoryLease(const DirectoryLease&)=delete;DirectoryLease& operator=(const DirectoryLease&)=delete;
};
bool Missing(const std::wstring& path){auto flags=GetFileAttributesW(path.c_str());auto error=GetLastError();return flags==INVALID_FILE_ATTRIBUTES&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND);}
void Publication(const char* phase){
#ifdef HFV_TESTING
 if(windowsPublicationFault)windowsPublicationFault(phase);
#else
 (void)phase;
#endif
}
std::string Recovery(const char* message,const std::wstring& data,const std::wstring& resource){std::string result=message;if(!data.empty())result+=" Data backup: "+BrowserPresentation::Utf8(U16(data));if(!resource.empty())result+=" Metadata backup: "+BrowserPresentation::Utf8(U16(resource));return result+". Preserve these files; do not delete them.";}
// HFVExplorer's wide companion uses a separate signature so an AppleDouble
// reader cannot mistake a truncated 32-bit field for a valid resource fork.
constexpr char WideCompanionSignature[]="HFVAD641";
constexpr uint64_t WideHeaderBytes=64,WideAttributeRecordBytes=18;
struct CompanionAttribute {std::u16string name;uint64_t offset,size;};
struct Double {
    uint64_t offset=0,size=0,finderOffset=0,finderSize=0;
    std::array<uint8_t,32> finder{};
    std::vector<CompanionAttribute> attributes;
};
std::u16string DecodeAttributeName(const std::string& utf8)
{
    if(utf8.empty()||utf8.find('\0')!=utf8.npos)return {};
    const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),int(utf8.size()),nullptr,0);
    if(n<=0)return {};
    std::wstring name(n,0);
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),int(utf8.size()),name.data(),n)!=n)return {};
    return U16(name);
}

bool WideCompanion(File& file,uint64_t bytes,Double& out)
{
    uint8_t header[WideHeaderBytes];
    if(bytes<sizeof(header))return false;
    file.Read(0,header,sizeof(header));
    const auto limit=U64(header+8),resourceBytes=U64(header+16);
    const auto attributeCount=U32(header+24);
    if(U32(header+28)||!Range(limit,resourceBytes,bytes)||limit+resourceBytes!=bytes||
       !Range(WideHeaderBytes,uint64_t(attributeCount)*(WideAttributeRecordBytes+1),limit))
        return false;

    uint64_t cursor=WideHeaderBytes,firstPayload=limit,nextPayload=0;
    std::set<std::u16string> names;
    for(uint32_t i=0;i<attributeCount;++i){
        uint8_t record[WideAttributeRecordBytes];
        if(!Range(cursor,sizeof(record),firstPayload))return false;
        file.Read(cursor,record,sizeof(record));
        const auto at=U64(record),size=U64(record+8);
        const auto length=HfsPlus::Disk::U16(record+16);
        if(i==0)firstPayload=nextPayload=at;
        cursor+=sizeof(record);
        if(at!=nextPayload||!Range(at,size,limit)||!Range(cursor,length,firstPayload))return false;
        // HFS+/HFSX names have at most 127 UTF-16 units, at most 381 UTF-8 bytes.
        if(!length||length>381)return false;
        std::string utf8(length,0);
        file.Read(cursor,utf8.data(),length);
        auto name=DecodeAttributeName(utf8);
        if(name.empty()||name.size()>127||!names.insert(name).second)return false;
        out.attributes.push_back({std::move(name),at,size});
        nextPayload=at+size;
        cursor+=length;
    }
    if(cursor!=firstPayload||(attributeCount&&nextPayload!=limit))return false;
    out.offset=limit;out.size=resourceBytes;
    memcpy(out.finder.data(),header+32,out.finder.size());
    return true;
}
bool AppleDouble(File& f, Double &out) {
    uint8_t header[26];
    const auto bytes=f.Size();
    if (bytes < sizeof(header)) return false;
    f.Read(0, header, sizeof(header));
    if(!memcmp(header,WideCompanionSignature,8)){
        Check(WideCompanion(f,bytes,out),"Invalid 64-bit Windows metadata companion");
        return true;
    }
    unsigned count = HfsPlus::Disk::U16(header + 24);
    const auto version = U32(header + 4);
    if (U32(header) != 0x00051607 || (version != 0x00010000 && version != 0x00020000) ||
        26ULL + count * 12ULL > bytes) return false;
    bool resource = false; std::set<uint32_t> ids;std::vector<std::pair<uint64_t,uint64_t>> ranges;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t e[12]; f.Read(26 + i * 12, e, sizeof(e));
        uint32_t id = U32(e), at = U32(e + 4), n = U32(e + 8);
        if (!ids.insert(id).second || at < 26 + count * 12 || !Range(at, n, bytes)) return false;
        if(n)ranges.push_back({at,uint64_t(at)+n});
        if (id == 2) { out.offset = at; out.size = n; resource = true; }
        if (id == 9 && n >= 16) {
            out.finderOffset = at;
            out.finderSize = n;
            f.Read(at, out.finder.data(), (std::min)(size_t(n), out.finder.size()));
        }
    }
    std::sort(ranges.begin(),ranges.end());for(size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)return false;
    // Finder-only companions have no resource descriptor.
    return resource || out.finderSize != 0;
}

bool AppleDouble(const std::wstring& path,Double& out){if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)return false;File file(path,GENERIC_READ,OPEN_EXISTING);return AppleDouble(file,out);}
HfsPlus::Attributes DoubleAttributes(const std::wstring& path,const Double&){
    File file(path,GENERIC_READ,OPEN_EXISTING);Double d;Check(AppleDouble(file,d),"Attribute companion changed or became invalid");
    HfsPlus::Attributes out;
    for(auto& item:d.attributes){
        auto data=HfsPlus::AttributeData::Snapshot(item.size,[&](uint64_t at,void* p,size_t n){
            file.Read(item.offset+at,p,n);
        });
        out.emplace(std::move(item.name),std::move(data));
    }
    if(d.finderSize<=34)return out;
    uint8_t header[36]{};Check(d.finderSize>=70,"Truncated AppleDouble attributes");file.Read(d.finderOffset+34,header,sizeof(header));
    if(!U32(header))return out; // Some older companions contain only padded Finder info.
    Check(U32(header)==0x41545452,"Unknown AppleDouble attribute extension; companion retained");
    uint64_t total=U32(header+8),start=U32(header+12),length=U32(header+16),at=d.finderOffset+70;
    Check(total<=d.finderOffset+d.finderSize&&total>=at&&start>=at&&Range(start,length,total),"Invalid AppleDouble attribute ranges");
    uint64_t bytes=0;std::vector<std::pair<uint64_t,uint64_t>> ranges;
    for(unsigned i=0;i<HfsPlus::Disk::U16(header+34);++i){uint8_t entry[11];Check(Range(at,11,start),"Truncated AppleDouble attribute entry");file.Read(at,entry,sizeof(entry));unsigned nameLength=entry[10];
        uint64_t next=(at+11+nameLength+3)&~3ULL;Check(nameLength>1&&nameLength<=255&&next<=start,"Invalid AppleDouble attribute name length");
        std::string utf8(nameLength,0);file.Read(at+11,&utf8[0],nameLength);Check(utf8.back()==0&&utf8.find('\0')==utf8.size()-1,"Invalid AppleDouble attribute name");
        utf8.pop_back();auto name=DecodeAttributeName(utf8);Check(!name.empty(),"Invalid AppleDouble UTF-8 attribute name");
        uint64_t offset=U32(entry),size=U32(entry+4);Check(offset>=start&&Range(offset,size,start+length),"Invalid AppleDouble attribute data");
        auto value=HfsPlus::AttributeData::Snapshot(size,[&](uint64_t at,void* p,size_t n){file.Read(offset+at,p,n);});Check(out.emplace(std::move(name),std::move(value)).second,"Duplicate AppleDouble attribute name");if(size)ranges.push_back({offset,offset+size});at=next;
    }
    std::sort(ranges.begin(),ranges.end());for(size_t i=1;i<ranges.size();++i)Check(ranges[i].first>=ranges[i-1].second,"Overlapping AppleDouble attributes");return out;
}
struct DoublePlan {
    HfsPlus::Bytes prefix;
    HfsPlus::Attributes attributes;
    bool large;
    void Write(File& file)const {
        if(large)file.EnableSparse();
        file.Write(prefix.data(),prefix.size());
        HfsPlus::Bytes buffer(attributes.empty()?0:1024*1024);
        for(const auto& [name,value]:attributes)
            for(uint64_t at=0;at<value.size();){
                const auto n=size_t((std::min)(uint64_t(buffer.size()),value.size()-at));
                value.Read(at,buffer.data(),n);
                file.WriteForkBytes(buffer.data(),n);
                at+=n;
            }
    }
};

DoublePlan WideHeader(const Entry& e,const std::vector<std::string>& names,uint64_t bytes)
{
    constexpr auto maximum=uint64_t(INT64_MAX);
    Check(names.size()<=UINT32_MAX,"Too many Windows companion attributes");
    uint64_t prefix=WideHeaderBytes;
    for(const auto& name:names){
        Check(!name.empty()&&name.size()<=381&&Range(prefix,WideAttributeRecordBytes+name.size(),maximum),
              "Invalid wide companion attribute name or size");
        prefix+=WideAttributeRecordBytes+name.size();
    }
    Check(Range(prefix,bytes,maximum)&&Range(prefix+bytes,e.forks[1].size,maximum)&&prefix<=SIZE_MAX,
          "Metadata and resource fork exceed the Windows file size limit");
    const auto resource=prefix+bytes;
    HfsPlus::Bytes b(size_t(prefix),0);
    memcpy(b.data(),WideCompanionSignature,8);
    P64(b.data()+8,resource);P64(b.data()+16,e.forks[1].size);
    P32(b.data()+24,uint32_t(names.size()));memcpy(b.data()+32,e.finder.data(),e.finder.size());
    uint64_t payload=prefix;size_t at=WideHeaderBytes,index=0;
    for(const auto& [key,value]:e.attributes){
        const auto& name=names[index++];
        P64(b.data()+at,payload);P64(b.data()+at+8,value.size());P16(b.data()+at+16,uint16_t(name.size()));
        memcpy(b.data()+at+WideAttributeRecordBytes,name.data(),name.size());
        at+=WideAttributeRecordBytes+name.size();
        payload+=value.size();
    }
    return {std::move(b),e.attributes,resource+e.forks[1].size>UINT32_MAX};
}

DoublePlan DoubleHeader(const Entry& e){
    HfsPlus::ValidateAttributes(e.attributes);
    std::vector<std::string> names;
    uint64_t headerSize=e.attributes.empty()?82:120,total=0;
    bool standard=e.attributes.size()<=65535&&e.forks[1].size<=UINT32_MAX;
    for(const auto& item:e.attributes){
        auto name=BrowserPresentation::Utf8(item.first);
        standard=standard&&name.size()<255;
        headerSize=(headerSize+12+name.size()+3)&~3ULL;
        Check(Range(total,item.second.size(),INT64_MAX),"Attribute values exceed the Windows file size limit");
        total+=item.second.size();
        names.push_back(std::move(name));
    }
    if(!standard||!Range(headerSize,total,UINT32_MAX))return WideHeader(e,names,total);

    const uint64_t resource=headerSize+total;
    HfsPlus::Bytes b(size_t(headerSize),0);P32(b.data(),0x00051607);P32(b.data()+4,0x00020000);P16(b.data()+24,2);P32(b.data()+26,9);P32(b.data()+30,50);P32(b.data()+34,uint32_t(resource-50));P32(b.data()+38,2);P32(b.data()+42,uint32_t(resource));P32(b.data()+46,uint32_t(e.forks[1].size));memcpy(b.data()+50,e.finder.data(),32);
    if(!e.attributes.empty()){P32(b.data()+84,0x41545452);P32(b.data()+92,uint32_t(resource));P32(b.data()+96,uint32_t(headerSize));P32(b.data()+100,uint32_t(total));P16(b.data()+118,uint16_t(e.attributes.size()));size_t at=120,data=size_t(headerSize),i=0;
        for(const auto& item:e.attributes){auto& name=names[i++];P32(b.data()+at,uint32_t(data));P32(b.data()+at+4,uint32_t(item.second.size()));b[at+10]=uint8_t(name.size()+1);memcpy(b.data()+at+11,name.c_str(),name.size()+1);at=(at+12+name.size()+3)&~size_t(3);data+=item.second.size();}}
    return {std::move(b),e.attributes,resource+e.forks[1].size>UINT32_MAX};
}
void Leaf(const std::u16string &s) {
    Check(!s.empty() && s.size()<=255 && s != u"." && s != u".." && s.back() != u'.' && s.back() != u' ', "Invalid Windows filename");
    for (auto c : s) Check(c >= 32 && !wcschr(L"<>:\"/\\|?*", wchar_t(c)), "A Macintosh filename is not valid on Windows; rename it before exporting");
    auto base = Wide(s.substr(0, s.find(u'.')));
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c){return wchar_t(towupper(c));});
    Check(base != L"CON" && base != L"PRN" && base != L"AUX" && base != L"NUL" &&
          !(base.size() == 4 && (base.substr(0,3) == L"COM" || base.substr(0,3) == L"LPT") && base[3] >= L'1' && base[3] <= L'9'),
          "Reserved Windows filename");
}
std::wstring Child(const std::wstring& directory,const std::wstring& name){
    return directory+(directory.back()==L'\\'?L"":L"\\")+name;
}
std::wstring Temp(const std::wstring &directory) {
    static LONG serial=0;
    for(unsigned attempt=0;attempt<1000;++attempt){wchar_t leaf[64];
        swprintf_s(leaf,L"HFV%08lX%08lX%08lX.tmp",GetCurrentProcessId(),GetTickCount(),DWORD(InterlockedIncrement(&serial)));
        auto path=Child(directory,leaf);HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h!=INVALID_HANDLE_VALUE){CloseHandle(h);return path;}
        Check(GetLastError()==ERROR_FILE_EXISTS,"Cannot create a staging file in the destination folder");
    }
    throw HfsPlus::Error("Cannot reserve a staging filename");
}
class Windows final : public Volume, public WorkerEndpoint {
    mutable std::recursive_mutex mutex_;
    std::map<uint32_t,std::wstring> paths_;
    std::string resourceKey_;
    struct ForkReader {std::shared_ptr<File> file;uint64_t offset=0,size=0;};
    std::map<uint32_t,ForkReader> dataReaders_,resourceReaders_;
    uint32_t next_ = 3;
    uint32_t Id(const std::wstring &p) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto &e : paths_) if (!_wcsicmp(e.second.c_str(), p.c_str())) return e.first;
        Check(next_ != UINT32_MAX, "Too many Windows entries"); paths_[next_] = p; return next_++;
    }
    std::wstring Path(uint32_t id)const { std::lock_guard<std::recursive_mutex> lock(mutex_);return paths_.at(id); }
    std::wstring Companion(const std::wstring &p) { auto at = p.find_last_of(L"\\/"); return p.substr(0,at+1) + L"%" + p.substr(at+1); }
    Entry Describe(uint32_t id) {
        auto p = Path(id); WIN32_FILE_ATTRIBUTE_DATA info;
        Check(GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &info) != FALSE, "Windows file no longer exists");
        if(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)throw HfsPlus::Error("Directory links and reparse points are not followed: "+BrowserPresentation::Utf8(U16(p)));
        Entry e; e.id = id; e.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.name = U16(p.substr(p.find_last_of(L"\\/") + 1));
        auto identity=[](const std::wstring& path){
            std::string result;HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            if(h!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION i{};if(GetFileInformationByHandle(h,&i))result=std::to_string(i.dwVolumeSerialNumber)+":"+std::to_string(i.nFileIndexHigh)+":"+std::to_string(i.nFileIndexLow)+":"+std::to_string(i.ftLastWriteTime.dwHighDateTime)+":"+std::to_string(i.ftLastWriteTime.dwLowDateTime);CloseHandle(h);}return result;
        };
        e.identity=identity(p);
        ULARGE_INTEGER created,modified;created.LowPart=info.ftCreationTime.dwLowDateTime;created.HighPart=info.ftCreationTime.dwHighDateTime;
        modified.LowPart=info.ftLastWriteTime.dwLowDateTime;modified.HighPart=info.ftLastWriteTime.dwHighDateTime;
        uint64_t a=created.QuadPart/10000000,b=modified.QuadPart/10000000;
        if(a>=9561628800ULL&&b>=9561628800ULL){
            e.created=a-9561628800ULL;e.modified=b-9561628800ULL;e.datesValid=true;
        }
        if(id==2)e.parent=1;else{auto parent=p.substr(0,p.find_last_of(L"\\/"));if((parent.size()==2||parent.size()==6)&&parent.back()==L':')parent+=L'\\';e.parent=Id(parent);}
        if(!e.directory)e.forks[0].size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        Double d;if(AppleDouble(Companion(p),d)){
            Check(!e.directory||!d.size,"Directory companion contains unexpected resource data; it was preserved");
            if(!e.directory)e.forks[1].size=d.size;e.finder=d.finder;
            const auto companion=identity(Companion(p));
            if(e.identity.empty()||companion.empty())e.identity.clear();
            else e.identity+="/"+companion;
        }
        return e;
    }
public:
    explicit Windows(std::wstring root) {
        root=BrowserPresentation::WindowsPath(root);
        while(root.size()>7&&(root.back()==L'\\'||root.back()==L'/'))root.pop_back();
        resourceKey_=VolumeCoord::HostKey(root);
        paths_[2] = root; Check(Describe(2).directory, "Destination is not a folder");
    }
    Entry DescribeFile(const std::wstring& path){return Describe(Id(BrowserPresentation::WindowsPath(path)));}
    std::wstring HostPath(uint32_t id)const override{return Path(id);}
    std::string ResourceKey()const override{return resourceKey_;}
    std::u16string Name() const override { return U16(Path(2)); }
    const char *Kind() const override { return "Windows"; }
    bool Writable() const override { return true; }
    bool SupportsAttributes()const override{return true;}
    HfsPlus::Attributes GetAttributes(uint32_t id)override{Double d;auto path=Companion(Path(id));return AppleDouble(path,d)?DoubleAttributes(path,d):HfsPlus::Attributes{};}
    void PublishMetadata(uint32_t id,const Entry& e,std::function<void()> finish={}){
        auto header=DoubleHeader(e);auto path=Path(id),companion=Companion(path),root=path.substr(0,path.find_last_of(L"\\/"));
        DirectoryLease directory(root);FileVersion original(companion);std::unique_ptr<FileVersion> replacement,primary;std::unique_ptr<DirectoryLease> folder;
        if(e.directory){folder.reset(new DirectoryLease(path));Check(e.identity.compare(0,folder->identity.size(),folder->identity)==0,"Primary folder changed before metadata publication");}else{primary.reset(new FileVersion(path));Check(primary->Present()&&e.identity.substr(0,e.identity.find('/'))==primary->Identity(),"Primary file changed before metadata publication");}
        Double old;bool exists=original.Present();Check(!exists||AppleDouble(companion,old),"An unrelated companion blocks metadata changes; it was preserved");Check(old.size==e.forks[1].size,"Resource fork changed before metadata publication");
        std::wstring staged,backup;bool backed=false,published=false;std::vector<std::unique_ptr<VolumeCoord::OperationLease>> leases;auto reserve=[&](const std::wstring& p){leases.emplace_back(new VolumeCoord::OperationLease({{VolumeCoord::PathKey(p),VolumeCoord::Mode::Write}}));};
        try{staged=Temp(root);reserve(staged);{File output(staged,GENERIC_WRITE,TRUNCATE_EXISTING);header.Write(output);if(e.forks[1].size){File input(companion,GENERIC_READ,OPEN_EXISTING);HfsPlus::Bytes buffer(65536);for(uint64_t at=0;at<old.size;){auto n=size_t((std::min)(uint64_t(buffer.size()),old.size-at));input.Read(old.offset+at,buffer.data(),n);output.WriteForkBytes(buffer.data(),n);at+=n;}}output.SealSparse();output.Flush();}
            {std::lock_guard<std::recursive_mutex> lock(mutex_);resourceReaders_.erase(id);}
            replacement.reset(new FileVersion(staged));Publication("metadata-staged");if(primary)primary->Verify(path);original.Verify(companion);
            if(exists){backup=Temp(root);reserve(backup);Check(MoveFileExW(companion.c_str(),backup.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot preserve old metadata companion");backed=true;original.Verify(backup);}
            Check(MoveFileExW(staged.c_str(),companion.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot publish metadata companion");staged.clear();published=true;Publication("metadata-published");if(finish)finish();if(primary)primary->Verify(path);replacement->Verify(companion);if(backed)original.Verify(backup);
        }catch(...){bool restored=true;if(published&&(!replacement->Matches(companion)||!DeleteFileW(companion.c_str())))restored=false;if(backed){if((primary&&!primary->Matches(path))||!MoveFileExW(backup.c_str(),companion.c_str(),MOVEFILE_WRITE_THROUGH))restored=false;}if(!staged.empty())DeleteFileW(staged.c_str());if(!backed&&!backup.empty())DeleteFileW(backup.c_str());if(backed||published)Storage::OutcomeEvidence::NoteRollback(restored?Storage::Rollback::Restored:Storage::Rollback::Failed);if(!restored)throw HfsPlus::Error(Recovery("Metadata rollback could not restore the original companion",{},backed?(Missing(backup)?companion:backup):std::wstring{}));throw;}
        if(!backup.empty())DeleteFileW(backup.c_str());
    }
    void SetAttributes(uint32_t id,const HfsPlus::Attributes& attributes)override{auto e=Describe(id);e.attributes=attributes;e.attributesKnown=true;PublishMetadata(id,e);}
    uint32_t MakeDirectoryWithAttributes(uint32_t parent,const Entry& e)override{
        ValidateEntry(e);auto path=Child(Path(parent),Wide(e.name));Check(GetFileAttributesW(Companion(path).c_str())==INVALID_FILE_ATTRIBUTES,"An unrelated companion blocks folder creation");return MakeDirectory(parent,e.name);
    }
    void FinalizeDirectory(uint32_t id,const Entry& desired,const Entry& expected,bool merge)override{
#ifdef HFV_TESTING
        if(directoryMetadataFault)directoryMetadataFault("before-directory-metadata");
#endif
        auto path=Path(id);HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        Check(h!=INVALID_HANDLE_VALUE,"Cannot open destination folder for metadata");struct Close{HANDLE h;~Close(){CloseHandle(h);}}close{h};BY_HANDLE_FILE_INFORMATION info{};
        Check(GetFileInformationByHandle(h,&info)&&(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Destination folder is unavailable or became a directory link");
        auto prefix=std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow)+":";
        Check(expected.directory&&expected.identity.compare(0,prefix.size(),prefix)==0,"Destination folder was replaced during copying");auto now=Describe(id);
        auto companionIdentity=[](const Entry& e){auto at=e.identity.find('/');return at==e.identity.npos?std::string{}:e.identity.substr(at);};
        Check(merge||companionIdentity(now)==companionIdentity(expected),"Destination folder metadata changed during copying; companion retained");
        FILETIME created=info.ftCreationTime,modified=info.ftLastWriteTime,rollbackCreated=created,rollbackModified=modified;
        if(desired.datesValid){ULARGE_INTEGER a,b;a.QuadPart=(desired.created+9561628800ULL)*10000000ULL;b.QuadPart=(desired.modified+9561628800ULL)*10000000ULL;created={a.LowPart,a.HighPart};modified={b.LowPart,b.HighPart};if(merge){rollbackCreated=created;rollbackModified=modified;}}
        auto finish=[&]{
#ifdef HFV_TESTING
            if(directoryMetadataFault)directoryMetadataFault("directory-companion-published");
#endif
            if(desired.datesValid)Check(SetFileTime(h,&created,nullptr,&modified)!=FALSE,"Cannot preserve destination folder timestamps");
#ifdef HFV_TESTING
            if(directoryMetadataFault)directoryMetadataFault("directory-dates-written");
#endif
        };
        try{
            if(merge)finish();else{auto metadata=now;metadata.finder=desired.finder;metadata.attributes=desired.attributes;metadata.attributesKnown=true;
                bool companion=std::any_of(metadata.finder.begin(),metadata.finder.end(),[](uint8_t b){return b!=0;})||!metadata.attributes.empty();
                if(companion)PublishMetadata(id,metadata,finish);else finish();}
        }catch(...){auto failure=std::current_exception();bool restored=SetFileTime(h,&rollbackCreated,nullptr,&rollbackModified)!=FALSE;Storage::OutcomeEvidence::NoteRollback(restored?Storage::Rollback::Restored:Storage::Rollback::Failed);Check(restored,"Folder timestamp rollback failed; source retained");std::rethrow_exception(failure);}
    }
    void ValidateCopyNames(uint32_t parent,const std::vector<Entry>& entries,bool parentExists)override{
        std::vector<std::wstring> names,reserved;
        for(const auto& e:entries){Leaf(e.name);auto name=Wide(e.name);for(const auto& other:names)Check(_wcsicmp(name.c_str(),other.c_str())!=0,"Source names collide on the Windows destination; rename them before copying");names.push_back(name);
            bool companion=!e.directory||!e.attributes.empty()||std::any_of(e.finder.begin(),e.finder.end(),[](uint8_t b){return b!=0;});
            if(parentExists){auto path=Child(Path(parent),name);auto leaf=path.substr(path.find_last_of(L"\\/")+1);if(leaf.size()>1&&leaf[0]==L'%'){auto owner=Child(Path(parent),leaf.substr(1));Double d;Check(GetFileAttributesW(owner.c_str())==INVALID_FILE_ATTRIBUTES||!AppleDouble(path,d),"The destination file is another item's metadata companion; it was preserved");}
                Entry existing;if(e.directory&&FindChild(parent,e.name,existing)&&existing.directory){Double d;companion=AppleDouble(Companion(path),d);}}
            if(companion)reserved.push_back(L"%"+name);
        }
        for(const auto& name:names)for(const auto& companion:reserved)Check(_wcsicmp(name.c_str(),companion.c_str())!=0,"Source names collide with required Windows metadata companions; rename them before copying");
    }
    bool CaseSensitive() const override { return false; }
    WorkerExecution Execution()const override{WorkerExecution out;out.windowsPaths=true;return out;}
    void ReleaseWorkerReaders(const std::vector<std::string>& keys)override{std::lock_guard<std::recursive_mutex> lock(mutex_);for(unsigned fork=0;fork<2;++fork){auto& readers=fork?resourceReaders_:dataReaders_;for(auto it=readers.begin();it!=readers.end();){auto path=Path(it->first);auto key=VolumeCoord::PathKey(fork?Companion(path):path);if(VolumeCoord::Overlapping(keys,key))it=readers.erase(it);else ++it;}}}
    void Close()override{std::lock_guard<std::recursive_mutex> lock(mutex_);dataReaders_.clear();resourceReaders_.clear();}
    void ValidateEntry(const Entry& e)const override {
        Volume::ValidateEntry(e);Check(!e.datesValid||(e.created<=UINT64_MAX/10000000ULL-9561628800ULL&&e.modified<=UINT64_MAX/10000000ULL-9561628800ULL),"Dates exceed the Windows FILETIME range");DoubleHeader(e);Leaf(e.name);Leaf(u"%"+e.name);
    }
    uint64_t FreeBytes() const override { ULARGE_INTEGER free{}; GetDiskFreeSpaceExW(Path(2).c_str(), &free, nullptr, nullptr); return free.QuadPart; }
    Entry Stat(uint32_t id) override { return Describe(id); }
    bool FindChild(uint32_t parent,const std::u16string& name,Entry& entry)override{
        Leaf(name);Check(Describe(parent).directory,"Destination is not a Windows folder");auto root=Path(parent),path=Child(root,Wide(name));WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW(path.c_str(),&data);
        if(find==INVALID_HANDLE_VALUE){DWORD error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return false;throw HfsPlus::Error("Cannot inspect Windows destination (Windows error "+std::to_string(error)+")");}
        FindClose(find);entry=Describe(Id(Child(root,data.cFileName)));entry.parent=parent;return true;
    }
    void ValidateCopyTo(uint32_t id,Volume& target,uint32_t parent)override {
        auto dst=dynamic_cast<Windows*>(&target);if(!dst)return;
        auto canonical=[](const std::wstring& path){
            HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            Check(h!=INVALID_HANDLE_VALUE,"Cannot establish copy source/destination identity");
            wchar_t name[32768];DWORD n=GetFinalPathNameByHandleW(h,name,ARRAYSIZE(name),FILE_NAME_NORMALIZED);CloseHandle(h);
            Check(n&&n<ARRAYSIZE(name),"Cannot normalize copy source/destination path");return std::wstring(name,n);
        };
        auto e=Stat(id);auto source=canonical(Path(id)),directory=canonical(dst->Path(parent));
        if(e.directory)Check(_wcsicmp(source.c_str(),directory.c_str())&&
            !(directory.size()>source.size()&&directory[source.size()]==L'\\'&&!_wcsnicmp(source.c_str(),directory.c_str(),source.size())),
            "Cannot copy a Windows folder into itself or its descendants");
        auto final=Child(dst->Path(parent),Wide(e.name));
        if(GetFileAttributesW(final.c_str())!=INVALID_FILE_ATTRIBUTES)
            Check(_wcsicmp(canonical(final).c_str(),source.c_str())!=0,"Source and destination are the same Windows entry");
    }
    std::vector<Entry> List(uint32_t parent) override {
        auto root = Path(parent); Check(Describe(parent).directory, "Not a Windows folder");
        WIN32_FIND_DATAW data; HANDLE h = FindFirstFileW((Child(root,L"*")).c_str(), &data);
        if (h == INVALID_HANDLE_VALUE) { Check(GetLastError() == ERROR_FILE_NOT_FOUND, "Cannot enumerate Windows folder"); return {}; }
        struct Close { HANDLE h; ~Close() { FindClose(h); } } close{h}; std::vector<Entry> out;
        do {
            std::wstring name = data.cFileName; if (name == L"." || name == L"..") continue;
            auto path = Child(root,name);
            if (name[0] == L'%' && GetFileAttributesW((Child(root,name.substr(1))).c_str()) != INVALID_FILE_ATTRIBUTES) {
                Double d; if (AppleDouble(path, d)) continue;
            }
            auto e = Describe(Id(path)); e.parent = parent; out.push_back(e);
        } while (FindNextFileW(h, &data));
        Check(GetLastError() == ERROR_NO_MORE_FILES, "Windows folder enumeration failed"); return out;
    }
    void Read(uint32_t id, unsigned fork, uint64_t at, void *p, size_t n) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto& readers=fork?resourceReaders_:dataReaders_;
        auto& stored=readers[id];
        if(!stored.file){
            const auto path=Path(id);
            auto file=std::make_shared<File>(fork?Companion(path):path,GENERIC_READ,OPEN_EXISTING);
            uint64_t offset=0,size=file->Size();
            if(fork){
                Double companion;
                Check(AppleDouble(*file,companion),"Resource fork companion is missing or invalid");
                offset=companion.offset;size=companion.size;
            }
            stored={std::move(file),offset,size};
        }
        Check(Range(at,n,stored.size),"Read outside Windows file");
        stored.file->Read(stored.offset+at,p,n);
    }
    uint32_t MakeDirectory(uint32_t parent, const std::u16string &name) override {
        Leaf(name); auto path = Child(Path(parent),Wide(name));
        Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "Cannot create Windows folder"); return Id(path);
    }
    uint32_t Put(uint32_t parent, const Entry &e, Reader read, bool replace) override {
        return PutFile(parent,e,std::move(read),replace,{},nullptr);
    }
    uint32_t PutFromHost(uint32_t parent,const Entry& e,Reader read,bool replace,
                         const std::wstring& source,Storage::Job& job) override {
        return PutFile(parent,e,std::move(read),replace,source,&job);
    }
private:
    uint32_t PutFile(uint32_t parent,const Entry& e,Reader read,bool replace,
                     const std::wstring& source,Storage::Job* job) {
        Leaf(e.name);Leaf(u"%"+e.name);
        auto root = Path(parent), path = Child(root,Wide(e.name)), companion = Companion(path);
        if(!e.name.empty()&&e.name[0]==u'%'&&e.name.size()>1){Double owned;auto owner=Child(root,Wide(e.name.substr(1)));Check(GetFileAttributesW(owner.c_str())==INVALID_FILE_ATTRIBUTES||!AppleDouble(path,owned),"The destination is another item's metadata companion; it was preserved");}
        DirectoryLease directory(root);FileVersion original(path),originalResource(companion);
        bool existed=original.Present(),sidecar=originalResource.Present();Double old;
        if(!e.expectedDestinationIdentity.empty()){auto identity=existed?original.Identity():std::string{};if(sidecar)identity+="/"+originalResource.Identity();Check(existed&&identity==e.expectedDestinationIdentity,"Destination changed while the overwrite decision was pending; source retained");}
        Check(!existed || replace, "Destination already exists");
        Check(!sidecar || (existed && AppleDouble(companion, old)), "An unrelated companion file blocks export; it was preserved");
        auto outgoing=e;if(!e.attributesKnown&&sidecar)outgoing.attributes=DoubleAttributes(companion,old);const bool withCompanion = !outgoing.portableContainer || !outgoing.attributes.empty() ||
            outgoing.forks[1].size != 0;
        auto header=DoubleHeader(outgoing);
        std::wstring staged, resource, backup, backupResource;
        bool published = false, publishedResource = false, backed = false, backedResource = false;
        std::unique_ptr<FileVersion> newData,newResource;
        std::vector<std::unique_ptr<VolumeCoord::OperationLease>> staging;
        auto reserve=[&](const std::wstring& name){staging.emplace_back(new VolumeCoord::OperationLease({{VolumeCoord::PathKey(name),VolumeCoord::Mode::Write}}));};
        try {
            staged = Temp(root);reserve(staged);
            if (withCompanion) { resource = Temp(root); reserve(resource); }
            {
                File data(staged, GENERIC_WRITE, TRUNCATE_EXISTING);
                std::unique_ptr<File> rsrc;
                if (withCompanion) {
                    rsrc = std::make_unique<File>(resource, GENERIC_WRITE, TRUNCATE_EXISTING);
                    header.Write(*rsrc);
                }
                Publication("before-data-write");
                std::vector<uint8_t> buffer(1024*1024);
                for (unsigned f=0; f<2; ++f) for (uint64_t at=0; at<e.forks[f].size;) {
                    size_t n=size_t((std::min)(uint64_t(buffer.size()),e.forks[f].size-at));
                    read(f,at,buffer.data(),n);
                    if(f)rsrc->WriteForkBytes(buffer.data(),n);else data.Write(buffer.data(),n);
                    at+=n;
                }
                if(!job)data.Dates(e);
                Publication("before-data-flush");data.Flush();
                if (rsrc) { rsrc->SealSparse(); rsrc->Dates(e); rsrc->Flush(); }
            }
            if(job)CopyNamedStreams(source,staged,e,*job);
            if(job&&e.datesValid){File data(staged,GENERIC_WRITE,OPEN_EXISTING);data.Dates(e);data.Flush();}
            {std::lock_guard<std::recursive_mutex> lock(mutex_);for(const auto& entry:paths_)if(!_wcsicmp(entry.second.c_str(),path.c_str())){dataReaders_.erase(entry.first);resourceReaders_.erase(entry.first);}}
            newData.reset(new FileVersion(staged));if (withCompanion) newResource.reset(new FileVersion(resource));Publication("staged");original.Verify(path);originalResource.Verify(companion);
            if (existed) { backup=Temp(root);reserve(backup); Check(MoveFileExW(path.c_str(),backup.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot preserve old destination"); backed=true;original.Verify(backup); }
            if (sidecar) { backupResource=Temp(root);reserve(backupResource); Check(MoveFileExW(companion.c_str(),backupResource.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot preserve old resource fork"); backedResource=true;originalResource.Verify(backupResource); }
            Check(MoveFileExW(staged.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot publish exported data"); staged.clear(); published=true;Publication("data-published");
            if (withCompanion) {
                Check(MoveFileExW(resource.c_str(),companion.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,
                      "Cannot publish exported resource fork");
                resource.clear();
                publishedResource = true;
                Publication("resource-published");
                newResource->Verify(companion);
            } else Check(Missing(companion), "An unrelated companion appeared during export");
            newData->Verify(path);
            if(backed)original.Verify(backup);
            if(backedResource)originalResource.Verify(backupResource);
        } catch (...) {
            bool restored=true;
            if(publishedResource && (!newResource->Matches(companion)||!DeleteFileW(companion.c_str())))restored=false;
            if(published && (!newData->Matches(path)||!DeleteFileW(path.c_str())))restored=false;
            bool slots=(!backed||Missing(path))&&(!backedResource||Missing(companion));if(!slots)restored=false;
            if(slots){if(backedResource && !MoveFileExW(backupResource.c_str(),companion.c_str(),MOVEFILE_WRITE_THROUGH))restored=false;
            if(backed && !MoveFileExW(backup.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))restored=false;}
            if(!staged.empty())DeleteFileW(staged.c_str()); if(!resource.empty())DeleteFileW(resource.c_str());
            if(!backed&&!backup.empty())DeleteFileW(backup.c_str()); if(!backedResource&&!backupResource.empty())DeleteFileW(backupResource.c_str());
            if(published||publishedResource||backed||backedResource)Storage::OutcomeEvidence::NoteRollback(restored?Storage::Rollback::Restored:Storage::Rollback::Failed);
            if(!restored)throw HfsPlus::Error(Recovery("Export rollback could not restore the original pair",backed?(Missing(backup)?path:backup):std::wstring{},backedResource?(Missing(backupResource)?companion:backupResource):std::wstring{})); throw;
        }
        if(!backup.empty())DeleteFileW(backup.c_str()); if(!backupResource.empty())DeleteFileW(backupResource.c_str()); return Id(path);
    }
    void Rename(uint32_t id, uint32_t parent, const std::u16string &name) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Check(id!=2,"Cannot rename a volume root");Leaf(name);auto entry=Describe(id);
        Check(parent==entry.parent,"Rename must stay in the same Windows folder");
        auto path=Path(id),target=Child(Path(parent),Wide(name));if(path==target)return;
        auto oldResource=Companion(path),newResource=Companion(target);Double data;
        bool resource=AppleDouble(oldResource,data);
        Check(!_wcsicmp(path.c_str(),target.c_str())||GetFileAttributesW(target.c_str())==INVALID_FILE_ATTRIBUTES,"Destination already exists");
        if(resource){Leaf(u"%"+name);Check(!_wcsicmp(oldResource.c_str(),newResource.c_str())||GetFileAttributesW(newResource.c_str())==INVALID_FILE_ATTRIBUTES,"An existing resource companion blocks rename; it was preserved");}
        // A copied directory can retain cached child readers. Its subtree lease
        // excludes other users, so release only those handles before renaming.
        auto descendant=[&](const std::wstring& child){return !_wcsicmp(child.c_str(),path.c_str())||(entry.directory&&child.size()>path.size()&&child[path.size()]==L'\\'&&!_wcsnicmp(child.c_str(),path.c_str(),path.size()));};
        for(const auto& item:paths_)if(descendant(item.second)){dataReaders_.erase(item.first);resourceReaders_.erase(item.first);}
        if(resource)Check(MoveFileExW(oldResource.c_str(),newResource.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot rename resource companion; source retained");
        if(!MoveFileExW(path.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)){
            if(resource){bool restored=MoveFileExW(newResource.c_str(),oldResource.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;Storage::OutcomeEvidence::NoteRollback(restored?Storage::Rollback::Restored:Storage::Rollback::Failed);Check(restored,"Rename rollback failed; data remains at the old name and the resource companion remains at the new name. Preserve both files.");}
            throw HfsPlus::Error("Cannot rename the selected Windows entry; source retained");
        }
        for(auto& item:paths_)if(descendant(item.second))item.second=target+item.second.substr(path.size());
    }
    void Remove(uint32_t id) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Check(id!=2,"Cannot remove a volume root"); auto path=Path(id); auto e=Describe(id);
        dataReaders_.erase(id);resourceReaders_.erase(id);
        if(e.directory){Double d;bool companion=AppleDouble(Companion(path),d);Check(RemoveDirectoryW(path.c_str())!=FALSE,"Cannot remove Windows folder");if(companion)Check(DeleteFileW(Companion(path).c_str())!=FALSE,"Cannot remove folder metadata companion");}
        else { Double d; bool companion=AppleDouble(Companion(path),d);
            Check(DeleteFileW(path.c_str())!=FALSE,"Cannot remove source Windows file");
            if(companion)Check(DeleteFileW(Companion(path).c_str())!=FALSE,"Cannot remove source resource companion"); }
    }
};
}
bool WindowsResourceCompanion(const std::wstring& path){Double data;return AppleDouble(path,data);}
std::pair<std::shared_ptr<Volume>,uint32_t> ResolveWindowsFile(const std::wstring& path){
    auto slash=path.find_last_of(L"\\/");Check(slash!=path.npos,"Use an absolute Windows source path");
    auto leaf=path.substr(slash+1);Check(!leaf.empty()&&leaf!=L"."&&leaf!=L".."&&leaf.find_first_of(L"*?")==leaf.npos,"Select a Windows file or folder");
    WIN32_FIND_DATAW data{};auto exact=BrowserPresentation::WindowsPath(path);HANDLE find=FindFirstFileW(exact.c_str(),&data);Check(find!=INVALID_HANDLE_VALUE,"The selected Windows source no longer exists");FindClose(find);
    // FindFirstFile on the exact path preserves the stored spelling, whereas
    // the legacy Windows view may capitalize display paths for presentation.
    auto parent=path.substr(0,slash);if(parent.size()==2&&parent.back()==L':')parent+=L'\\';
    auto actual=parent+(parent.back()==L'\\'?L"":L"\\")+data.cFileName;
    auto volume=std::make_shared<Windows>(parent);auto entry=volume->DescribeFile(actual);return {TrackWorkerVolume(volume),entry.id};
}
Entry DescribeWindowsFile(const std::wstring& path){
    auto parent=path.substr(0,path.find_last_of(L"\\/"));if(parent.size()==2&&parent.back()==L':')parent+=L'\\';
    Windows volume(parent);return volume.DescribeFile(path);
}
std::shared_ptr<Volume> OpenWindows(const std::wstring &path) { return TrackWorkerVolume(std::make_shared<Windows>(path)); }
}
