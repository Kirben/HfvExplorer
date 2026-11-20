// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "OpticalReader.h"
#include "VolumeCoordinator.h"
#include "WorkerEndpoint.h"
#include "StorageHandoff.h"
#include <mutex>

namespace Optical {
void Reader::Contents(const Node& node,uint64_t offset,void* output,size_t bytes)
{
    Check(Range(offset,bytes,node.size)&&(!bytes||output),"Optical read exceeds the file length");
    if (!bytes) return;
    auto dest=static_cast<uint8_t*>(output);
    if (!node.embedded.empty()) {
        Check(Range(offset,bytes,node.embedded.size()),"Truncated embedded optical file");
        memcpy(dest,node.embedded.data()+size_t(offset),bytes);
        return;
    }
    for(const auto& extent:node.extents) {
        if(offset>=extent.bytes){offset-=extent.bytes;continue;}
        const auto chunk=size_t((std::min)(uint64_t(bytes),extent.bytes-offset));
        if(extent.sparse)memset(dest,0,chunk);
        else ReadExtent(extent,offset,dest,chunk);
        dest+=chunk;
        bytes-=chunk;
        offset=0;
        if(!bytes)return;
    }
    throw HfsPlus::Error("Incomplete optical file extents");
}

namespace {
struct Recognition { Bytes primary,joliet; bool udf=false; };
Recognition Probe(HfsPlus::Source& source)
{
    Recognition out;
    // The standard descriptor sequence starts at sector 16. Scan through an
    // ISO terminator as a UDF bridge can put its recognition sequence next.
    for(uint64_t lba=16;lba<256 && Range(lba*BlockSize,BlockSize,source.Size());++lba){
        const auto p=HfsPlus::Disk::Read(source,lba*BlockSize,7);
        if(!memcmp(p.data()+1,"CD001",5)&&p[6]==1) {
            if(p[0]==1&&out.primary.empty())
                out.primary=HfsPlus::Disk::Read(source,lba*BlockSize,BlockSize);
            if(p[0]==2) {
                auto desc=HfsPlus::Disk::Read(source,lba*BlockSize,BlockSize);
                if(desc[88]=='%'&&desc[89]=='/'&&(desc[90]=='@'||desc[90]=='C'||desc[90]=='E')&&
                   (out.joliet.empty()||desc[90]>out.joliet[90]))
                    out.joliet=std::move(desc);
            }
        } else if(!memcmp(p.data()+1,"NSR02",5)||!memcmp(p.data()+1,"NSR03",5)) {
            if(p[0]==0&&p[6]==1)out.udf=true;
        } else if(memcmp(p.data()+1,"BEA01",5)&&memcmp(p.data()+1,"BOOT2",5)) {
            break;
        }
    }
    return out;
}
bool GoodName(const std::u16string& name)
{
    return !name.empty()&&name!=u"."&&name!=u".."&&
        std::none_of(name.begin(),name.end(),[](char16_t ch){return !ch||ch==u'/'||ch==u'\\';});
}
uint16_t Both16(const uint8_t* p)
{
    Check(Le16(p)==HfsPlus::Disk::U16(p+2),"Inconsistent ISO 9660 16-bit field");
    return Le16(p);
}
uint32_t Both32(const uint8_t* p)
{
    Check(Le32(p)==HfsPlus::Disk::U32(p+4),"Inconsistent ISO 9660 32-bit field");
    return Le32(p);
}

class Iso final:public Reader {
    uint64_t blocks_=0;
    bool joliet_;
    Node root_;

