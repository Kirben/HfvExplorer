// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsVolume.h"
#include "ApfsCatalogWrite.h"
#include "ApfsUnicode.h"
#include "StorageWorker.h"
#include "ApfsTree.h"
#include "ApfsCrypto.h"
#include "HfsPlusDisk.h"
#include "HfsPlusCompression.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include "WorkerEndpoint.h"
#include <algorithm>
#include <atomic>
#include <mutex>

namespace Apfs {
namespace {
using namespace HfsPlus;
using Disk::Check;
using Disk::Range;
using MacBrowser::Entry;
constexpr uint64_t IdMask=(uint64_t(1)<<60)-1;
constexpr uint64_t ExtentLengthMask=(uint64_t(1)<<56)-1;

std::u16string Name(const uint8_t* bytes,size_t size)
{
    Check(size>1&&bytes[size-1]==0&&!memchr(bytes,0,size-1),"Invalid APFS name length");
    const auto p=reinterpret_cast<const char*>(bytes);
    const auto len=int(size-1);
    const int chars=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,p,len,nullptr,0);
    Check(chars>0,"Invalid UTF-8 APFS name");
    std::wstring out(chars,L'\0');
    Check(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,p,len,out.data(),chars)==chars,
          "Invalid UTF-8 APFS name");
    return MacBrowser::U16(out);
}
uint64_t Date(const uint8_t* p)
{
    return uint64_t(int64_t(Le64(p))/1000000000+2082844800);
}
struct Mounted {
    uint64_t oid,xid,map,tree;
    bool sensitive,hashedNames,locked,softwareEncryption;
    std::u16string name;
    std::array<uint8_t,16> uuid;
    std::shared_ptr<const VolumeCipher> cipher;
    uint64_t mapTransaction=0,snapshotTree=0;
    std::optional<uint64_t> snapshotCreated;
};

Mounted ReadVolume(const Bytes& block,uint64_t oid,uint64_t liveMap=0)
{
    const auto p=block.data();
    Check(!memcmp(p+32,"APSB",4),"Missing APFS volume signature");
    const auto incompatible=Le64(p+56);
    const bool locked=!(Le64(p+264)&1);
    const auto map=liveMap?liveMap:Le64(p+128);
    if(!locked){
        Check(!(incompatible&~uint64_t(0x0b)),"Unsupported APFS volume features");
        Check(Le32(p+116)==2&&map&&Le64(p+136),"Unsupported APFS filesystem tree");
    }
    constexpr size_t nameOffset=704,nameLimit=256;
    const auto end=static_cast<const uint8_t*>(memchr(p+nameOffset,0,nameLimit));
    Check(end!=nullptr,"Unterminated APFS volume name");
    const bool software=(Le64(p+264)&8)&&!Le64(p+976)&&
        !(incompatible&~uint64_t(0x0f))&&Le32(p+116)==2&&Le64(p+128)&&Le64(p+136);
    Mounted mount{oid,Le64(p+16),map,Le64(p+136),!(incompatible&1),bool(incompatible&9),
                  locked,software,Name(p+nameOffset,size_t(end-(p+nameOffset))+1),{},{},0,0,{}};
    memcpy(mount.uuid.data(),p+240,mount.uuid.size());
    if(Le32(p+124)==0x40000002&&Le64(p+216))mount.snapshotTree=Le64(p+152);
    return mount;
}

struct Snapshot {
    uint64_t xid,superblock,created;
    std::u16string name;
};

void ReadSnapshotRecords(Source& source,const Container& container,const Mounted& live,
                         uint64_t block,std::optional<unsigned> parentLevel,TreeLayout layout,
                         std::set<uint64_t>& visited,std::vector<Snapshot>& snapshots)
{
    const bool root=!parentLevel;
    Check(visited.insert(block).second,"Repeated APFS snapshot tree node");
    const Node node(ReadObject(source,container,block,block,live.xid,root?2:3,16,true),root,layout);
    Check(node.Level()<64&&(root||node.Level()+1==*parentLevel),"Invalid APFS snapshot tree level");
    for(size_t i=0;i<node.Count();++i){
        const auto record=node.At(i);
        Check(record.key.size>=8,"Truncated APFS snapshot key");
        if(node.Level()){
            Check(record.value.size==8,"Invalid APFS snapshot tree child");
            ReadSnapshotRecords(source,container,live,Le64(record.value.data),node.Level(),node.Layout(),
                                visited,snapshots);
            continue;
        }
        const auto key=Le64(record.key.data);
        if(key>>60!=1)continue; // Snapshot-name index entries refer back to these metadata records.
        const auto xid=key&IdMask;
        const auto value=record.value.data;
        Check(record.key.size==8&&record.value.size>=50&&xid&&xid<=live.xid,"Invalid APFS snapshot record");
        const auto nameLength=Le16(value+48);
        Check(record.value.size==50+size_t(nameLength),"Invalid APFS snapshot name length");
        if(Le32(value+44))continue; // Dataless or in-progress snapshots cannot supply a full view.
        auto name=Name(value+50,nameLength);
        Check(name.find(u'/')==std::u16string::npos,"Invalid APFS snapshot name");
        snapshots.push_back({xid,Le64(value+8),Date(value+16),std::move(name)});
    }
}

std::vector<Mounted> ReadSnapshots(Source& source,const Container& container,const Mounted& live)
{
    std::vector<Mounted> views;
    if(live.locked||!live.snapshotTree)return views;
    std::set<uint64_t> visited;
    std::vector<Snapshot> snapshots;
    ReadSnapshotRecords(source,container,live,live.snapshotTree,{},TreeLayout{},visited,snapshots);
    std::set<uint64_t> transactions;
    for(const auto& snapshot:snapshots){
        if(!transactions.insert(snapshot.xid).second)continue;
        try{
            const auto bytes=ReadObject(source,container,snapshot.superblock,snapshot.superblock,snapshot.xid,13,0,true);
            auto view=ReadVolume(bytes,live.oid,live.map);
            Check(!view.locked&&view.xid==snapshot.xid&&view.uuid==live.uuid,"Invalid APFS snapshot superblock");
            view.mapTransaction=live.xid;
            view.snapshotCreated=snapshot.created;
            view.name=live.name+u" [Snapshot: "+snapshot.name+u"]";
            views.push_back(std::move(view));
        }catch(const Error&){
            // An unavailable historical view must not prevent opening the live volume.
        }
    }
    return views;
}

