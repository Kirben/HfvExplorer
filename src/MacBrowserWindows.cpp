// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include "HfsPlusDisk.h"
#include "BrowserPresentation.h"
#include <set>
namespace MacBrowser {
namespace {
using namespace HfsPlus::Disk;
void Check(bool ok, const char *text) { if (!ok) throw HfsPlus::Error(text); }
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    File(const std::wstring &path, DWORD access, DWORD disposition)
        : h(CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, disposition,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {
        Check(h != INVALID_HANDLE_VALUE, "Cannot open Windows file");
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
    void Flush() { Check(FlushFileBuffers(h) != FALSE, "Windows file flush failed"); }
    void Dates(const Entry& e){
        if(!e.datesValid)return;
        ULARGE_INTEGER created,modified;created.QuadPart=(uint64_t(e.created)+9561628800ULL)*10000000;
        modified.QuadPart=(uint64_t(e.modified)+9561628800ULL)*10000000;
        FILETIME a{created.LowPart,created.HighPart},b{modified.LowPart,modified.HighPart};
        Check(SetFileTime(h,&a,nullptr,&b)!=FALSE,"Cannot preserve Windows file timestamps");
    }
};
struct Double { uint64_t offset = 0, size = 0; std::array<uint8_t,32> finder{}; };
bool AppleDouble(const std::wstring &path, Double &out) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    File f(path, GENERIC_READ, OPEN_EXISTING); uint8_t header[26];
    if (f.Size() < sizeof(header)) return false;
    f.Read(0, header, sizeof(header)); unsigned count = HfsPlus::Disk::U16(header + 24);
    if (U32(header) != 0x00051607 || U32(header + 4) != 0x00020000 || count > 1024 ||
        26ULL + count * 12ULL > f.Size()) return false;
    bool resource = false; std::set<uint32_t> ids;std::vector<std::pair<uint64_t,uint64_t>> ranges;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t e[12]; f.Read(26 + i * 12, e, sizeof(e));
        uint32_t id = U32(e), at = U32(e + 4), n = U32(e + 8);
        if (!ids.insert(id).second || at < 26 + count * 12 || !Range(at, n, f.Size())) return false;
        if(n)ranges.push_back({at,uint64_t(at)+n});
        if (id == 2) { out.offset = at; out.size = n; resource = true; }
        if (id == 9 && n >= out.finder.size()) f.Read(at, out.finder.data(), out.finder.size());
    }
    std::sort(ranges.begin(),ranges.end());for(size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)return false;
    return resource;
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
class Windows final : public Volume {
    mutable std::recursive_mutex mutex_;
    std::map<uint32_t,std::wstring> paths_;
    std::string resourceKey_;
    std::map<uint32_t,std::shared_ptr<File>> dataReaders_,resourceReaders_;
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
        if(a>=9561628800ULL&&b>=9561628800ULL&&a-9561628800ULL<=UINT32_MAX&&b-9561628800ULL<=UINT32_MAX){
            e.created=uint32_t(a-9561628800ULL);e.modified=uint32_t(b-9561628800ULL);e.datesValid=true;
        }
        if(id==2)e.parent=1;else{auto parent=p.substr(0,p.find_last_of(L"\\/"));if((parent.size()==2||parent.size()==6)&&parent.back()==L':')parent+=L'\\';e.parent=Id(parent);}
        if (!e.directory) {
            e.forks[0].size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            Double d; if (AppleDouble(Companion(p), d)) { e.forks[1].size = d.size; e.finder = d.finder;e.identity+="/"+identity(Companion(p)); }
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
    bool CaseSensitive() const override { return false; }
    void Close()override{std::lock_guard<std::recursive_mutex> lock(mutex_);dataReaders_.clear();resourceReaders_.clear();}
    void ValidateEntry(const Entry& e)const override {
        Leaf(e.name);Leaf(u"%"+e.name);Check(e.forks[1].size<=UINT32_MAX-82,"Resource fork is too large for AppleDouble export; use raw fork export");
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
        auto path = Path(id); uint64_t size, offset = 0;
        if (fork) { Double d; Check(AppleDouble(Companion(path), d), "Resource fork companion is missing or invalid"); offset = d.offset; size = d.size; path = Companion(path); }
        else { File f(path, GENERIC_READ, OPEN_EXISTING); size = f.Size(); }
        Check(Range(at, n, size), "Read outside Windows file");
        auto& readers=fork?resourceReaders_:dataReaders_;auto& file=readers[id];
        if(!file)file=std::make_shared<File>(path,GENERIC_READ,OPEN_EXISTING);file->Read(offset+at,p,n);
    }
    uint32_t MakeDirectory(uint32_t parent, const std::u16string &name) override {
        Leaf(name); auto path = Child(Path(parent),Wide(name));
        Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "Cannot create Windows folder"); return Id(path);
    }
    uint32_t Put(uint32_t parent, const Entry &e, Reader read, bool replace) override {
        Leaf(e.name);Leaf(u"%"+e.name); Check(e.forks[1].size <= UINT32_MAX - 82, "Resource fork is too large for AppleDouble export; export the raw fork instead");
        auto root = Path(parent), path = Child(root,Wide(e.name)), companion = Companion(path);
        bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        bool sidecar = GetFileAttributesW(companion.c_str()) != INVALID_FILE_ATTRIBUTES; Double old;
        Check(!existed || replace, "Destination already exists");
        Check(!sidecar || (existed && AppleDouble(companion, old)), "An unrelated companion file blocks export; it was preserved");
        std::wstring staged, resource, backup, backupResource;
        bool published = false, publishedResource = false, backed = false, backedResource = false;
        std::vector<std::unique_ptr<VolumeCoord::OperationLease>> staging;
        auto reserve=[&](const std::wstring& name){staging.emplace_back(new VolumeCoord::OperationLease({{VolumeCoord::PathKey(name),VolumeCoord::Mode::Write}}));};
        try {
            staged = Temp(root);reserve(staged);resource = Temp(root);reserve(resource);
            {
                File data(staged, GENERIC_WRITE, TRUNCATE_EXISTING), rsrc(resource, GENERIC_WRITE, TRUNCATE_EXISTING);
                uint8_t header[82]{}; P32(header, 0x00051607); P32(header+4, 0x00020000); P16(header+24, 2);
                P32(header+26, 9); P32(header+30, 50); P32(header+34, 32);
                P32(header+38, 2); P32(header+42, 82); P32(header+46, uint32_t(e.forks[1].size));
                memcpy(header+50, e.finder.data(), 32); rsrc.Write(header, sizeof(header));
                std::vector<uint8_t> buffer(1024*1024);
                for (unsigned f=0; f<2; ++f) for (uint64_t at=0; at<e.forks[f].size;) {
                    size_t n=size_t((std::min)(uint64_t(buffer.size()),e.forks[f].size-at));
                    read(f,at,buffer.data(),n); (f ? rsrc : data).Write(buffer.data(),n); at+=n;
                }
                data.Dates(e);rsrc.Dates(e);data.Flush(); rsrc.Flush();
            }
            {std::lock_guard<std::recursive_mutex> lock(mutex_);for(const auto& entry:paths_)if(!_wcsicmp(entry.second.c_str(),path.c_str())){dataReaders_.erase(entry.first);resourceReaders_.erase(entry.first);}}
            if (existed) { backup=Temp(root);reserve(backup); Check(MoveFileExW(path.c_str(),backup.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot preserve old destination"); backed=true; }
            if (sidecar) { backupResource=Temp(root);reserve(backupResource); Check(MoveFileExW(companion.c_str(),backupResource.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot preserve old resource fork"); backedResource=true; }
            Check(MoveFileExW(staged.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot publish exported data"); staged.clear(); published=true;
            Check(MoveFileExW(resource.c_str(),companion.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot publish exported resource fork"); resource.clear(); publishedResource=true;
        } catch (...) {
            bool restored=true;
            if(publishedResource && !DeleteFileW(companion.c_str()))restored=false;
            if(published && !DeleteFileW(path.c_str()))restored=false;
            if(backedResource && !MoveFileExW(backupResource.c_str(),companion.c_str(),MOVEFILE_WRITE_THROUGH))restored=false;
            if(backed && !MoveFileExW(backup.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))restored=false;
            if(!staged.empty())DeleteFileW(staged.c_str()); if(!resource.empty())DeleteFileW(resource.c_str());
            if(!backed&&!backup.empty())DeleteFileW(backup.c_str()); if(!backedResource&&!backupResource.empty())DeleteFileW(backupResource.c_str());
            Check(restored,"Export rollback failed; old data is retained in destination hfv*.tmp backup files. Do not delete them."); throw;
        }
        if(!backup.empty())DeleteFileW(backup.c_str()); if(!backupResource.empty())DeleteFileW(backupResource.c_str()); return Id(path);
    }
    void Rename(uint32_t id, uint32_t parent, const std::u16string &name) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Check(id!=2,"Cannot rename a volume root");Leaf(name);auto entry=Describe(id);
        Check(parent==entry.parent,"Rename must stay in the same Windows folder");
        auto path=Path(id),target=Child(Path(parent),Wide(name));if(path==target)return;
        auto oldResource=Companion(path),newResource=Companion(target);Double data;
        bool resource=!entry.directory&&AppleDouble(oldResource,data);
        Check(!_wcsicmp(path.c_str(),target.c_str())||GetFileAttributesW(target.c_str())==INVALID_FILE_ATTRIBUTES,"Destination already exists");
        if(resource){Leaf(u"%"+name);Check(!_wcsicmp(oldResource.c_str(),newResource.c_str())||GetFileAttributesW(newResource.c_str())==INVALID_FILE_ATTRIBUTES,"An existing resource companion blocks rename; it was preserved");}
        dataReaders_.erase(id);resourceReaders_.erase(id);
        if(resource)Check(MoveFileExW(oldResource.c_str(),newResource.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot rename resource companion; source retained");
        if(!MoveFileExW(path.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)){
            if(resource)Check(MoveFileExW(newResource.c_str(),oldResource.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Rename rollback failed; data remains at the old name and the resource companion remains at the new name. Preserve both files.");
            throw HfsPlus::Error("Cannot rename the selected Windows entry; source retained");
        }
        paths_[id]=target;
    }
    void Remove(uint32_t id) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Check(id!=2,"Cannot remove a volume root"); auto path=Path(id); auto e=Describe(id);
        dataReaders_.erase(id);resourceReaders_.erase(id);
        if(e.directory)Check(RemoveDirectoryW(path.c_str())!=FALSE,"Cannot remove Windows folder");
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
    auto volume=std::make_shared<Windows>(parent);auto entry=volume->DescribeFile(actual);return {volume,entry.id};
}
Entry DescribeWindowsFile(const std::wstring& path){
    auto parent=path.substr(0,path.find_last_of(L"\\/"));if(parent.size()==2&&parent.back()==L':')parent+=L'\\';
    Windows volume(parent);return volume.DescribeFile(path);
}
std::shared_ptr<Volume> OpenWindows(const std::wstring &path) { return std::make_shared<Windows>(path); }
}