    std::u16string Name(const uint8_t* p,size_t length)const {
        std::u16string name;
        Check(!joliet_||length%2==0,"Invalid Joliet Unicode name");
        for(size_t i=0;i<length;i+=joliet_?2:1)
            name.push_back(char16_t(joliet_?HfsPlus::Disk::U16(p+i):p[i]));
        return name;
    }
    Node Record(const uint8_t* p,size_t size)const {
        Check(size>=34&&p[32]&&p[32]<=size-33,"Truncated ISO 9660 directory record");
        Check(!p[26]&&!p[27]&&Both16(p+28)==1,"Unsupported interleaved or multi-volume ISO 9660 file");
        Node entry;
        const auto lba=uint64_t(Both32(p+2))+p[1];
        entry.size=Both32(p+10);
        Check(Range(lba*BlockSize,entry.size,blocks_*BlockSize),"ISO 9660 extent is outside the volume");
        entry.extents.push_back({lba,entry.size});
        entry.address=std::to_string(lba);
        entry.entry.directory=(p[25]&2)!=0;
        SYSTEMTIME time{};
        time.wYear=uint16_t(1900+p[18]);time.wMonth=p[19];time.wDay=p[20];
        time.wHour=p[21];time.wMinute=p[22];time.wSecond=p[23];
        const auto zone=int(int8_t(p[24]));
        if(zone>=-48&&zone<=52)
            if(const auto stamp=CatalogDate(time,zone*15)){
                entry.entry.datesValid=true;
                entry.entry.created=entry.entry.modified=*stamp;
            }
        return entry;
    }
    void ReadExtent(const Extent& extent,uint64_t at,void* dest,size_t bytes)override {
        Read(extent.block*BlockSize+at,dest,bytes);
    }
public:
    Iso(HfsPlus::Source& source,const Bytes& descriptor,bool joliet)
        :Reader(source),blocks_(Both32(descriptor.data()+80)),joliet_(joliet)
    {
        Check(Both16(descriptor.data()+128)==BlockSize&&blocks_&&blocks_<=source.Size()/BlockSize,
              "Invalid ISO 9660 volume geometry");
        const auto* record=descriptor.data()+156;
        root_=Record(record,record[0]);
        Check(root_.entry.directory&&root_.size,"ISO 9660 has no root directory");
        root_.entry.name=Name(descriptor.data()+40,32);
        while(!root_.entry.name.empty()&&(root_.entry.name.back()==u' '||!root_.entry.name.back()))
            root_.entry.name.pop_back();
        if(root_.entry.name.empty())root_.entry.name=u"ISO 9660";
    }
    const char* Kind()const override{return joliet_?"ISO 9660 / Joliet":"ISO 9660";}
    Node Root()const override{return root_;}
    std::vector<Node> List(const Node& folder)override {
        std::vector<Node> entries;
        std::u16string unfinished;
        std::array<uint8_t,BlockSize> block{};
        for(uint64_t at=0;at<folder.size;at+=BlockSize){
            const auto bytes=size_t((std::min)(BlockSize,folder.size-at));
            Contents(folder,at,block.data(),bytes);
            for(size_t pos=0;pos<bytes&&block[pos];){
                const auto p=block.data()+pos;
                const auto length=size_t(p[0]);
                Check(length>=34&&length<=bytes-pos&&p[32]&&p[32]<=length-33,
                      "Invalid ISO 9660 directory record length");
                pos+=length;
                if(p[32]==1&&p[33]<=1)continue; // self and parent
                if(p[25]&4)continue; // associated files are a separate namespace
                auto raw=Name(p+33,p[32]);
                auto item=Record(p,length);
                const bool continued=(p[25]&128)!=0;
                Check(!continued||!item.entry.directory,"ISO 9660 directory uses multiple extents");
                if(!unfinished.empty()){
                    Check(raw==unfinished&&!item.entry.directory,"Incomplete ISO 9660 multi-extent file");
                    auto& previous=entries.back();
                    Check(item.size<=UINT64_MAX-previous.size,"ISO 9660 file length overflow");
                    previous.size+=item.size;
                    previous.extents.push_back(item.extents.front());
                }else{
                    item.entry.name=raw;
                    if(!item.entry.directory && item.entry.name.size()>=2 &&
                       item.entry.name.substr(item.entry.name.size()-2)==u";1")
                        item.entry.name.resize(item.entry.name.size()-2);
                    if(!item.entry.directory&&!item.entry.name.empty()&&item.entry.name.back()==u'.')
                        item.entry.name.pop_back();
                    Check(GoodName(item.entry.name),"Invalid ISO 9660 entry name");
                    entries.push_back(std::move(item));
                }
                unfinished=continued?std::move(raw):std::u16string{};
            }
        }
        Check(unfinished.empty(),"Unterminated ISO 9660 multi-extent file");
        return entries;
    }
};

class OpticalVolume final:public MacBrowser::Volume,public MacBrowser::WorkerEndpoint {
    std::unique_ptr<HfsPlus::Source> source_;
    std::unique_ptr<Reader> reader_;
    std::string key_,kind_;
    std::u16string name_;
    MacBrowser::WorkerOrigin origin_;
    const bool processLocal_;
    mutable std::mutex mutex_;
    std::vector<Node> nodes_;
    std::map<uint32_t,std::vector<uint32_t>> listings_;