class Catalog {
    Source& source;
    const Container& container;
    const Mounted& volume;
    ObjectMap map;
    using Visit=std::function<void(Record)>;
    void Scan(uint64_t oid,unsigned level,bool root,TreeLayout layout,uint64_t wanted,
              std::set<uint64_t>& seen,const Visit& visit)const
    {
        Check(seen.insert(oid).second,"Repeated APFS filesystem node");
        const auto mapping=map.Find(oid);
        Check(mapping.has_value(),"Missing APFS filesystem node");
        Check(!mapping->encrypted||volume.cipher,"Missing APFS filesystem decryption key");
        const Node node(ReadObject(source,container,mapping->block,oid,volume.xid,root?2:3,14,false,
                                   mapping->encrypted?volume.cipher.get():nullptr),root,layout);
        Check(node.Level()<64&&(root||node.Level()==level),"Invalid APFS filesystem tree level");
        uint64_t previous=0,child=0;
        for(size_t i=0;i<node.Count();++i){
            const auto record=node.At(i);
            Check(record.key.size>=8,"Truncated APFS filesystem key");
            const auto raw=Le64(record.key.data),current=raw&IdMask;
            Check(current>=previous,"Unordered APFS filesystem keys");
            if(node.Level()){
                Check(record.value.size==8,"Invalid APFS child record");
                if(child&&previous<=wanted&&wanted<=current)
                    Scan(child,node.Level()-1,false,node.Layout(),wanted,seen,visit);
                child=Le64(record.value.data);
                Check(child,"Missing APFS filesystem child identifier");
            }else if(current==wanted)visit(record);
            previous=current;
        }
        if(child&&previous<=wanted)
            Scan(child,node.Level()-1,false,node.Layout(),wanted,seen,visit);
    }
public:
    Catalog(Source& s,const Container& c,const Mounted& v)
        :source(s),container(c),volume(v),map(s,c,v.map,v.xid,v.mapTransaction){}
    void VisitRecords(uint64_t oid,unsigned type,const Visit& visit)const
    {
        std::set<uint64_t> seen;
        Scan(volume.tree,0,true,{},oid,seen,[&](Record record){
            if(Le64(record.key.data)>>60==type)visit(record);
        });
    }
};


constexpr auto ResourceFork=u"com.apple.ResourceFork";
constexpr auto FinderInfo=u"com.apple.FinderInfo";
constexpr auto CompressionAttribute=u"com.apple.decmpfs";
constexpr auto SymlinkAttribute=u"com.apple.fs.symlink";
constexpr auto LockedMessage="This APFS volume is encrypted and locked";
struct Attribute {
    uint64_t size=0,stream=0;
    Bytes embedded;
};
using AttributeRecords=std::map<std::u16string,Attribute>;
AttributeRecords ReadAttributes(const Catalog& catalog,uint64_t oid)
{
    AttributeRecords attributes;
    catalog.VisitRecords(oid,4,[&](Record record){
        Check(record.key.size>=10&&record.value.size>=4,"Truncated APFS attribute");
        const auto length=Le16(record.key.data+8);
        Check(length==record.key.size-10,"Invalid APFS attribute name length");
        auto name=Name(record.key.data+10,length);
        const auto p=record.value.data;
        const auto flags=Le16(p),size=Le16(p+2);
        Check(size==record.value.size-4&&!(flags&~7u),"Invalid APFS attribute value");
        Attribute attribute;
        switch(flags&3){
        case 1:
            Check(size==48&&Le64(p+4),"Invalid APFS attribute data stream");
            attribute.stream=Le64(p+4);
            attribute.size=Le64(p+12);
            break;
        case 2:
            attribute.size=size;
            attribute.embedded.assign(p+4,p+4+size);
            break;
        default: throw Error("Unsupported APFS attribute storage");
        }
        Check(attributes.emplace(std::move(name),std::move(attribute)).second,"Duplicate APFS attribute");
    });
    return attributes;
}
struct Inode {
    uint64_t stream=0;
    Entry entry;
    AttributeRecords attributes;
    bool symlink=false;
};
Inode ReadInode(const Catalog& catalog,uint64_t oid)
{
    Inode out;
    bool found=false;
    catalog.VisitRecords(oid,3,[&](Record record){
        const auto p=record.value.data;
        constexpr size_t inodeSize=92;
        Check(!found&&record.key.size==8&&record.value.size>=inodeSize,"Invalid APFS inode record");
        found=true;
        out.stream=Le64(p+8);
        auto& entry=out.entry;
        entry.created=Date(p+16);entry.modified=Date(p+24);entry.datesValid=true;
        const auto mode=Le16(p+80)&0170000;
        entry.directory=mode==0040000;
        entry.linkCount=entry.directory?1:Le32(p+56);
        entry.hardLink=entry.linkCount>1;
        entry.compressed=(Le32(p+68)&0x20)!=0;
        out.symlink=mode==0120000;
        if(out.symlink)entry.readError="APFS symbolic links cannot be exported";
        else if(!entry.directory&&mode!=0100000)entry.readError="APFS special files are unsupported";
        if(record.value.size==inodeSize)return;
        const auto fields=p+inodeSize;
        Check(record.value.size>=inodeSize+4,"Truncated APFS inode extended-field header");
        const auto count=Le16(fields),used=Le16(fields+2);
        const size_t first=inodeSize+4+size_t(count)*4;
        Check(first<=record.value.size&&used<=record.value.size-inodeSize-4,"Invalid APFS inode extended fields");
        size_t at=first;
        const size_t end=record.value.size;
        bool hasStream=false;
        for(unsigned i=0;i<count;++i){
            const auto field=fields+4+i*4;
            const size_t size=Le16(field+2),aligned=(size+7)&~size_t(7);
            Check(Range(at,aligned,end),"Truncated APFS inode extended field");
            if(field[0]==8){
                Check(size==40&&!hasStream,"Invalid APFS inode data stream");
                entry.forks[0].size=Le64(p+at);hasStream=true;
            }else if(field[0]==7){
                Check(size==entry.finder.size(),"Invalid APFS Finder information");
                memcpy(entry.finder.data(),p+at,size);
            }
            at+=aligned;
        }
        Check(used==at-first||used==at-inodeSize-4,"Invalid APFS extended-field size");
    });
    Check(found,"Missing APFS file inode");
    out.attributes=ReadAttributes(catalog,oid);
    if(auto found=out.attributes.find(FinderInfo);found!=out.attributes.end()){
        const auto& value=found->second;
        Check(!value.stream&&value.size==out.entry.finder.size(),"Invalid APFS Finder attribute");
        memcpy(out.entry.finder.data(),value.embedded.data(),value.embedded.size());
    }
    if(auto found=out.attributes.find(ResourceFork);found!=out.attributes.end()){
        Check(!out.entry.directory,"APFS directory unexpectedly contains a resource fork");
        out.entry.forks[1].size=found->second.size;
    }
    return out;
}

