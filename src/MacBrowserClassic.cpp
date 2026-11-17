// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include "LegacyEngine.h"
#include "HfsPlusDisk.h"
#include "charset.h"
#include <fcntl.h>
#include <set>
extern "C" {
#undef ERROR
#include "hfs/libhfs/internal.h"
#include "hfs/Replacement.h"
}
namespace MacBrowser {
namespace {
void Check(bool ok) { if (!ok) throw HfsPlus::Error(hfs_error ? hfs_error : "Classic HFS operation failed"); }
std::u16string Decode(const std::string &s) {
    int n = MultiByteToWideChar(10000, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(10000, 0, s.data(), int(s.size()), &w[0], n); return U16(w);
}
std::string Encode(const std::u16string &s) {
    BOOL lost = FALSE; auto w = Wide(s); char out[32];
    int n = WideCharToMultiByte(10000, WC_NO_BEST_FIT_CHARS, w.data(), int(w.size()), out, sizeof(out)-1, nullptr, &lost);
    if (!n || lost || n > HFS_MAX_FLEN || s.empty() || s.find(u':') != s.npos)
        throw HfsPlus::Error("This name cannot be represented in classic HFS (31 MacRoman bytes maximum)");
    return {out, size_t(n)};
}
struct RootScope {
 hfsvol* volume;long previous;
 explicit RootScope(hfsvol* v):volume(v),previous(hfs_getcwd(v)){Check(hfs_setcwd(v,HFS_CNID_ROOTDIR)==0);}
 ~RootScope(){hfs_setcwd(volume,previous);}
};
class Classic final : public Volume {
    std::shared_ptr<LegacyEngine::Context> context_;
    hfsvol *volume_ = nullptr;
    std::string image_;
    std::string resourceKey_;
    std::string imageIdentity_;
    bool write_;
    std::map<uint32_t,std::string> paths_;
    uint32_t next_ = 3;
    uint32_t Id(const std::string &p) { for (auto &e : paths_) if(e.second == p)return e.first; paths_[next_]=p;return next_++; }
    std::string Path(uint32_t id) { return paths_.at(id); }
    Entry Describe(const std::string &path, uint32_t id) {
        RootScope scope(volume_);hfsdirent in; Check(hfs_stat(volume_, const_cast<char*>(path.c_str()), &in) == 0); Entry e;
        e.id=id;if(id==2)e.parent=1;else{auto parent=path.substr(0,path.find_last_of(':'));if(parent.empty())parent=":";e.parent=Id(parent);}
        e.name=Decode(in.name);e.identity=imageIdentity_+":"+std::to_string(uint32_t(in.cnid));e.resolvedId=uint32_t(in.cnid);
        e.directory=(in.flags&HFS_ISDIR)!=0;e.protectedFile=(in.flags&HFS_ISLOCKED)!=0;
        e.created=uint32_t(int64_t(in.crdate)+2082844800LL);e.modified=uint32_t(int64_t(in.mddate)+2082844800LL);e.datesValid=true;
        if(!e.directory){e.forks[0].size=in.u.file.dsize;e.forks[1].size=in.u.file.rsize;
            memcpy(e.finder.data(),in.u.file.type,4);memcpy(e.finder.data()+4,in.u.file.creator,4);}
        Check(hfs_getfinderinfo(volume_,const_cast<char*>(path.c_str()),e.finder.data())==0);return e;
    }
public:
    Classic(const std::string &image, std::string root, bool write) : write_(write),image_(image) {
        resourceKey_=VolumeCoord::ImageKey(image);context_=LegacyEngine::ForVolume(resourceKey_);LegacyEngine::Scope engine(context_);
        imageIdentity_=image;HANDLE identity=CreateFileA(image.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        if(identity!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION info{};if(GetFileInformationByHandle(identity,&info))imageIdentity_=std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);CloseHandle(identity);}
        std::vector<char> converted(root.begin(),root.end());converted.push_back(0);pc_to_mac_charset((unsigned char*)converted.data());root=converted.data();
        volume_=hfs_mount(const_cast<char*>(image.c_str()),1,write?O_RDWR:O_RDONLY);Check(volume_!=nullptr);
        auto colon=root.find(':');root=colon==root.npos?":":root.substr(colon);
        while(root.size()>1&&root.back()==':'&&root.find(':')<root.size()-1)root.pop_back();paths_[2]=root;
        try {Check(Describe(root,2).directory);}catch(...){hfs_umount(volume_);volume_=nullptr;throw;}
    }
    ~Classic(){LegacyEngine::Scope engine(context_);if(volume_)hfs_umount(volume_);}
    void Close()override{LegacyEngine::Scope engine(context_);if(volume_){auto v=volume_;volume_=nullptr;Check(hfs_umount(v)==0);}}
    std::string ResourceKey()const override{return resourceKey_;}
    std::u16string Name()const override{LegacyEngine::Scope engine(context_);return Decode(paths_.at(2));}
    const char *Kind()const override{return "HFS";}
    bool Writable()const override{LegacyEngine::Scope engine(context_);return write_&&volume_&&!(volume_->flags&HFS_READONLY);}
    bool CaseSensitive()const override{return false;}
    void ValidateEntry(const Entry& e)const override{
        Volume::ValidateEntry(e);if(e.datesValid&&(e.created>UINT32_MAX||e.modified>UINT32_MAX))throw HfsPlus::Error("Classic HFS cannot preserve dates after February 2040");Encode(e.name);if(e.forks[0].size>HFS_MAX_FORK_SIZE||e.forks[1].size>HFS_MAX_FORK_SIZE)
            throw HfsPlus::Error("Fork exceeds the classic HFS 2 GiB minus one byte limit");
    }
    uint64_t FreeBytes()const override{LegacyEngine::Scope engine(context_);hfsvolent e;Check(hfs_vstat(volume_,&e)==0);return e.freebytes;}
    Entry Stat(uint32_t id)override{LegacyEngine::Scope engine(context_);return Describe(Path(id),id);}
    void ValidateCopyTo(uint32_t id,Volume& target,uint32_t parent)override {LegacyEngine::Scope engine(context_);
        auto dst=dynamic_cast<Classic*>(&target);if(!dst)return;RootScope sourceScope(volume_),targetScope(dst->volume_);
        bool same=!_stricmp(image_.c_str(),dst->image_.c_str());
        if(!same){
            HANDLE a=CreateFileA(image_.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            HANDLE b=CreateFileA(dst->image_.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            BY_HANDLE_FILE_INFORMATION x{},y{};
            if(a!=INVALID_HANDLE_VALUE&&b!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(a,&x)&&GetFileInformationByHandle(b,&y))
                same=x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
            if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);
        }
        if(!same)return;
        auto source=Path(id),directory=dst->Path(parent);hfsdirent from,to;
        Check(hfs_stat(volume_,const_cast<char*>(source.c_str()),&from)==0&&hfs_stat(dst->volume_,const_cast<char*>(directory.c_str()),&to)==0);
        if(from.flags&HFS_ISDIR){long at=to.cnid;std::set<long> visited;
            for(;;){if(at==from.cnid||!visited.insert(at).second)throw HfsPlus::Error("Cannot copy a classic HFS folder into itself or its descendants");
                if(at==HFS_CNID_ROOTDIR)break;char name[32];Check(hfs_dirinfo(dst->volume_,&at,name)==0);}
        }
        auto name=directory+(directory.back()==':'?"":":")+from.name;
        if(hfs_stat(dst->volume_,const_cast<char*>(name.c_str()),&to)==0&&from.cnid==to.cnid)
            throw HfsPlus::Error("Source and destination are the same classic HFS entry");
    }
    std::vector<Entry> List(uint32_t parent)override {LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);auto path=Path(parent);if(path.back()!=':')path+=":";hfsdir* dir=hfs_opendir(volume_,const_cast<char*>(path.c_str()));Check(dir!=nullptr);
        struct Close{hfsdir* p;~Close(){hfs_closedir(p);}}close{dir};hfsdirent e;std::vector<Entry> out;
        while(hfs_readdir(dir,&e)==0){auto p=path+e.name;auto item=Describe(p,Id(p));item.parent=parent;out.push_back(item);}
        if(errno!=ENOENT)Check(false);return out;
    }
    void Read(uint32_t id,unsigned fork,uint64_t at,void* p,size_t n)override {LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);auto path=Path(id);hfsfile* f=hfs_open(volume_,const_cast<char*>(path.c_str()));Check(f!=nullptr);
        struct Close{hfsfile* p;~Close(){hfs_close(p);}}close{f};Check(hfs_setfork(f,fork)==0&&hfs_lseek(f,at,SEEK_SET)==at);
        Check(n<=LONG_MAX&&hfs_read(f,p,(unsigned long)n)==n);
    }
    uint32_t MakeDirectory(uint32_t parent,const std::u16string& name)override {LegacyEngine::Scope engine(context_);
        if(!write_)throw HfsPlus::Error("Classic source is read-only");RootScope scope(volume_);auto path=(Path(parent)+(Path(parent).back()==':'?"":":"))+Encode(name);Check(hfs_mkdir(volume_,const_cast<char*>(path.c_str()))==0);return Id(path);
    }
    void FinalizeDirectory(uint32_t id,const Entry& desired,const Entry& expected,bool merge)override{LegacyEngine::Scope engine(context_);RootScope scope(volume_);
        if(!Writable())throw HfsPlus::Error("Classic destination is read-only");if(!merge)ValidateEntry(desired);auto path=Path(id);
        time_t dates[2]={time_t(int64_t(desired.created)-2082844800LL),time_t(int64_t(desired.modified)-2082844800LL)};
        Check(hfs_setdirmetadata(volume_,const_cast<char*>(path.c_str()),long(expected.resolvedId),merge?nullptr:desired.finder.data(),desired.datesValid?dates:nullptr)==0);
    }
    uint32_t Put(uint32_t parent,const Entry& e,Reader read,bool replace)override {LegacyEngine::Scope engine(context_);
        if(!write_)throw HfsPlus::Error("Classic source is read-only");ValidateEntry(e);RootScope scope(volume_);
        if(e.forks[0].size>HFS_MAX_FORK_SIZE||e.forks[1].size>HFS_MAX_FORK_SIZE)throw HfsPlus::Error("Fork exceeds the classic HFS 2 GiB minus one byte limit");
        auto path=(Path(parent)+(Path(parent).back()==':'?"":":"))+Encode(e.name);hfsdirent old;bool exists=hfs_stat(volume_,const_cast<char*>(path.c_str()),&old)==0;
        if(exists&&!replace)throw HfsPlus::Error("Destination already exists");
        char type[5]{},creator[5]{};memcpy(type,e.finder.data(),4);memcpy(creator,e.finder.data()+4,4);
        HfsReplacement state{};auto file=replacement_open(&state,volume_,const_cast<char*>(path.c_str()),type,creator);Check(file!=nullptr);
        int result=0;char* error=nullptr;std::exception_ptr failure;
        try {
            std::vector<uint8_t> bytes(1024*1024);
            for(unsigned fork=0;fork<2;++fork){Check(hfs_setfork(file,fork)==0);
                for(uint64_t at=0;at<e.forks[fork].size;){size_t n=size_t((std::min)(uint64_t(bytes.size()),e.forks[fork].size-at));
                    read(fork,at,bytes.data(),n);Check(hfs_write(file,bytes.data(),(unsigned long)n)==n);at+=n;}}
            hfsdirent meta;Check(hfs_fstat(file,&meta)==0);meta.fdflags=HfsPlus::Disk::U16(e.finder.data()+8);
            if(e.datesValid){meta.crdate=time_t(int64_t(e.created)-2082844800LL);meta.mddate=time_t(int64_t(e.modified)-2082844800LL);}
            Check(hfs_fsetattr(file,&meta)==0);Check(hfs_fsetfinderinfo(file,e.finder.data())==0);
        }catch(...){result=-1;failure=std::current_exception();}
        if(hfs_close(file)<0){result=-1;error=hfs_error;}
        replacement_finish(&state,&result,&error);
        if(result){if(!exists)hfs_delete(volume_,const_cast<char*>(path.c_str()));if(failure)std::rethrow_exception(failure);throw HfsPlus::Error(error?error:"Classic HFS copy failed");}
        Check(hfs_flush(volume_)==0);return Id(path);
    }
    bool CanMoveFrom(const Volume& source)const override{return dynamic_cast<const Classic*>(&source)&&source.ResourceKey()==ResourceKey();}
    void MoveHere(Volume& source,uint32_t id,uint32_t parent,const Entry& entry)override{LegacyEngine::Scope engine(context_);
        auto from=dynamic_cast<Classic*>(&source);Check(from&&CanMoveFrom(source)&&write_);RootScope scope(volume_);
        auto old=from->Path(id),path=Path(parent);if(path.back()!=':')path+=':';path+=Encode(entry.name);
        Check(hfs_rename(volume_,const_cast<char*>(old.c_str()),const_cast<char*>(path.c_str()))==0);
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{LegacyEngine::Scope engine(context_);
        RootScope scope(volume_);auto from=Path(id),to=(Path(parent)+(Path(parent).back()==':'?"":":"))+Encode(name);Check(write_&&hfs_rename(volume_,const_cast<char*>(from.c_str()),const_cast<char*>(to.c_str()))==0);paths_[id]=to;
    }
    void Remove(uint32_t id)override{LegacyEngine::Scope engine(context_);RootScope scope(volume_);auto path=Path(id);Check(write_&&id!=2);auto e=Stat(id);Check((e.directory?hfs_rmdir(volume_,const_cast<char*>(path.c_str())):hfs_delete(volume_,const_cast<char*>(path.c_str())))==0);}
};
}
std::shared_ptr<Volume> OpenClassic(const std::string& image,const std::string& path,bool write){return std::make_shared<Classic>(image,path,write);}
}