    const Node& At(uint32_t id)const {
        Check(id>=2&&id-2<nodes_.size(),"Unknown optical file or directory");
        return nodes_[id-2];
    }
    void CheckMedia() {
        Check(bool(source_),"The disc was released. Select the drive again.");
        source_->CheckMedia();
    }
    template<class Work>auto Access(Work work)->decltype(work()) {
        VolumeCoord::BackendRead lease(key_);
        std::lock_guard<std::mutex> lock(mutex_);
        CheckMedia();
        auto result=work();
        CheckMedia();
        return result;
    }
    [[noreturn]] static void ReadOnly() { throw HfsPlus::Error("Optical filesystems are read-only"); }
public:
    OpticalVolume(std::unique_ptr<HfsPlus::Source> source,std::unique_ptr<Reader> reader,std::string key)
        :source_(std::move(source)),reader_(std::move(reader)),key_(std::move(key)),kind_(reader_->Kind()),processLocal_(source_->ProcessLocal())
    {
        auto root=reader_->Root();
        root.entry.id=2;root.entry.parent=1;
        root.entry.identity=MacBrowser::EntryIdentity(key_+":"+root.address);
        name_=root.entry.name;
        nodes_.push_back(std::move(root));
        const auto files=source_->WorkerFiles();
        if(!files.empty()){
            origin_.kind=MacBrowser::WorkerOrigin::Image;
            origin_.path=files.front().first;
        }else if(source_->MediaDrive()>=0){
            origin_.kind=MacBrowser::WorkerOrigin::Cd;
            origin_.drive=source_->MediaDrive();
            uint32_t generation=0;
            origin_.generationKnown=source_->MediaGeneration(generation);
            origin_.generation=generation;
        }
    }
    MacBrowser::WorkerExecution Execution()const override{MacBrowser::WorkerExecution result;result.processLocal=processLocal_;return result;}
    MacBrowser::WorkerOrigin WorkerDescription()const override{return origin_;}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return source_?source_->WorkerFiles():std::vector<std::pair<std::wstring,uintptr_t>>{};
    }
    void ReleaseMedia()override {
        std::lock_guard<std::mutex> lock(mutex_);
        reader_.reset();source_.reset();
    }
    void ValidateMedia()override{Access([]{return true;});}
    int CdDrive()const override{return origin_.drive;}
    std::string ResourceKey()const override{return key_;}
    std::u16string Name()const override{return name_;}
    const char* Kind()const override{return kind_.c_str();}
    bool Writable()const override{return false;}
    bool CaseSensitive()const override{return true;}
    uint64_t FreeBytes()const override{return 0;}
    MacBrowser::Capabilities Supports()const override {
        auto capabilities=MacBrowser::Volume::Supports();
        capabilities.resourceForks=false;
        return capabilities;
    }
    bool FindChild(uint32_t parent,const std::u16string& name,MacBrowser::Entry& out)override {
        for(auto& entry:List(parent))if(entry.name==name){out=std::move(entry);return true;}
        return false;
    }
    std::vector<MacBrowser::Entry> List(uint32_t parent)override{
        return Access([&]{
            if(listings_.find(parent)==listings_.end()){
                const auto& folder=At(parent);
                Check(folder.entry.directory,"The optical entry is not a directory");
                auto children=reader_->List(folder);
                Check(children.size()<=UINT32_MAX-nodes_.size()-1,"Too many optical entries");
                std::set<std::u16string> names;
                std::vector<uint32_t> ids;
                ids.reserve(children.size());
                for(auto& child:children){
                    Check(GoodName(child.entry.name)&&names.insert(child.entry.name).second,
                          "Invalid or duplicate optical entry name");
                    if(child.entry.directory){
                        for(auto at=parent;at>=2;at=At(at).entry.parent)
                            Check(At(at).address!=child.address,"Cyclic optical directory");
                    }else child.entry.forks[0].size=child.size;
                    child.entry.id=uint32_t(nodes_.size()+ids.size()+2);
                    child.entry.parent=parent;
                    child.entry.identity=MacBrowser::EntryIdentity(folder.entry.identity.primary.key+"/"+child.address);
                    ids.push_back(child.entry.id);
                }
                nodes_.insert(nodes_.end(),std::make_move_iterator(children.begin()),std::make_move_iterator(children.end()));
                listings_.emplace(parent,std::move(ids));
            }
            std::vector<MacBrowser::Entry> entries;
            for(auto id:listings_.at(parent))entries.push_back(At(id).entry);
            return entries;
        });
    }
    MacBrowser::Entry Stat(uint32_t id)override{return Access([&]{return At(id).entry;});}
    void Read(uint32_t id,unsigned fork,uint64_t offset,void* p,size_t n)override{
        Access([&]{
            const auto& node=At(id);
            Check(!node.entry.directory&&fork<2&&(fork==0||(!offset&&!n)),"Invalid optical file fork");
            if(!node.entry.readError.empty())throw HfsPlus::Error(node.entry.readError);
            if(fork==0)reader_->Contents(node,offset,p,n);
            return true;
        });
    }
    uint32_t MakeDirectory(uint32_t,const std::u16string&)override{ReadOnly();}
    uint32_t Put(uint32_t,const MacBrowser::Entry&,MacBrowser::Reader,bool)override{ReadOnly();}
    void Rename(uint32_t,uint32_t,const std::u16string&)override{ReadOnly();}
    void Remove(uint32_t)override{ReadOnly();}
};
}

bool Recognizes(HfsPlus::Source& source)
{
    const auto probe=Probe(source);
    return probe.udf||!probe.primary.empty()||!probe.joliet.empty();
}

std::shared_ptr<MacBrowser::Volume> Open(std::unique_ptr<HfsPlus::Source>& source,const std::string& key)
{
    const auto probe=Probe(*source);
    std::unique_ptr<Reader> reader;
    if(probe.udf){
        try{reader=OpenUdf(*source);}
        catch(const HfsPlus::Error&){
            if(probe.joliet.empty()&&probe.primary.empty())throw;
        }
    }
    if(!reader&&!probe.joliet.empty())reader=std::make_unique<Iso>(*source,probe.joliet,true);
    if(!reader&&!probe.primary.empty())reader=std::make_unique<Iso>(*source,probe.primary,false);
    if(!reader)return {};
    return MacBrowser::TrackWorkerVolume(std::make_shared<OpticalVolume>(std::move(source),std::move(reader),key));
}
}