struct Extent {uint64_t offset,length,block,cryptoId;};
std::vector<Extent> Extents(const Catalog& catalog,const Container& container,uint64_t stream,bool decrypted)
{
    std::vector<Extent> out;
    catalog.VisitRecords(stream,8,[&](Record record){
        Check(record.key.size==16&&record.value.size==24,"Invalid APFS file extent");
        const auto offset=Le64(record.key.data+8),encoded=Le64(record.value.data);
        const auto length=encoded&ExtentLengthMask,block=Le64(record.value.data+8);
        const auto flags=encoded>>56;
        // macOS marks software-encrypted runs with bit 0 in the extent flags.
        Check((flags==0||(flags==1&&decrypted))&&length&&offset<=UINT64_MAX-length&&
              !(offset%container.blockSize)&&!(length%container.blockSize)&&
              Range(block,length/container.blockSize,container.blockCount)&&
              (out.empty()||out.back().offset+out.back().length<=offset),
              "Overlapping or unsupported APFS file extent");
        out.push_back({offset,length,block,Le64(record.value.data+16)});
    });
    return out;
}

class Volume final:public MacBrowser::Volume,public MacBrowser::WorkerEndpoint {
    std::unique_ptr<Source> source;
    Container container;
    std::vector<Mounted> mounts;
    std::u16string label;
    bool caseSensitive=true;
    const std::string key;
    const bool writable,processLocal;
    std::atomic<uint64_t> freeBytes;
    MacBrowser::WorkerOrigin origin;
    std::recursive_mutex mutex;
    bool requiresReopen=false,writing=false;
    std::map<uint64_t,uint32_t> writableIds;
    struct Item {
        size_t mount;
        uint64_t oid,stream;
        Entry entry;
        AttributeRecords attributes;
        bool symlink=false;
    };
    using Address=std::pair<size_t,uint64_t>;
    std::map<uint32_t,Item> entries;
    std::map<Address,uint32_t> linkIdentities;
    std::map<uint32_t,std::vector<uint32_t>> listings;
    std::map<Address,std::vector<Extent>> extents;
    std::map<Address,std::unique_ptr<HfsPlus::Compression>> compressed;
    uint64_t next=3;

    Item& At(uint32_t id){
        const auto found=entries.find(id);
        Check(found!=entries.end(),"Unknown APFS entry");
        return found->second;
    }
    template<class Work>auto Access(Work work)->decltype(work()){
        VolumeCoord::BackendRead lease(key);
        std::lock_guard<std::recursive_mutex> lock(mutex);
        Check(!requiresReopen,"Reopen the APFS image before continuing");
        source->CheckMedia();
        auto result=work();
        source->CheckMedia();
        return result;
    }
    Inode LoadInode(const Catalog& catalog,size_t mount,uint64_t oid){
        auto inode=ReadInode(catalog,oid);
        if(inode.symlink)return inode;
        const auto found=inode.attributes.find(CompressionAttribute);
        if(!inode.entry.compressed&&found==inode.attributes.end())return inode;
        inode.entry.compressed=true;
        try{
            Check(!inode.entry.directory&&found!=inode.attributes.end()&&found->second.size>=16,
                  "Missing or unsupported APFS compression attribute");
            Bytes header(16);
            ReadAttribute(mount,found->second,0,header.data(),header.size());
            const HfsPlus::Compression descriptor(std::move(header),inode.entry.forks[1].size,{});
            inode.entry.forks[0].size=descriptor.Size();
            if(descriptor.UsesResource()){
                Check(found->second.size==16&&inode.entry.forks[1].size>0,"Invalid APFS compression resource");
                inode.entry.forks[1].size=0;
            }
        }catch(const Error& error){inode.entry.readError=error.what();}
        return inode;
    }
    std::string MountKey(const Mounted& mount)const{
        auto identity=key+":"+std::to_string(mount.oid);
        if(mount.snapshotCreated)identity+=":snapshot:"+std::to_string(mount.xid);
        return identity;
    }
    [[noreturn]] static void ReadOnly(){throw Error("APFS is read-only");}
public:
    Volume(std::unique_ptr<Source> s,Container c,std::vector<Mounted> volumes,std::string resource,bool write,uint64_t free)
        :source(std::move(s)),container(std::move(c)),mounts(std::move(volumes)),key(std::move(resource)),
         writable(write),processLocal(write||source->ProcessLocal()||std::any_of(mounts.begin(),mounts.end(),[](const auto& mount){
             return mount.locked;
         })),freeBytes(free)
    {
        std::vector<Item> roots;
        std::exception_ptr failure;
        for(size_t i=0;i<mounts.size();++i){
            const auto& mount=mounts[i];
            try{
                Inode root;
                if(mount.locked){
                    root.entry.directory=true;
                    root.entry.readError=LockedMessage;
                }else{
                    root=LoadInode(Catalog(*source,container,mount),i,2);
                }
                Check(root.entry.directory,"APFS root is not a directory");
                root.entry.name=mount.name;
                root.entry.identity=MacBrowser::EntryIdentity(MountKey(mount),EntryVersion(mount));
                if(mount.snapshotCreated){
                    root.entry.created=root.entry.modified=*mount.snapshotCreated;
                    root.entry.datesValid=true;
                }
                roots.push_back({i,2,root.stream,std::move(root.entry),std::move(root.attributes)});
            }catch(const Error&){if(!failure)failure=std::current_exception();}
        }
        if(roots.empty())std::rethrow_exception(failure);
        if(roots.size()==1&&!mounts[roots.front().mount].locked){
            auto& root=roots.front();
            label=root.entry.name;
            caseSensitive=mounts[root.mount].sensitive;
            root.entry.id=root.entry.resolvedId=2;root.entry.parent=1;
            entries.emplace(2,std::move(root));
        }else{
            label=u"APFS Container";
            Entry root;
            root.id=root.resolvedId=2;root.parent=1;root.directory=true;root.name=label;root.identity=MacBrowser::EntryIdentity(key);
            entries.emplace(2,Item{0,0,0,std::move(root),{}});
            std::vector<uint32_t> ids;
            std::set<std::u16string> names;
            for(auto& item:roots){
                const auto name=item.entry.name;
                for(unsigned suffix=2;!names.insert(item.entry.name).second;++suffix){
                    const auto number=std::to_string(suffix);
                    item.entry.name=name+u" ("+std::u16string(number.begin(),number.end())+u")";
                }
                const auto id=uint32_t(next++);
                item.entry.id=item.entry.resolvedId=id;item.entry.parent=2;
                ids.push_back(id);
                entries.emplace(id,std::move(item));
            }
            listings.emplace(2,std::move(ids));
        }
        const auto files=source->WorkerFiles();
        if(!files.empty()){
            origin.kind=MacBrowser::WorkerOrigin::Image;
            origin.path=files.front().first;
        }
    }
    std::u16string Name()const override{return label;}
    std::string ResourceKey()const override{return key;}
    const char* Kind()const override{return "APFS";}
    bool CaseSensitive()const override{return caseSensitive;}
    bool Writable()const override{return writable;}
    uint64_t FreeBytes()const override{return freeBytes.load();}
    MacBrowser::Capabilities Supports() const override {
        auto capabilities = MacBrowser::Volume::Supports();
        capabilities.resourceForks=!writable;
        capabilities.createFolders=writable;
        capabilities.unicodeNames=true;
        capabilities.attributes=!writable;
        capabilities.basicMetadata=writable;
        return capabilities;
    }
    MacBrowser::WorkerExecution Execution()const override{
        MacBrowser::WorkerExecution out;out.processLocal=processLocal;return out;
    }
    MacBrowser::WorkerOrigin WorkerDescription()const override{return origin;}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{
        return processLocal?std::vector<std::pair<std::wstring,uintptr_t>>{}:source->WorkerFiles();
    }
    void ValidateMedia()override{Access([]{return true;});}
    void Unlock(uint32_t id,const Dmg::Password& password){
        Access([&]{
            auto& item=At(id);
            Check(item.oid==2,"Select an encrypted APFS volume");
            auto& mount=mounts[item.mount];
            if(!mount.locked)return true; // Another window may have unlocked the shared mount.
            Check(mount.softwareEncryption,"This APFS volume does not support software unlocking");
            const ObjectMap map(*source,container,mount.map,mount.xid);
            Check(!map.ChangingEncryption(),"APFS volume encryption is changing");
            auto unlocked=mount;
            unlocked.cipher=UnlockSoftwareVolume(*source,container,mount.uuid,password);
            if(!unlocked.cipher)throw LockedVolume(true,PasswordHint(*source,container,mount.uuid));
            auto root=ReadInode(Catalog(*source,container,unlocked),2);
            Check(root.entry.directory,"Invalid decrypted APFS root");
            root.entry.id=item.entry.id;
            root.entry.resolvedId=item.entry.resolvedId;
            root.entry.parent=item.entry.parent;
            root.entry.name=item.entry.name;
            root.entry.identity=item.entry.identity;
            source->CheckMedia();
            item.entry=std::move(root.entry);
            item.attributes=std::move(root.attributes);
            item.stream=root.stream;
            mount.cipher=std::move(unlocked.cipher);
            mount.locked=false;
            return true;
        });
    }
    std::vector<Entry> List(uint32_t parent)override{
        return Access([&]{return ListDirectory(parent);});
    }
private:
    std::optional<uint64_t> EntryVersion(const Mounted& mount)const{
        if(writable)return {};
        return mount.xid;
    }
    std::vector<Entry> ListDirectory(uint32_t parent){
        auto& folder=At(parent);
        Check(folder.entry.directory,"The APFS entry is not a directory");
        if(!listings.count(parent)){
            std::vector<Item> children;
            std::set<std::u16string> names;
            const auto& mounted=mounts[folder.mount];
            if(mounted.locked){
                Check(mounted.softwareEncryption,"This APFS volume does not support software unlocking");
                throw LockedVolume(false,PasswordHint(*source,container,mounted.uuid));
            }
            const Catalog catalog(*source,container,mounted);
            catalog.VisitRecords(folder.oid,9,[&](Record record){
                const size_t header=mounted.hashedNames?12:10;
                Check(record.key.size>=header&&record.value.size>=18,"Truncated APFS directory record");
                const auto nameLength=mounted.hashedNames?Le32(record.key.data+8)&0x3ff:Le16(record.key.data+8);
                Check(record.key.size-header==nameLength,"Invalid APFS directory record name length");
                const auto oid=Le64(record.value.data);
                Check(oid>2&&oid<=IdMask,"Invalid APFS directory record inode");
                auto inode=LoadInode(catalog,folder.mount,oid);
                Check(!writable||!inode.entry.hardLink,"APFS writable mounts require unlinked native entries");
                auto name=Apfs::Name(record.key.data+header,nameLength);
                Check(name!=u"."&&name!=u".."&&name.find(u'/')==std::u16string::npos&&names.insert(name).second,
                      "Invalid or duplicate APFS directory name");
                if(inode.entry.directory){
                    for(uint32_t id=parent;id>=2;id=At(id).entry.parent)
                        Check(At(id).oid!=oid,"Circular APFS directory");
                }
                auto& entry=inode.entry;
                entry.parent=parent;entry.name=std::move(name);
                auto identity=MountKey(mounted)+":"+std::to_string(folder.oid)+":"+std::to_string(oid);
                // Alias names in the same directory can refer to the same
                // inode, but each alias is a separate browser selection.
                if(entry.hardLink){
                    identity+=':';
                    identity.append(reinterpret_cast<const char*>(record.key.data+header),nameLength-1);
                }
                entry.identity=MacBrowser::EntryIdentity(std::move(identity),EntryVersion(mounted));
                children.push_back({folder.mount,oid,inode.stream,std::move(entry),
                                    std::move(inode.attributes),inode.symlink});
            });
            Check(children.size()<=uint64_t(UINT32_MAX)+1-next,"Too many APFS browser entries");
            std::vector<uint32_t> ids;
            ids.reserve(children.size());
            for(auto& child:children){
                if(writable){
                    const auto slot=writableIds.emplace(child.oid,uint32_t(next));
                    if(slot.second)++next;
                    child.entry.id=slot.first->second;
                }else child.entry.id=uint32_t(next++);
                child.entry.resolvedId=child.entry.id;
                if(child.entry.hardLink){
                    const auto first=linkIdentities.emplace(Address{child.mount,child.oid},child.entry.id);
                    child.entry.resolvedId=first.first->second;
                }
                ids.push_back(child.entry.id);
                entries.insert_or_assign(child.entry.id,std::move(child));
            }
            listings.emplace(parent,std::move(ids));
        }
        std::vector<Entry> out;
        for(const auto id:listings.at(parent))out.push_back(At(id).entry);
        return out;
    }
public:
    Entry Stat(uint32_t id)override{return Access([&]{return At(id).entry;});}
    bool FindChild(uint32_t parent,const std::u16string& name,Entry& found)override{
        const auto [sensitive,normalized]=Access([&]{
            const auto& folder=At(parent);
            return std::pair{!folder.oid||mounts[folder.mount].sensitive,
                             folder.oid&&mounts[folder.mount].hashedNames};
        });
        const auto key=normalized?NormalizedName(name,!sensitive):std::u32string{};
        for(auto& entry:List(parent)){
            if(normalized?NormalizedName(entry.name,!sensitive)!=key:
                          HfsPlus::CompareNames(entry.name,name,sensitive)!=0)continue;
            found=std::move(entry);
            return true;
        }
        return false;
    }
private:
    void ReadEncryptedExtent(const VolumeCipher& cipher,const Extent& extent,uint64_t within,uint8_t* out,size_t size){
        constexpr size_t sectorSize=512;
        const auto sectorsPerBlock=container.blockSize/sectorSize;
        Check(extent.cryptoId<=(UINT64_MAX-extent.length/sectorSize)/sectorsPerBlock,
              "Invalid APFS extent encryption address");
        const auto firstSector=extent.cryptoId*sectorsPerBlock;
        const auto base=container.offset+extent.block*container.blockSize;
        std::array<uint8_t,sectorSize> sector;
        while(size){
            const auto index=within/sectorSize,skip=size_t(within%sectorSize);
            size_t take;
            if(!skip&&size>=sectorSize){
                take=size-size%sectorSize;
                source->Read(base+within,out,take);
                cipher.Decrypt(firstSector+index,out,take);
            }else{
                take=(std::min)(sectorSize-skip,size);
                source->Read(base+index*sectorSize,sector.data(),sector.size());
                cipher.Decrypt(firstSector+index,sector.data(),sector.size());
                memcpy(out,sector.data()+skip,take);
            }
            within+=take;out+=take;size-=take;
        }
    }
    void ReadStream(size_t mount,uint64_t stream,uint64_t at,void* data,size_t size){
        if(!size)return;
        Check(stream!=0,"Missing APFS stream identifier");
        const Address address{mount,stream};
        auto it=extents.find(address);
        if(it==extents.end()){
            const Catalog catalog(*source,container,mounts[mount]);
            it=extents.emplace(address,Extents(catalog,container,stream,bool(mounts[mount].cipher))).first;
        }
        const auto& list=it->second;
        auto run=std::lower_bound(list.begin(),list.end(),at,[](const Extent& extent,uint64_t offset){
            return extent.offset+extent.length<=offset;
        });
        auto out=static_cast<uint8_t*>(data);
        while(size){
            Check(run!=list.end()&&run->offset<=at,"Missing APFS file extent");
            const auto within=at-run->offset;
            const auto take=size_t(std::min<uint64_t>(size,run->length-within));
            if(!run->block)memset(out,0,take);
            else if(const auto& cipher=mounts[mount].cipher)ReadEncryptedExtent(*cipher,*run,within,out,take);
            else source->Read(container.offset+run->block*container.blockSize+within,out,take);
            at+=take;out+=take;size-=take;++run;
        }
    }
    void ReadAttribute(size_t mount,const Attribute& attribute,uint64_t at,void* data,size_t size){
        Check(Range(at,size,attribute.size),"Invalid APFS attribute range");
        if(!size)return;
        if(attribute.stream)ReadStream(mount,attribute.stream,at,data,size);
        else memcpy(data,attribute.embedded.data()+size_t(at),size);
    }
public:
    std::string PropertiesDetails(uint32_t id)override{
        return Access([&]() -> std::string {
            const auto& file=At(id);
            if(file.oid==2){
                const auto& mount=mounts[file.mount];
                if(mount.locked)return LockedMessage;
                if(mount.snapshotCreated)return "APFS snapshot (transaction "+std::to_string(mount.xid)+")";
            }
            if(!file.symlink)return {};
            const auto target=file.attributes.find(SymlinkAttribute);
            constexpr size_t maxDisplayedPath=4096;
            if(target==file.attributes.end()||target->second.size<2||target->second.size>maxDisplayedPath)
                return "Symbolic link target unavailable";
            try{
                Bytes path(size_t(target->second.size));
                ReadAttribute(file.mount,target->second,0,path.data(),path.size());
                Apfs::Name(path.data(),path.size()); // Validate UTF-8 and the terminating null.
                std::string details="Symbolic link to ";
                for(size_t i=0;i+1<path.size();++i){
                    const auto c=path[i];
                    if(c<32||c==127){
                        constexpr char hex[]="0123456789abcdef";
                        details+="\\x";
                        details+=hex[c>>4];
                        details+=hex[c&15];
                    }else details+=char(c);
                }
                return details;
            }catch(const Error&){return "Symbolic link target unavailable";}
        });
    }
    Attributes GetAttributes(uint32_t id)override{
        return Access([&]{
            Attributes out;
            const auto& file=At(id);
            for(const auto& pair:file.attributes){
                if(pair.first==FinderInfo||pair.first==ResourceFork||pair.first==CompressionAttribute||
                   pair.first==SymlinkAttribute)continue;
                const auto& attr=pair.second;
                if(attr.stream)
                    out.emplace(pair.first,AttributeData::Snapshot(attr.size,[&](uint64_t at,void* data,size_t size){
                        ReadAttribute(file.mount,attr,at,data,size);
                    },true));
                else out.emplace(pair.first,AttributeData(attr.embedded));
            }
            return out;
        });
    }
    void Read(uint32_t id,unsigned fork,uint64_t at,void* data,size_t size)override{
        Access([&]{
            const auto& file=At(id);
            const auto& entry=file.entry;
            Check(fork<2&&!entry.directory&&Range(at,size,entry.forks[fork].size),"Invalid APFS fork range");
            if(!entry.readError.empty())throw Error(entry.readError);
            if(size){
                if(fork==1)ReadAttribute(file.mount,file.attributes.at(ResourceFork),at,data,size);
                else if(entry.compressed){
                    const Address address{file.mount,file.oid};
                    auto decoder=compressed.find(address);
                    if(decoder==compressed.end()){
                        const auto attr=file.attributes.find(ResourceFork);
                        const auto resource=attr==file.attributes.end()?Attribute{}:attr->second;
                        auto reader=[this,resource,mount=file.mount](uint64_t offset,void* bytes,size_t length){
                            ReadAttribute(mount,resource,offset,bytes,length);
                        };
                        const auto& attribute=file.attributes.at(CompressionAttribute);
                        // The shared codecs accept compressed units up to 128 KiB.
                        Check(attribute.size<=131072+16,"APFS compression attribute exceeds supported unit size");
                        Bytes encoded(size_t(attribute.size));
                        ReadAttribute(file.mount,attribute,0,encoded.data(),encoded.size());
                        decoder=compressed.emplace(address,std::make_unique<HfsPlus::Compression>(
                            std::move(encoded),resource.size,std::move(reader))).first;
                    }
                    Check(decoder->second->Read(at,data,size)==size,"Short APFS compressed read");
                }else ReadStream(file.mount,file.stream,at,data,size);
            }
            return true;
        });
    }
private:
    template<class Work> auto WriteMount(Work work)->decltype(work()){
        if(!writable)ReadOnly();
        VolumeCoord::OperationLease lease({{key,VolumeCoord::Mode::Write}});
        std::lock_guard<std::recursive_mutex> lock(mutex);
        Check(!requiresReopen,"Reopen the APFS image before continuing");
        Check(!writing,"An APFS write cannot run inside another APFS write");
        struct Guard {
            bool& active;
            explicit Guard(bool& state):active(state){active=true;}
            ~Guard(){active=false;}
        } guard(writing);
        source->CheckMedia();
        return work();
    }

    void RefreshNamespace(uint32_t parent,uint32_t removed,uint64_t transaction)
    {
        auto fresh=Probe(*source);
        Check(fresh&&fresh->uuid==container.uuid&&fresh->volumes==container.volumes&&
              fresh->transaction==transaction,"The APFS image changed during a catalog write");
        const auto oid=mounts.front().oid;
        const ObjectMap map(*source,*fresh,fresh->objectMap,fresh->transaction);
        const auto address=map.Lookup(oid);
        Check(address.has_value(),"Missing APFS volume after a catalog write");
        auto mount=ReadVolume(ReadObject(*source,*fresh,*address,oid,fresh->transaction,13),oid);
        freeBytes=WritableFreeBytes(*source,*fresh);
        container=std::move(*fresh);
        mounts.front()=std::move(mount);
        listings.clear();
        extents.clear();
        compressed.clear();
        if(removed){
            writableIds.erase(At(removed).oid);
            entries.erase(removed);
        }
        auto& folder=At(parent);
        const auto changed=LoadInode(Catalog(*source,container,mounts.front()),0,folder.oid);
        folder.entry.modified=changed.entry.modified;
        ListDirectory(parent);
    }

    template<class Work>void CommitMutation(uint32_t parent,uint32_t removed,Storage::Job& job,Work work)
    {
        uint64_t transaction;
        try{transaction=work(job);}
        catch(const CheckpointPublicationUncertain&){
            // A buffered checkpoint may still be visible after a failed flush.
            // Keep this session closed to further mutations until it is reopened.
            requiresReopen=true;
            Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Unverified);
            throw;
        }
        Storage::OutcomeEvidence::NoteRetainedChange();
        try{RefreshNamespace(parent,removed,transaction);}
        catch(const std::exception& error){
            requiresReopen=true;
            throw Error(std::string("APFS catalog change committed. Reopen the image to refresh it. ")+error.what());
        }
    }
    uint32_t CreateFolder(uint32_t parent,const std::u16string& name,std::optional<FileDates> dates,const FinderBytes* finder=nullptr){
        return WriteMount([&]{
            const auto oid=At(parent).oid;
            Storage::Job job;
            CommitMutation(parent,0,job,[&](auto& job){return CreateCatalogDirectory(*source,container,oid,name,job,dates,finder);});
            for(const auto id:listings.at(parent))if(At(id).entry.name==name)return id;
            throw Error("APFS folder was created but could not be found after reopening");
        });
    }
    static std::optional<FileDates> CopyDates(const Entry& entry){
        if(!entry.datesValid)return {};
        return FileDates{entry.created,entry.modified};
    }
public:
    uint32_t MakeDirectory(uint32_t parent,const std::u16string& name)override{
        return CreateFolder(parent,name,{});
    }
    uint32_t MakeDirectoryWithAttributes(uint32_t parent,const Entry& entry)override{
        ValidateEntry(entry);
        Check(entry.directory,"The APFS copy source is not a folder");
        return CreateFolder(parent,entry.name,CopyDates(entry),&entry.finder);
    }
    void FinalizeDirectory(uint32_t id,const Entry& desired,const Entry& expected,bool)override{
        const auto dates=CopyDates(desired);
        if(!dates)return;
        WriteMount([&]{
            const auto& item=At(id);
            const auto& current=item.entry;
            Check(current.directory&&current.identity==expected.identity&&current.parent==expected.parent&&
                  current.name==expected.name,"The APFS destination folder changed during copying");
            if(current.datesValid&&current.created==dates->created&&current.modified==dates->modified)return;
            const auto parent=current.parent;
            const auto parentOid=At(parent).oid,oid=item.oid;
            // Restore destination folder dates even if the transfer was cancelled.
            Storage::Job metadata;
            CommitMutation(parent,0,metadata,[&](auto& job){
                return SetDirectoryDates(*source,container,parentOid,oid,*dates,job);
            });
        });
    }
    void ValidateName(const std::u16string& name)const override{EncodeCatalogName(name);}
    void ValidateEntry(const Entry& entry)const override{
        if(!writable)ReadOnly();
        ValidateName(entry.name);
        Check(!entry.directory||!entry.forks[0].size,"An APFS folder cannot store file data");
        Check(entry.readError.empty()&&!entry.forks[1].size&&entry.attributes.empty()&&
              !entry.protectedFile&&!entry.hardLink&&!entry.compressed&&!entry.version,
              "This APFS writer cannot preserve the source entry's Mac metadata");
        if(entry.datesValid){
            CatalogTimestamp(entry.created);
            CatalogTimestamp(entry.modified);
        }
    }
    void ValidateCopyNames(uint32_t,const std::vector<Entry>& entries,bool)override{
        // The common planner skips per-entry validation for folders it will merge.
        for(const auto& entry:entries)if(entry.directory)ValidateEntry(entry);
    }
    uint32_t Put(uint32_t parent,const Entry& entry,MacBrowser::Reader reader,bool replace)override{
        Storage::Job job;
        return PutForTransfer(parent,entry,std::move(reader),replace,job);
    }
    uint32_t PutFromHost(uint32_t parent,const Entry& entry,MacBrowser::Reader reader,bool replace,
                         const std::wstring&,Storage::Job& job)override{
        return PutForTransfer(parent,entry,std::move(reader),replace,job);
    }
    uint32_t PutForTransfer(uint32_t parent,const Entry& entry,MacBrowser::Reader reader,bool replace,
                            Storage::Job& job)override{
        ValidateEntry(entry);
        Check(!entry.directory,"Select a regular file for APFS data transfer");
        const auto dates=CopyDates(entry);
        class DataFork final:public Source {
            const uint64_t size;
            const MacBrowser::Reader& read;
        public:
            DataFork(uint64_t length,const MacBrowser::Reader& reader):size(length),read(reader){}
            uint64_t Size()const override{return size;}
            bool Writable()const override{return false;}
            void Read(uint64_t at,void* bytes,size_t length)override{
                Check(Range(at,length,size)&&bool(read),"Invalid APFS input data request");
                read(0,at,bytes,length);
            }
            void Write(uint64_t,const void*,size_t)override{ReadOnly();}
            void Flush()override{}
        } data(entry.forks[0].size,reader);

        return WriteMount([&]{
            const auto parentOid=At(parent).oid;
            const auto key=NormalizedName(entry.name,!caseSensitive);
            const auto matches=[&](const Entry& candidate){
                return NormalizedName(candidate.name,!caseSensitive)==key;
            };
            const auto children=ListDirectory(parent);
            const auto found=std::find_if(children.begin(),children.end(),matches);
            if(found!=children.end()){
                Check(replace&&!found->directory&&!found->protectedFile&&found->readError.empty(),
                      "The APFS destination already exists or cannot be replaced");
                Check(entry.expectedDestinationIdentity.empty()||entry.expectedDestinationIdentity==found->identity,
                      "The APFS destination changed after overwrite approval");
                const auto oid=At(found->id).oid;
                CommitMutation(parent,0,job,[&](auto& job){
                    return ReplaceFileData(*source,container,parentOid,oid,data,job,dates,&entry.finder);
                });
                return found->id;
            }
            Check(entry.expectedDestinationIdentity.empty(),"The APFS destination disappeared after overwrite approval");
            CommitMutation(parent,0,job,[&](auto& job){
                return CreateCatalogFile(*source,container,parentOid,entry.name,data,job,dates,&entry.finder);
            });
            for(const auto id:listings.at(parent))if(matches(At(id).entry))return id;
            throw Error("APFS file was committed but could not be found after reopening");
        });
    }
    uint32_t PutResourceReplacement(uint32_t,const Entry&,MacBrowser::Reader,
                                     std::function<void(uint64_t,size_t)>)override{
        throw Error("APFS resource-fork replacement is not supported yet");
    }
    void SetBasicMetadata(uint32_t id,const Entry& desired)override{
        WriteMount([&]{
            const auto& item=At(id);
            const auto& current=item.entry;
            Check(id!=2&&current.name==desired.name&&current.directory==desired.directory&&
                  current.version==desired.version&&current.protectedFile==desired.protectedFile,
                  "APFS Properties can only change Finder information and timestamps on files and folders");
            Check(current.datesValid==desired.datesValid,"APFS timestamps cannot be removed");
            TimestampEdits dates;
            if(current.created!=desired.created)dates.created=desired.created;
            if(current.modified!=desired.modified)dates.modified=desired.modified;
            if(current.finder==desired.finder&&!dates.created&&!dates.modified)return;
            const auto parent=current.parent;
            const auto parentOid=At(parent).oid,oid=item.oid;
            Storage::Job job;
            CommitMutation(parent,0,job,[&](auto& job){
                return SetEntryProperties(*source,container,parentOid,oid,desired.finder,dates,job);
            });
        });
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{
        WriteMount([&]{
            const auto& entry=At(id);
            Check(entry.entry.parent==parent,"APFS rename must stay in the original folder");
            const auto oid=entry.oid,parentOid=At(parent).oid;
            Storage::Job job;
            CommitMutation(parent,0,job,[&](auto& job){return RenameEntry(*source,container,parentOid,oid,name,job);});
        });
    }
    void Remove(uint32_t id)override{
        WriteMount([&]{
            const auto& entry=At(id);
            Check(id!=2,"Cannot delete the APFS root");
            const auto parent=entry.entry.parent;
            const auto oid=entry.oid;
            const auto parentOid=At(parent).oid;
            Storage::Job job;
            CommitMutation(parent,id,job,[&](auto& job){return DeleteEntry(*source,container,parentOid,oid,job);});
        });
    }
};
}

LockedVolume::LockedVolume(bool again,std::wstring passwordHint)
    :HfsPlus::Error(again?"The password did not unlock the APFS volume":LockedMessage),
     retry(again),hint(std::move(passwordHint)){}

void Unlock(MacBrowser::Volume& volume,uint32_t id,const Dmg::Password& password)
{
    const auto apfs=dynamic_cast<Volume*>(&volume);
    Check(apfs!=nullptr,"Select an encrypted APFS volume");
    apfs->Unlock(id,password);
}

static std::shared_ptr<MacBrowser::Volume> OpenMounted(std::unique_ptr<Source>& source,const std::string& key,
                                                    const Container& container,bool writable,uint64_t freeBytes=0)
{
    Check(!container.volumes.empty(),"The APFS container has no volumes");
    const ObjectMap map(*source,container,container.objectMap,container.transaction);
    std::vector<Mounted> volumes;
    std::exception_ptr problem;
    for(const auto oid:container.volumes){
        try{
            const auto block=map.Lookup(oid);
            Check(block.has_value(),"Missing APFS volume mapping");
            auto live=ReadVolume(ReadObject(*source,container,*block,oid,container.transaction,13),oid);
            try{
                auto snapshots=ReadSnapshots(*source,container,live);
                volumes.push_back(std::move(live));
                for(auto& view:snapshots)volumes.push_back(std::move(view));
            }catch(const Error&){
                volumes.push_back(std::move(live));
            }
        }catch(const Error&){if(!problem)problem=std::current_exception();}
    }
    if(volumes.empty())std::rethrow_exception(problem);
    return std::make_shared<Volume>(std::move(source),container,std::move(volumes),key,writable,freeBytes);
}

std::shared_ptr<MacBrowser::Volume> Open(std::unique_ptr<Source>& source,const std::string& key)
{
    const auto container=Probe(*source);
    return container?OpenMounted(source,key,*container,false):nullptr;
}

std::shared_ptr<MacBrowser::Volume> OpenWritable(std::unique_ptr<Source>& source,const std::string& key)
{
    const auto container=Probe(*source);
    if(!container)return {};
    Check(source->Writable(),"APFS writes require an exclusive writable image");
    ValidateWritableLayout(*source,*container);
    const auto free=WritableFreeBytes(*source,*container);
    // A prior mount may have closed after an uncertain publication.
    source->Flush();
    return OpenMounted(source,key,*container,true,free);
}

std::shared_ptr<MacBrowser::Volume> OpenRawBrowserImage(std::unique_ptr<Source>& source,
    const std::string& key,const ReopenRawImage& reopen)
{
    const auto expected=Probe(*source);
    if(!expected)return {};
    try{
        ValidateWritableLayout(*source,*expected);
        WritableFreeBytes(*source,*expected);
    }catch(const Error&){
        return OpenMounted(source,key,*expected,false);
    }

    const auto verify=[&]{
        const auto mounted=Probe(*source);
        if(!mounted||mounted->uuid!=expected->uuid||mounted->transaction!=expected->transaction||
           mounted->checkpointBlock!=expected->checkpointBlock)
            throw Storage::MediaChanged("The selected APFS checkpoint changed while opening the image");
    };
    source.reset();
    try{
        source=reopen(true);
        verify();
        return OpenWritable(source,key);
    }catch(const Storage::MediaChanged&){
        throw;
    }catch(const Error&){
        source.reset();
        source=reopen(false);
        verify();
        return OpenMounted(source,key,*expected,false);
    }
}
}
