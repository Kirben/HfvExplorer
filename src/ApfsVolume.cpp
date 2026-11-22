// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsVolume.h"
#include "ApfsCatalogWrite.h"
#include "ApfsUnicode.h"
#include "StorageWorker.h"
#include "ApfsTree.h"
#include "ApfsWriteDisk.h"
#include "ApfsCrypto.h"
#include "HfsPlusDisk.h"
#include "DiskPartitions.h"
#include "DiskSlice.h"
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
constexpr int64_t NanosecondsPerSecond=1000000000;
uint64_t Date(const uint8_t* p)
{
    const auto timestamp=int64_t(Le64(p));
    const auto seconds=timestamp/NanosecondsPerSecond;
    // Floor fractional instants on either side of the Unix epoch.
    return uint64_t(seconds-(timestamp%NanosecondsPerSecond<0)+2082844800);
}
struct Mounted {
    uint64_t oid,xid,map,tree;
    bool sensitive,hashedNames,locked,softwareEncryption;
    std::u16string name;
    std::array<uint8_t,16> uuid;
    std::shared_ptr<const VolumeCipher> cipher;
    uint64_t mapTransaction=0,snapshotTree=0;
    std::optional<uint64_t> snapshotCreated;
    uint64_t reserveBlocks,quotaBlocks;
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
                  locked,software,Name(p+nameOffset,size_t(end-(p+nameOffset))+1),{},{},0,0,{},
                  Le64(p+72),Le64(p+80)};
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


std::pair<uint64_t,bool> ReadSiblingId(const Catalog& catalog,Record drec,uint64_t parent,uint64_t inode,
                       const uint8_t* name,size_t nameLength)
{
    const auto* value=drec.value.data;
    Check(drec.value.size==34&&Le16(value+18)==1&&Le16(value+20)==8&&
          value[22]==1&&Le16(value+24)==8,"Invalid APFS hard-link directory record");
    const auto sibling=Le64(value+26);
    Check(sibling>2&&sibling<=IdMask&&sibling!=inode,"Invalid APFS sibling ID");

    bool linked=false,mapped=false,primary=true;
    catalog.VisitRecords(inode,5,[&](Record record){
        Check(record.key.size==16,"Invalid APFS sibling link key");
        const auto id=Le64(record.key.data+8);
        primary&=id>=sibling;
        if(id!=sibling)return;
        const auto* link=record.value.data;
        Check(!linked&&record.value.size==10+nameLength&&Le64(link)==parent&&
              Le16(link+8)==nameLength&&!memcmp(link+10,name,nameLength),
              "APFS sibling link does not match its directory entry");
        linked=true;
    });
    catalog.VisitRecords(sibling,12,[&](Record record){
        Check(!mapped&&record.key.size==8&&record.value.size==8&&Le64(record.value.data)==inode,
              "APFS sibling map does not match its inode");
        mapped=true;
    });
    Check(linked&&mapped,"Missing APFS sibling link or map");
    return {sibling,primary};
}

constexpr auto ResourceFork=u"com.apple.ResourceFork";
constexpr auto FinderInfo=u"com.apple.FinderInfo";
constexpr auto CompressionAttribute=u"com.apple.decmpfs";

size_t InlineCompressionCapacity(){
    constexpr auto attributeOverhead=10+sizeof("com.apple.decmpfs")+4;
    return WriterDisk::Block-WriterDisk::NodeHeader-WriterDisk::TreeFooter-
           WriterDisk::VariableToc(1)-attributeOverhead;
}
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
        // Apple can mark a streamed attribute with the 0x10 flag.
        Check(size==record.value.size-4&&!(flags&~0x17u),"Invalid APFS attribute value");
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
        entry.additionalDates={Date(p+40),Date(p+32)};
        constexpr uint64_t wasCloned=0x10,wasEverCloned=0x400;
        entry.apfsCloneMarked=(Le64(p+48)&(wasCloned|wasEverCloned))!=0;
        for(unsigned offset:{16,24,32,40})
            entry.fractionalDates|=int64_t(Le64(p+offset))%NanosecondsPerSecond!=0;
        const auto fullMode=Le16(p+80);
        const auto mode=fullMode&0170000;
        entry.directory=mode==0040000;
        const auto special=SpecialType(mode>>12);
        if(entry.directory||mode==0100000||mode==0120000||special!=SpecialFileType::None)
            entry.posix=PosixMetadata{Le32(p+72),Le32(p+76),uint16_t(fullMode&PosixMetadata::ModeMask),
                                     uint8_t(((Le32(p+68)&UserImmutable)?PosixMetadata::UserImmutable:0)|
                                             ((Le32(p+68)&SystemImmutable)?PosixMetadata::SystemImmutable:0)|
                                             ((Le32(p+68)&UserAppendOnly)?PosixMetadata::UserAppendOnly:0)|
                                             ((Le32(p+68)&SystemAppendOnly)?PosixMetadata::SystemAppendOnly:0)),
                                     uint8_t(((Le32(p+68)&NoDump)?PosixMetadata::NoDump:0)|
                                             ((Le32(p+68)&Opaque)?PosixMetadata::Opaque:0)|
                                             ((Le32(p+68)&Archived)?PosixMetadata::Archived:0)),special};
        entry.linkCount=entry.directory?1:Le32(p+56);
        entry.hardLink=entry.linkCount>1;
        entry.compressed=(Le32(p+68)&0x20)!=0;
        entry.protectedFile=(Le32(p+68)&WriteProtectionMask)!=0;
        out.symlink=mode==0120000;
        entry.symbolicLink=out.symlink;
        if(out.symlink)entry.readError="APFS symbolic links cannot be exported";
        else if(!entry.directory&&mode!=0100000&&special==SpecialFileType::None)entry.readError="APFS special files are unsupported";
        if(record.value.size==inodeSize){Check(!IsDevice(special),"Missing APFS device identifier");return;}
        const auto fields=p+inodeSize;
        Check(record.value.size>=inodeSize+4,"Truncated APFS inode extended-field header");
        const auto count=Le16(fields),used=Le16(fields+2);
        const size_t first=inodeSize+4+size_t(count)*4;
        Check(first<=record.value.size&&used<=record.value.size-inodeSize-4,"Invalid APFS inode extended fields");
        size_t at=first;
        const size_t end=record.value.size;
        bool hasStream=false,hasDevice=false;
        for(unsigned i=0;i<count;++i){
            const auto field=fields+4+i*4;
            const size_t size=Le16(field+2),aligned=(size+7)&~size_t(7);
            Check(Range(at,aligned,end),"Truncated APFS inode extended field");
            if(field[0]==8){
                Check(size==40&&!hasStream,"Invalid APFS inode data stream");
                entry.forks[0].size=Le64(p+at);hasStream=true;
            }else if(field[0]==14){
                Check(IsDevice(special)&&size==4&&!hasDevice,"Invalid APFS device identifier");
                entry.posix->device=Le32(p+at);
                hasDevice=true;
            }else if(field[0]==7){
                Check(size==entry.finder.size(),"Invalid APFS Finder information");
                memcpy(entry.finder.data(),p+at,size);
            }
            at+=aligned;
        }
        Check(hasDevice==IsDevice(special),"Missing APFS device identifier");
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
              (!block||Range(block,length/container.blockSize,container.blockCount))&&
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
    std::shared_ptr<const std::u16string> updatedLabel;
    bool caseSensitive=true;
    const std::string key;
    std::atomic<bool> writable,limitedEdits;
    const bool processLocal;
    std::atomic<uint64_t> freeBytes;
    MacBrowser::WorkerOrigin origin;
    std::recursive_mutex mutex;
    bool requiresReopen=false,writing=false;
    std::map<std::pair<size_t,uint64_t>,uint32_t> writableIds;
    struct Item {
        size_t mount;
        uint64_t oid,stream;
        Entry entry;
        AttributeRecords attributes;
        bool symlink=false;
        uint64_t aliasId=0,siblingId=0;
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
    Volume(std::unique_ptr<Source> s,Container c,std::vector<Mounted> volumes,std::string resource,bool write,uint64_t free,bool limited)
        :source(std::move(s)),container(std::move(c)),mounts(std::move(volumes)),key(std::move(resource)),
         writable(write),limitedEdits(limited),processLocal(write||limited||source->ProcessLocal()||std::any_of(mounts.begin(),mounts.end(),[](const auto& mount){
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
    std::vector<std::u16string> SnapshotNames(std::u16string* volumeName=nullptr){
        return Access([&]{
            Check(writable&&source->Writable()&&container.volumes.size()==1&&!mounts.front().locked&&
                  !mounts.front().cipher,"Snapshot management requires a writable unencrypted single-volume APFS image");
            return ListSnapshotNames(*source,container,volumeName);
        });
    }
    bool CanRestoreFolder(uint32_t id){
        VolumeCoord::BackendRead lease(key);
        std::lock_guard<std::recursive_mutex> lock(mutex);
        const auto item=entries.find(id);
        return writable&&!requiresReopen&&item!=entries.end()&&item->second.entry.directory&&
               mounts[item->second.mount].snapshotCreated.has_value();
    }
    Entry RestoreDestination(uint32_t id){
        return Access([&]{
            Check(CanRestoreFolder(id),"Select a folder in a snapshot of a writable APFS volume");
            const auto view=At(id).mount;
            const auto oid=mounts[view].oid;
            std::vector<std::u16string> names;
            std::set<uint32_t> seen;
            for(auto current=id;;current=At(current).entry.parent){
                const auto& item=At(current);
                Check(item.mount==view&&item.entry.directory&&seen.insert(current).second,
                      "The APFS snapshot folder changed");
                if(item.oid==2)break;
                names.push_back(item.entry.name);
            }
            Entry destination;
            for(const auto root:listings.at(2)){
                const auto& entry=At(root);
                if(mounts[entry.mount].oid==oid&&!mounts[entry.mount].snapshotCreated){
                    destination=entry.entry;
                    break;
                }
            }
            Check(destination.id!=0,"The live APFS volume is unavailable");
            for(auto name=names.rbegin();name!=names.rend();++name){
                Entry child;
                Check(FindChild(destination.id,*name,child)&&child.directory,
                      "The original folder is missing on the live volume. Restore its parent folder first.");
                destination=std::move(child);
            }
            Check(!destination.protectedFile,"The live APFS destination folder is locked");
            return destination;
        });
    }
    std::vector<uint32_t> FolderRoute(const std::vector<uint32_t>& previous){
        return Access([&]{
            for(const auto candidate:previous){
                const auto selected=entries.find(candidate);
                if(selected==entries.end()||!selected->second.entry.directory)continue;
                std::vector<uint32_t> route;
                std::set<uint32_t> seen;
                for(auto id=candidate;id!=1;id=At(id).entry.parent){
                    Check(seen.insert(id).second,"Cyclic APFS folder route");
                    route.push_back(id);
                }
                return route;
            }
            return std::vector<uint32_t>{2};
        });
    }
    std::vector<std::u16string> ChangeSnapshot(const std::u16string& name,bool create,Storage::Job& job){
        VolumeCoord::Lease access({{key,VolumeCoord::Mode::Write}});
        bool changed=false;
        try{
            auto names=WriteMount([&]{
                SnapshotNames();
                return CompleteWrite([&]{
                    return create?CreateSnapshot(*source,container,name,job):DeleteSnapshot(*source,container,name,job);
                },[&](uint64_t transaction){
                    RefreshSnapshotViews(transaction,create);
                    return ListSnapshotNames(*source,container);
                },&changed);
            },true);
            access.ReleaseAndPublish({key});
            return names;
        }catch(...){
            if(changed)access.ReleaseAndPublish({key});
            throw;
        }
    }
    bool CanRenameRoot(uint32_t id){
        VolumeCoord::BackendRead lease(key);
        std::unique_lock<std::recursive_mutex> lock(mutex,std::try_to_lock);
        if(!lock.owns_lock()||!writable||requiresReopen)return false;
        const auto item=entries.find(id);
        if(item==entries.end()||item->second.oid!=2)return false;
        const auto& mount=mounts[item->second.mount];
        return !mount.locked&&!mount.snapshotCreated;
    }
    void RenameRoot(uint32_t id,const std::u16string& name,Storage::Job& job){
        VolumeCoord::Lease access({{key,VolumeCoord::Mode::Write}});
        bool committed=false;
        try{
            const bool changed=WriteMount([&]{
                if(job.cancelled())throw Storage::Cancelled();
                Check(CanRenameRoot(id),"Select a writable live APFS volume");
                const auto mountIndex=At(id).mount;
                const auto oldName=mounts[mountIndex].name;
                if(oldName==name)return false;
                CompleteWrite([&]{
                    return RenameVolume(*source,WriteTarget(id),name,job);
                },[&](uint64_t transaction){
                    RefreshNamespace(id,0,transaction,0,0);
                    auto& root=At(id).entry;
                    const auto& actual=mounts[mountIndex].name;
                    root.name=actual;
                    for(auto& [rootId,item]:entries){
                        auto& view=mounts[item.mount];
                        if(item.oid!=2||view.oid!=mounts[mountIndex].oid||!view.snapshotCreated)continue;
                        view.name=actual+view.name.substr(oldName.size());
                        item.entry.name=view.name;
                    }
                    if(id==2)std::atomic_store(&updatedLabel,std::make_shared<const std::u16string>(actual));
                    else{
                        for(unsigned suffix=2;;++suffix){
                            const auto duplicate=std::any_of(listings.at(2).begin(),listings.at(2).end(),[&](uint32_t sibling){
                                return sibling!=id&&At(sibling).entry.name==root.name;
                            });
                            if(!duplicate)break;
                            const auto number=std::to_string(suffix);
                            root.name=actual+u" ("+std::u16string(number.begin(),number.end())+u")";
                        }
                    }
                },&committed);
                return true;
            },true);
            if(changed)access.ReleaseAndPublish({key});
        }catch(...){
            if(committed)access.ReleaseAndPublish({key});
            throw;
        }
    }
    std::u16string Name()const override{
        const auto name=std::atomic_load(&updatedLabel);
        return name?*name:label;
    }
    std::string ResourceKey()const override{return key;}
    const char* Kind()const override{return "APFS";}
    bool CaseSensitive()const override{return caseSensitive;}
    bool Writable()const override{return writable&&!limitedEdits;}
    uint64_t FreeBytes()const override{return freeBytes.load();}
    MacBrowser::Capabilities Supports() const override {
        auto capabilities = MacBrowser::Volume::Supports();
        capabilities.resourceForks=true;
        capabilities.create=writable;
        capabilities.rename=writable;
        capabilities.remove=writable;
        capabilities.writable=writable;
        capabilities.createFolders=writable;
        capabilities.unicodeNames=true;
        capabilities.attributes=true;
        capabilities.basicMetadata=writable;
        capabilities.finderMetadata=writable;
        capabilities.compression=writable;
        capabilities.hardLinks=writable;
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
            unlocked.locked=false;
            root.entry.id=item.entry.id;
            root.entry.resolvedId=item.entry.resolvedId;
            root.entry.parent=item.entry.parent;
            root.entry.name=item.entry.name;
            root.entry.identity=item.entry.identity;
            bool canWrite=false;
            if(source->Writable()&&limitedEdits){
                try{
                    auto target=WriteTarget(unlocked);
                    InspectFreeQueueAllocation(*source,target,unlocked.oid);

                    source->Flush();
                    canWrite=true;
                }catch(const Error&){}
            }
            source->CheckMedia();
            item.entry=std::move(root.entry);
            item.attributes=std::move(root.attributes);
            item.stream=root.stream;
            mount.cipher=std::move(unlocked.cipher);
            mount.locked=false;
            writable=canWrite;
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
            std::set<std::u32string> names;
            std::set<uint64_t> unlinkedInodes;
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
                uint64_t aliasId=0,siblingId=0;
                auto name=Apfs::Name(record.key.data+header,nameLength);
                Check(name!=u"."&&name!=u".."&&name.find(u'/')==std::u16string::npos,
                      "Invalid APFS directory name");
                auto nameKey=mounted.hashedNames?NormalizedName(name,!mounted.sensitive):
                                                  std::u32string(name.begin(),name.end());
                Check(names.insert(std::move(nameKey)).second,"Ambiguous APFS directory name");
                Check(inode.entry.hardLink||unlinkedInodes.insert(oid).second,
                      "APFS directory entries repeat an inode without hard-link records");
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
                    const auto [sibling,primary]=ReadSiblingId(catalog,record,folder.oid,oid,
                                                               record.key.data+header,nameLength);
                    siblingId=sibling;
                    if(!writable||!primary){
                        aliasId=sibling;
                        identity+=':';
                        identity+=std::to_string(sibling);
                    }
                }
                entry.identity=MacBrowser::EntryIdentity(std::move(identity),EntryVersion(mounted));
                children.push_back({folder.mount,oid,inode.stream,std::move(entry),
                                    std::move(inode.attributes),inode.symlink,aliasId,siblingId});
            });
            Check(children.size()<=uint64_t(UINT32_MAX)+1-next,"Too many APFS browser entries");
            std::vector<uint32_t> ids;
            ids.reserve(children.size());
            for(auto& child:children){
                if(writable){
                    const Address key{child.mount,child.aliasId?child.aliasId:child.oid};
                    if(!child.aliasId&&child.siblingId&&!writableIds.count(key)){
                        auto previous=writableIds.extract({child.mount,child.siblingId});
                        if(previous){
                            previous.key()=key;
                            writableIds.insert(std::move(previous));
                        }
                    }
                    const auto slot=writableIds.emplace(key,uint32_t(next));
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
    Entry Stat(uint32_t id)override{return Access([&]{
        const auto& file=At(id);
        auto entry=file.entry;
        if(entry.compressed&&entry.readError.empty()){
            const auto found=file.attributes.find(CompressionAttribute);
            if(found!=file.attributes.end()&&found->second.size<=InlineCompressionLimit){
                Bytes bytes(size_t(found->second.size));
                ReadAttribute(file.mount,found->second,0,bytes.data(),bytes.size());
                if(IsInlineCompression(bytes,entry.forks[0].size))entry.inlineCompression=std::move(bytes);
                else if(bytes.size()==16){
                    const Compression descriptor(bytes,0,{});
                    if(descriptor.UsesResource()){
                        entry.compressionResourceSize=file.attributes.at(ResourceFork).size;
                        entry.resourceCompressionHeader=std::move(bytes);
                    }
                }
            }
        }
        return entry;
    });}
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
    const std::vector<Extent>& StreamExtents(size_t mount,uint64_t stream){
        Check(stream!=0,"Missing APFS stream identifier");
        const Address address{mount,stream};
        auto it=extents.find(address);
        if(it==extents.end()){
            const Catalog catalog(*source,container,mounts[mount]);
            it=extents.emplace(address,Extents(catalog,container,stream,bool(mounts[mount].cipher))).first;
        }
        return it->second;
    }
    void ReadStream(size_t mount,uint64_t stream,uint64_t at,void* data,size_t size){
        if(!size)return;
        const auto& list=StreamExtents(mount,stream);
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
    std::optional<Bytes> SymbolicLinkTarget(uint32_t id)override{
        return Access([&]() -> std::optional<Bytes> {
            const auto& file=At(id);
            if(!file.symlink)return {};
            const auto target=file.attributes.find(SymlinkAttribute);
            Check(target!=file.attributes.end()&&target->second.size>=2&&target->second.size<=4096,
                  "Invalid APFS symbolic link target size");
            Bytes path(size_t(target->second.size));
            ReadAttribute(file.mount,target->second,0,path.data(),path.size());
            Apfs::Name(path.data(),path.size());
            return path;
        });
    }
    std::string PropertiesDetails(uint32_t id)override{
        return Access([&]() -> std::string {
            const auto& file=At(id);
            if(file.oid==2){
                const auto& mount=mounts[file.mount];
                if(mount.locked)return LockedMessage;
                if(mount.snapshotCreated)return "APFS snapshot (transaction "+std::to_string(mount.xid)+")";
                std::string limits;
                const auto append=[&](const char* label,uint64_t blocks){
                    if(!blocks||blocks>UINT64_MAX/container.blockSize)return;
                    if(!limits.empty())limits+="\r\n";
                    limits+=std::string(label)+std::to_string(blocks*container.blockSize)+" bytes";
                };
                append("APFS quota: ",mount.quotaBlocks);
                append("APFS reserve: ",mount.reserveBlocks);
                return limits;
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
    MacBrowser::SparseForks SparseRanges(uint32_t id)override{
        return Access([&]{
            const auto& file=At(id);
            MacBrowser::SparseForks holes;
            const auto collect=[&](unsigned fork,uint64_t stream){
                const auto size=file.entry.forks[fork].size;
                if(!stream||!size)return;
                for(const auto& extent:StreamExtents(file.mount,stream)){
                    if(extent.offset>=size)break;
                    if(!extent.block){
                        const auto length=(std::min)(extent.length,size-extent.offset);
                        auto& ranges=holes[fork];
                        if(!ranges.empty()&&ranges.back().offset+ranges.back().length==extent.offset)
                            ranges.back().length+=length;
                        else ranges.push_back({extent.offset,length});
                    }
                }
            };
            if(!file.entry.compressed)collect(0,file.stream);
            const auto resource=file.attributes.find(ResourceFork);
            if(resource!=file.attributes.end())collect(1,resource->second.stream);
            return holes;
        });
    }
    void ReadCompressionResource(uint32_t id,uint64_t at,void* data,size_t size)override{
        Access([&]{
            const auto entry=Stat(id);
            const auto& file=At(id);
            Check(entry.compressionResourceSize&&Range(at,size,entry.compressionResourceSize),
                  "Invalid APFS compression resource range");
            ReadAttribute(file.mount,file.attributes.at(ResourceFork),at,data,size);
            return true;
        });
    }
    void Read(uint32_t id,unsigned fork,uint64_t at,void* data,size_t size)override{
        Access([&]{
            const auto& file=At(id);
            const auto& entry=file.entry;
            Check(fork<2&&!entry.directory&&Range(at,size,entry.forks[fork].size),"Invalid APFS fork range");
            if(!entry.readError.empty())throw Error(entry.readError);
            Check(!entry.posix||!entry.posix->IsSpecial(),"A special file has no file forks");
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
    template<class Work> auto WriteMount(Work work,bool allowLimited=false)->decltype(work()){
        if(!writable||(limitedEdits&&!allowLimited))ReadOnly();
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

    void RefreshNamespace(uint32_t parent,uint32_t removed,uint64_t transaction,uint32_t changedFile,uint32_t oldParent)
    {
        auto fresh=Probe(*source);
        Check(fresh&&fresh->uuid==container.uuid&&fresh->volumes==container.volumes&&
              fresh->transaction==transaction,"The APFS image changed during a catalog write");
        const auto mountIndex=At(parent).mount;
        const auto oid=mounts[mountIndex].oid;
        const ObjectMap map(*source,*fresh,fresh->objectMap,fresh->transaction);
        const auto address=map.Lookup(oid);
        Check(address.has_value(),"Missing APFS volume after a catalog write");
        auto mount=ReadVolume(ReadObject(*source,*fresh,*address,oid,fresh->transaction,13),oid);
        if(const auto& cipher=mounts[mountIndex].cipher){
            mount.cipher=cipher;
            mount.locked=false;
        }
        freeBytes=WritableFreeBytes(*source,*fresh);
        container=std::move(*fresh);
        for(auto& view:mounts){
            if(view.oid!=oid||!view.snapshotCreated)continue;
            view.map=mount.map;
            view.mapTransaction=mount.xid;
        }
        mounts[mountIndex]=std::move(mount);
        for(auto listing=listings.begin();listing!=listings.end();){
            const auto& folder=At(listing->first);
            if(folder.oid&&folder.mount==mountIndex)listing=listings.erase(listing);
            else ++listing;
        }
        extents.clear();
        compressed.clear();
        if(removed){
            const auto deleted=At(removed);
            if(limitedEdits&&deleted.entry.directory){
                std::vector<uint32_t> descendants;
                std::set<uint64_t> affectedLinks;
                for(const auto& [id,item]:entries){
                    auto ancestor=item.entry.parent;
                    while(ancestor>2&&ancestor!=removed){
                        const auto found=entries.find(ancestor);
                        if(found==entries.end())break;
                        ancestor=found->second.entry.parent;
                    }
                    if(ancestor==removed){
                        descendants.push_back(id);
                        if(item.entry.hardLink)affectedLinks.insert(item.oid);
                    }
                }
                for(const auto& [id,item]:entries){
                    if(item.mount==deleted.mount&&affectedLinks.count(item.oid))descendants.push_back(id);
                }
                std::sort(descendants.begin(),descendants.end());
                descendants.erase(std::unique(descendants.begin(),descendants.end()),descendants.end());
                for(const auto id:descendants){
                    const auto& item=At(id);
                    writableIds.erase({item.mount,item.aliasId?item.aliasId:item.oid});
                    if(item.entry.hardLink)linkIdentities.erase({item.mount,item.oid});
                    entries.erase(id);
                }
            }
            writableIds.erase({deleted.mount,deleted.aliasId?deleted.aliasId:deleted.oid});
            entries.erase(removed);
            if(deleted.entry.hardLink){
                const auto count=deleted.entry.linkCount-1;
                linkIdentities.erase({deleted.mount,deleted.oid});
                for(auto& [id,item]:entries){
                    if(item.mount!=deleted.mount||item.oid!=deleted.oid)continue;
                    item.entry.linkCount=count;
                    item.entry.hardLink=count>1;
                    const auto resolved=linkIdentities.emplace(Address{item.mount,item.oid},id);
                    item.entry.resolvedId=resolved.first->second;
                    if(count==1){
                        if(item.aliasId){
                            writableIds.erase({item.mount,item.aliasId});
                            writableIds.emplace(Address{item.mount,item.oid},id);
                        }
                        item.aliasId=item.siblingId=0;
                    }
                }
            }
        }
        const auto refreshFolder=[&](uint32_t id){
            auto& folder=At(id);
            const auto changed=LoadInode(Catalog(*source,container,mounts[mountIndex]),mountIndex,folder.oid);
            folder.entry.modified=changed.entry.modified;
            ListDirectory(id);
        };
        if(oldParent)refreshFolder(oldParent);
        refreshFolder(parent);
        if(changedFile&&At(changedFile).entry.hardLink){
            const auto oid=At(changedFile).oid;
            std::set<uint32_t> folders;
            for(const auto& [id,item]:entries){
                if(item.mount==mountIndex&&item.oid==oid&&item.entry.parent!=parent)folders.insert(item.entry.parent);
            }
            for(const auto aliasFolder:folders)ListDirectory(aliasFolder);
        }
    }

    void RefreshSnapshotViews(uint64_t transaction,bool create){
        const auto fresh=Probe(*source);
        Check(fresh&&fresh->uuid==container.uuid&&fresh->volumes==container.volumes&&
              fresh->transaction==transaction,"The APFS image changed during snapshot management");
        const auto oid=fresh->volumes.front();
        const ObjectMap map(*source,*fresh,fresh->objectMap,transaction);
        const auto address=map.Lookup(oid);
        Check(address.has_value(),"Missing APFS live volume after snapshot management");
        auto live=ReadVolume(ReadObject(*source,*fresh,*address,oid,transaction,13),oid);
        auto snapshots=ReadSnapshots(*source,*fresh,live);
        Check(snapshots.size()==size_t(create),"Could not load the updated APFS snapshot view");
        freeBytes=WritableFreeBytes(*source,*fresh);

        uint32_t liveRoot=2;
        if(At(2).oid==0){
            const auto& roots=listings.at(2);
            Check(!roots.empty(),"Missing APFS live root");
            liveRoot=roots.front();
        }
        if(create)limitedEdits=true;
        if(create&&liveRoot==2){
            Check(next<=UINT32_MAX,"Too many APFS browser entries");
            liveRoot=uint32_t(next++);
            auto root=entries.extract(2);
            root.key()=liveRoot;
            root.mapped().entry.id=root.mapped().entry.resolvedId=liveRoot;
            root.mapped().entry.parent=2;
            entries.insert(std::move(root));
            for(auto& [id,item]:entries)
                if(id!=liveRoot&&item.entry.parent==2)item.entry.parent=liveRoot;
            Entry index;
            index.id=index.resolvedId=2;
            index.parent=1;
            index.directory=true;
            index.name=u"APFS Container";
            index.identity=MacBrowser::EntryIdentity(key);
            entries.emplace(2,Item{0,0,0,std::move(index),{}});
        }
        for(auto entry=entries.begin();entry!=entries.end();)
            if(entry->second.mount!=0)entry=entries.erase(entry);
            else ++entry;
        for(auto id=writableIds.begin();id!=writableIds.end();)
            if(id->first.first!=0)id=writableIds.erase(id);
            else ++id;
        for(auto id=linkIdentities.begin();id!=linkIdentities.end();)
            if(id->first.first!=0)id=linkIdentities.erase(id);
            else ++id;
        listings.clear();
        extents.clear();
        compressed.clear();
        container=*fresh;
        mounts.clear();
        mounts.push_back(std::move(live));
        if(create){
            Check(next<=UINT32_MAX,"Too many APFS browser entries");
            mounts.push_back(std::move(snapshots.front()));
            const auto& mount=mounts.back();
            auto root=LoadInode(Catalog(*source,container,mount),1,2);
            auto& entry=root.entry;
            const auto id=uint32_t(next++);
            entry.id=entry.resolvedId=id;
            entry.parent=2;
            entry.name=mount.name;
            entry.identity=MacBrowser::EntryIdentity(MountKey(mount),EntryVersion(mount));
            entry.created=entry.modified=*mount.snapshotCreated;
            entry.datesValid=true;
            entries.emplace(id,Item{1,2,root.stream,std::move(entry),std::move(root.attributes)});
            listings.emplace(2,std::vector<uint32_t>{liveRoot,id});
        }else if(liveRoot!=2){
            entries.erase(2);
            auto root=entries.extract(liveRoot);
            root.key()=2;
            root.mapped().entry.id=root.mapped().entry.resolvedId=2;
            root.mapped().entry.parent=1;
            entries.insert(std::move(root));
            for(auto& [id,item]:entries)
                if(item.entry.parent==liveRoot)item.entry.parent=2;
        }
    }

    Container WriteTarget(uint32_t parent)
    {
        const auto& folder=At(parent);
        Check(folder.oid&&folder.entry.directory,"Select an APFS volume or a folder");
        return WriteTarget(mounts[folder.mount]);
    }

    Container WriteTarget(const Mounted& mount)
    {
        Check(!mount.snapshotCreated,"APFS snapshots are read-only");
        Check(!mount.locked,"Unlock the APFS volume before writing");
        auto target=container;
        target.writeVolume=mount.oid;
        target.writeCipher=mount.cipher;
        for(const auto& sibling:mounts)
            if(sibling.cipher&&sibling.oid!=target.writeVolume)
                target.siblingCiphers.emplace(sibling.oid,sibling.cipher);
        return target;
    }

    template<class Commit,class Refresh>
    auto CompleteWrite(Commit commit,Refresh refresh,bool* publishChange=nullptr)
        ->decltype(refresh(uint64_t{}))
    {
        uint64_t transaction;
        try{transaction=commit();}
        catch(const CheckpointPublicationUncertain&){
            requiresReopen=true;
            if(publishChange)*publishChange=true;
            Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Unverified);
            throw;
        }
        if(publishChange)*publishChange=true;
        Storage::OutcomeEvidence::NoteRetainedChange();
        try{return refresh(transaction);}
        catch(const std::exception& error){
            requiresReopen=true;
            throw Error(std::string("APFS change committed. Reopen the image to refresh it. ")+error.what());
        }
    }

    template<class Work>void CommitMutation(uint32_t parent,uint32_t removed,Storage::Job& job,Work work,
                                            uint32_t changedFile=0,uint32_t oldParent=0)
    {
        CompleteWrite([&]{return work(job);},[&](uint64_t transaction){
            RefreshNamespace(parent,removed,transaction,changedFile,oldParent);
        });
    }

    uint32_t CreateFolder(uint32_t parent,const std::u16string& name,std::optional<FileDates> dates,const FinderBytes* finder=nullptr,
                          const Attributes* attributes=nullptr,std::optional<PosixMetadata> posix={}){
        return WriteMount([&]{
            const auto oid=At(parent).oid;
            Storage::Job job;
            CommitMutation(parent,0,job,[&](auto& job){return CreateCatalogDirectory(*source,WriteTarget(parent),oid,name,job,dates,finder,attributes,limitedEdits,posix);});
            for(const auto id:listings.at(parent))if(At(id).entry.name==name)return id;
            throw Error("APFS folder was created but could not be found after reopening");
        },true);
    }
    static std::optional<FileDates> CopyDates(const Entry& entry){
        if(!entry.datesValid)return {};
        return FileDates{entry.created,entry.modified,entry.additionalDates};
    }
public:
    uint32_t MakeHardLink(uint32_t id,uint32_t parent,const std::u16string& name,bool replace=false)override{
        Storage::Job job;
        return MakeHardLinkForTransfer(id,parent,name,replace,job);
    }
    uint32_t MakeHardLinkForTransfer(uint32_t id,uint32_t parent,const std::u16string& name,
                                     bool replace,Storage::Job& job)override{
        return LinkForTransfer(id,parent,name,replace,job,nullptr,nullptr);
    }
    uint32_t MakeCopiedHardLinkForTransfer(const Entry& copied,uint32_t parent,const std::u16string& name,
                                          const Entry* approvedDestination,Storage::Job& job)override{
        return LinkForTransfer(copied.id,parent,name,approvedDestination!=nullptr,job,&copied,approvedDestination);
    }
private:
    uint32_t LinkForTransfer(uint32_t id,uint32_t parent,const std::u16string& name,
                             bool replace,Storage::Job& job,const Entry* copied,const Entry* approvedDestination){
        return WriteMount([&]{
            const auto& current=At(id).entry;
            if(copied)Check(current.identity==copied->identity&&current.protectedFile==copied->protectedFile&&
                            current.posix==copied->posix,
                            "The copied APFS hard-link source changed");
            if(job.cancelled())throw Storage::Cancelled();
            Check(At(id).mount==At(parent).mount,"APFS hard links cannot cross volumes");
            const auto oid=At(id).oid,folder=At(parent).oid;
            const auto sourceParent=At(id).entry.parent;
            const auto links=At(id).entry.linkCount;
            Entry destination;
            uint64_t replaced=0;
            if(replace){
                Check(FindChild(parent,name,destination),"The APFS hard-link destination disappeared");
                Check(!approvedDestination||destination.identity==approvedDestination->identity,
                      "The APFS hard-link destination changed after overwrite approval");
                replaced=At(destination.id).oid;
                if(replaced==oid)return destination.id;
            }
            CommitMutation(parent,destination.id,job,[&](auto& job){
                return CreateCatalogHardLink(*source,WriteTarget(parent),folder,oid,name,job,replaced,
                                             limitedEdits||replace,copied!=nullptr);
            });
            if(sourceParent!=parent)ListDirectory(sourceParent);
            for(auto& [row,item]:entries){
                if(item.mount==At(parent).mount&&item.oid==oid)item.entry.linkCount=links+1;
            }
            Entry alias;
            if(FindChild(parent,name,alias))return alias.id;
            throw Error("APFS hard link committed but could not be found after refresh");
        },true);
    }
public:
    uint32_t MakeDirectory(uint32_t parent,const std::u16string& name)override{
        return CreateFolder(parent,name,{});
    }
    uint32_t MakeDirectoryWithAttributes(uint32_t parent,const Entry& entry)override{
        ValidateEntry(entry);
        Check(entry.directory,"The APFS copy source is not a folder");
        return CreateFolder(parent,entry.name,CopyDates(entry),&entry.finder,&entry.attributes,entry.posix);
    }
    void FinalizeDirectory(uint32_t id,const Entry& desired,const Entry& expected,bool merge)override{
        const auto dates=CopyDates(desired);
        const bool lockAfterCopy=!merge&&desired.protectedFile;
        if(!dates&&!lockAfterCopy)return;
        WriteMount([&]{
            const auto& item=At(id);
            const auto& current=item.entry;
            Check(current.directory&&current.identity==expected.identity&&current.parent==expected.parent&&
                  current.name==expected.name,"The APFS destination folder changed during copying");
            TimestampEdits changes;
            if(dates){
                if(!current.datesValid||current.created!=dates->created)changes.created=dates->created;
                if(!current.datesValid||current.modified!=dates->modified)changes.modified=dates->modified;
                if(current.additionalDates.accessed!=dates->additionalDates.accessed)
                    changes.additionalDates.accessed=dates->additionalDates.accessed;
                if(current.additionalDates.changed!=dates->additionalDates.changed)
                    changes.additionalDates.changed=dates->additionalDates.changed;
            }
            if(!changes.created&&!changes.modified&&!changes.additionalDates.accessed&&
               !changes.additionalDates.changed&&!lockAfterCopy)return;
            const auto parent=current.parent;
            const auto parentOid=At(parent).oid,oid=item.oid;
            // Restore destination folder dates even if the transfer was cancelled.
            Storage::Job metadata;
            CommitMutation(parent,0,metadata,[&](auto& job){
                if(limitedEdits)
                    return SetExternalCatalogProperties(*source,WriteTarget(parent),parentOid,current.name,changes,job,
                                                        lockAfterCopy?std::optional<bool>(true):std::nullopt,nullptr,
                                                        CopiedProtectionFlags(desired.posix));
                if(lockAfterCopy)
                    return SetEntryProperties(*source,WriteTarget(parent),parentOid,oid,current.finder,changes,true,job,
                                              CopiedProtectionFlags(desired.posix));
                return SetDirectoryDates(*source,WriteTarget(parent),parentOid,oid,*dates,job);
            });
        },true);
    }
    void ValidateName(const std::u16string& name)const override{EncodeCatalogName(name);}
    bool CanReplaceFileWithSymbolicLink(const Entry& entry)const override{
        return writable&&!entry.directory&&!entry.symbolicLink&&
               !entry.protectedFile&&entry.readError.empty();
    }
    void ValidateSymbolicLink(const Entry& entry,const Bytes& target)const override{
        Check(!entry.directory&&!entry.hardLink&&!entry.compressed&&!entry.forks[0].size&&!entry.forks[1].size,
              "Unsupported APFS symbolic link metadata");
        Check(target.size()<EmbeddedAttributeLimit,"APFS symbolic link target does not fit in an embedded attribute");
        Apfs::Name(target.data(),target.size());
        auto metadata=entry;
        metadata.readError.clear();
        ValidateEntry(metadata);
    }
    void PutSymbolicLink(uint32_t parent,const Entry& entry,const Bytes& target,Storage::Job& job)override{
        ValidateSymbolicLink(entry,target);
        WriteMount([&]{
            const auto parentOid=At(parent).oid;
            Entry child;
            uint64_t replacedEntry=0;
            if(!entry.expectedDestinationIdentity.empty()){
                Check(FindChild(parent,entry.name,child)&&!child.protectedFile&&
                      child.identity==entry.expectedDestinationIdentity,
                      "The APFS symbolic link destination changed after overwrite approval");
                if(!child.symbolicLink){
                    Check(CanReplaceFileWithSymbolicLink(child),"The APFS symbolic link cannot replace this entry");
                    replacedEntry=At(child.id).oid;
                }
            }
            CommitMutation(parent,replacedEntry?child.id:0,job,[&](auto& job){
                if(child.symbolicLink)
                    return ReplaceCatalogSymlink(*source,WriteTarget(parent),parentOid,At(child.id).oid,target,job,
                                                 CopyDates(entry),entry.finder,entry.protectedFile,entry.attributes,limitedEdits,entry.posix);
                return CreateCatalogSymlink(*source,WriteTarget(parent),parentOid,entry.name,target,job,
                                            CopyDates(entry),entry.finder,entry.protectedFile,entry.attributes,
                                            limitedEdits||replacedEntry,replacedEntry,entry.posix);
            });
        },true);
    }
    void ValidateEntry(const Entry& entry)const override{
        if(!writable)ReadOnly();
        ValidateName(entry.name);
        Check(!entry.directory||!entry.forks[0].size,"An APFS folder cannot store file data");
        Check(!entry.directory||!entry.forks[1].size,"An APFS folder cannot store a resource fork");
        Check(entry.readError.empty()&&
              (!entry.directory||(!entry.hardLink&&!entry.compressed))&&!entry.version,
              "This APFS writer cannot preserve the source entry's Mac metadata");
        ValidateWritableAttributes(entry.attributes);
        Check(!entry.posix||entry.posix->mode<=PosixMetadata::ModeMask,"Invalid POSIX permissions");
        if(entry.datesValid){
            CatalogTimestamp(entry.created);
            CatalogTimestamp(entry.modified);
        }
        if(entry.additionalDates.accessed)CatalogTimestamp(*entry.additionalDates.accessed);
        if(entry.additionalDates.changed)CatalogTimestamp(*entry.additionalDates.changed);
    }
    void ValidateCopyNames(uint32_t parent,const std::vector<Entry>& entries,bool)override{
        // A not-yet-created folder has the same comparison rules as its ancestor.
        const auto fold=Access([&]{
            const auto& folder=At(parent);
            Check(folder.oid&&folder.entry.directory,"Select an APFS volume or a folder");
            const auto& mount=mounts[folder.mount];
            Check(!mount.snapshotCreated,"APFS snapshots are read-only");
            return !mount.sensitive;
        });
        std::set<std::u32string> names;
        for(const auto& entry:entries){
            // The common planner skips per-entry validation for folder merges.
            if(entry.directory)ValidateEntry(entry);
            Check(names.insert(NormalizedName(entry.name,fold)).second,
                  "More than one file has the same APFS destination name");
        }
    }
    uint32_t Put(uint32_t parent,const Entry& entry,MacBrowser::Reader reader,bool replace)override{
        Storage::Job job;
        return PutFile(parent,entry,std::move(reader),replace,job,false);
    }
    uint32_t PutFromHost(uint32_t parent,const Entry& entry,MacBrowser::Reader reader,bool replace,
                         const std::wstring&,Storage::Job& job)override{
        return PutForTransfer(parent,entry,std::move(reader),replace,job);
    }
    class ForkInput final:public Source {
        const uint64_t size;
        const unsigned fork;
        const MacBrowser::Reader& read;
        const std::vector<SparseRange> holes;
    public:
        ForkInput(uint64_t length,unsigned selector,const MacBrowser::Reader& reader,std::vector<SparseRange> sparse={}):
            size(length),fork(selector),read(reader),holes(std::move(sparse)){}
        std::vector<SparseRange> KnownZeroRanges()const override{return holes;}
        uint64_t Size()const override{return size;}
        bool Writable()const override{return false;}
        void Read(uint64_t at,void* bytes,size_t length)override{
            Check(Range(at,length,size)&&bool(read),"Invalid APFS input fork request");
            read(fork,at,bytes,length);
        }
        void Write(uint64_t,const void*,size_t)override{ReadOnly();}
        void Flush()override{}
    };
    uint32_t PutForTransfer(uint32_t parent,const Entry& entry,MacBrowser::Reader reader,bool replace,
                            Storage::Job& job)override{
        return PutFile(parent,entry,std::move(reader),replace,job,true);
    }
    uint32_t PutFile(uint32_t parent,const Entry& entry,MacBrowser::Reader reader,bool replace,
                     Storage::Job& job,bool allowPrompt){
        ValidateEntry(entry);
        Check(!entry.directory,"Select a file for APFS transfer");
        const bool special=entry.posix&&entry.posix->IsSpecial();
        Check(!special||(!entry.compressed&&!entry.hardLink&&
              !entry.forks[0].size&&!entry.forks[1].size),
              "Special file copies cannot contain forks");
        const auto dates=CopyDates(entry);
        const auto* attributes=entry.attributesKnown||!entry.attributes.empty()?&entry.attributes:nullptr;
        std::optional<HfsPlus::EncodedCompression> encoded;
        const Bytes* compression=nullptr;
        const auto inlineLimit=InlineCompressionCapacity();
        if(entry.WritesStoredCompressionResource()){
            ValidateResourceCompression(entry.resourceCompressionHeader,entry.forks[0].size,entry.forks[1].size,
                [&](uint64_t offset,void* bytes,size_t size){
                    if(job.cancelled())throw Storage::Cancelled();
                    reader(1,offset,bytes,size);
                });
            compression=&entry.resourceCompressionHeader;
        }else if(entry.compressed&&!entry.inlineCompression.empty()&&entry.inlineCompression.size()<=inlineLimit){
            ValidateInlineCompression(entry.inlineCompression,entry.forks[0].size);
            compression=&entry.inlineCompression;
        }else if(entry.compressed){
            encoded.emplace(entry.forks[0].size,[&](uint64_t offset,void* bytes,size_t count){
                if(job.cancelled())throw Storage::Cancelled();
                reader(0,offset,bytes,count);
            },HfsPlus::Volume::CompressionMode::Zlib,entry.forks[1].size?
                EncodedCompression::Layout::InlineOnly:EncodedCompression::Layout::InlineOrResource,inlineLimit);
            if(encoded->Attribute().empty()){
                Check(allowPrompt,"Copying both forks without compression requires approval");
                if(job.requestChoice(Storage::CopyPrompt::Uncompressed,EncodeCatalogName(entry.name))!=
                   Storage::ConflictChoice::Replace||job.cancelled())
                    throw Storage::Cancelled();
                encoded.reset();
            }else compression=&encoded->Attribute();
        }
        const bool compressionResource=encoded&&encoded->UsesResource();
        const MacBrowser::Reader payload=[&](unsigned fork,uint64_t offset,void* bytes,size_t count){
            if(compressionResource&&fork==1)encoded->ReadResource(offset,bytes,count);
            else reader(fork,offset,bytes,count);
        };
        auto sparse=MacBrowser::SparseForks{};
        if(allowPrompt&&!compression&&!special)
            sparse=MacBrowser::ValidateApfsSparseLayout(entry,reader,job);
        const bool restoreSparse=!sparse[0].empty()&&std::all_of(sparse[0].begin(),sparse[0].end(),[&](const auto& hole){
            constexpr auto block=WriterDisk::Block;
            return !(hole.offset%block)&&(!(hole.length%block)||hole.offset+hole.length==entry.forks[0].size);
        });
        ForkInput data(compression?0:entry.forks[0].size,0,payload,
                       restoreSparse?std::move(sparse[0]):std::vector<SparseRange>{});
        ForkInput resource(compressionResource?encoded->ResourceSize():entry.forks[1].size,1,payload);
        return WriteMount([&]{
            const auto parentOid=At(parent).oid;
            Entry existing;
            if(FindChild(parent,entry.name,existing)){
                Check(replace&&!existing.directory&&!existing.protectedFile&&
                      (existing.symbolicLink||existing.readError.empty()),
                      "The APFS destination already exists or cannot be replaced");
                Check(entry.expectedDestinationIdentity.empty()||entry.expectedDestinationIdentity==existing.identity,
                      "The APFS destination changed after overwrite approval");
                const auto oid=At(existing.id).oid,sibling=At(existing.id).siblingId;
                // An approved copy targets a name. In-place edits still use ReplaceFileData.
                const bool replaceAlias=existing.hardLink&&!entry.expectedDestinationIdentity.empty();
                const bool existingSpecial=existing.posix&&existing.posix->IsSpecial();
                if(special||existingSpecial)
                    Check(((!existing.hardLink&&!sibling)||(special&&!existingSpecial&&replaceAlias&&sibling))&&
                          (!existingSpecial||(!existing.forks[0].size&&!existing.forks[1].size)),
                          "The APFS special file destination changed after overwrite approval");
                if(existing.symbolicLink||replaceAlias||special||existingSpecial||restoreSparse){
                    Check(!replaceAlias||sibling,"The APFS hard-link destination has no sibling ID");
                    const auto& name=replaceAlias?existing.name:entry.name;
                    CommitMutation(parent,existing.id,job,[&](auto& job){
                        return CreateCatalogFile(*source,WriteTarget(parent),parentOid,name,data,job,dates,
                                                 &entry.finder,entry.protectedFile,attributes,&resource,compression,
                                                 true,oid,entry.posix,replaceAlias?sibling:0);
                    });
                    if(restoreSparse)limitedEdits=true;
                    Entry replacement;
                    if(FindChild(parent,entry.name,replacement))return replacement.id;
                    throw Error("APFS replacement was committed but could not be found after reopening");
                }
                CommitMutation(parent,0,job,[&](auto& job){
                    return ReplaceFileData(*source,WriteTarget(parent),parentOid,oid,data,job,dates,&entry.finder,entry.protectedFile,attributes,&resource,compression,limitedEdits,sibling,false,entry.posix);
                },existing.id);
                return existing.id;
            }
            Check(entry.expectedDestinationIdentity.empty(),"The APFS destination disappeared after overwrite approval");
            CommitMutation(parent,0,job,[&](auto& job){
                return CreateCatalogFile(*source,WriteTarget(parent),parentOid,entry.name,data,job,dates,&entry.finder,entry.protectedFile,attributes,&resource,compression,limitedEdits||restoreSparse,0,entry.posix);
            });
            if(restoreSparse)limitedEdits=true;
            if(FindChild(parent,entry.name,existing))return existing.id;
            throw Error("APFS file was committed but could not be found after reopening");
        },true);
    }
    uint32_t PutResourceReplacement(uint32_t id,const Entry& desired,MacBrowser::Reader reader,
                                     std::function<void(uint64_t,size_t)> keptData)override{
        Storage::Job job;
        return PutResourceReplacementForJob(id,desired,std::move(reader),std::move(keptData),job);
    }
    uint32_t PutResourceReplacementForJob(uint32_t id,const Entry& desired,MacBrowser::Reader reader,
                                           std::function<void(uint64_t,size_t)> keptData,Storage::Job& job)override{
        ForkInput resource(desired.forks[1].size,1,reader);
        return WriteMount([&]{
            const auto& item=At(id);
            const auto& current=item.entry;
            Check(!current.directory&&!current.protectedFile&&!current.compressed&&current.readError.empty()&&
                  current.identity==desired.identity&&current.parent==desired.parent&&current.name==desired.name,
                  "The APFS resource fork destination changed or cannot be edited");
            if(job.cancelled())throw Storage::Cancelled();
            const auto parent=current.parent;
            const auto& folder=At(parent);
            Check(!folder.entry.protectedFile,"The APFS destination folder is locked");
            if(keptData&&current.forks[0].size)keptData(0,size_t(current.forks[0].size));
            const auto oid=item.oid;
            CommitMutation(parent,0,job,[&](auto& job){
                return SetResourceFork(*source,WriteTarget(parent),folder.oid,oid,resource,desired.modified,job,limitedEdits);
            },id);
            return id;
        },true);
    }
    void SetCompressionMethod(uint32_t id,HfsPlus::Volume::CompressionMode method,
                              std::function<void(uint64_t,uint64_t)> progress)override{
        Storage::Job job;
        SetCompressionMethodForJob(id,method,std::move(progress),job);
    }
    void SetCompressionMethodForJob(uint32_t id,HfsPlus::Volume::CompressionMode method,
                                    std::function<void(uint64_t,uint64_t)> progress,Storage::Job& job)override{
        WriteMount([&]{
            if(job.cancelled())throw Storage::Cancelled();
            const auto& item=At(id);
            const auto current=item.entry;
            Check(!current.directory&&!current.protectedFile&&current.readError.empty(),
                  "Select an unlocked APFS regular file to change compression");
            const bool compress=method!=HfsPlus::Volume::CompressionMode::Uncompressed;
            if(!compress&&!current.compressed)return;
            Check(!compress||!current.forks[1].size,"APFS cannot retain a user resource fork on a compressed file");
            const auto size=current.forks[0].size;
            uint64_t done=0;
            const auto report=[&]{
                if(job.cancelled())throw Storage::Cancelled();
                if(progress)progress(done,size);
                if(job.cancelled())throw Storage::Cancelled();
            };
            const HfsPlus::EncodedCompression::Reader logical=[&](uint64_t offset,void* bytes,size_t count){
                Read(id,0,offset,bytes,count);
                done=(std::max)(done,offset+count);
                report();
            };
            std::optional<HfsPlus::EncodedCompression> encoded;
            if(compress)encoded.emplace(size,logical,method);
            const bool compressionResource=encoded&&encoded->UsesResource();

            const bool oldCompressionResource=current.compressed&&!current.forks[1].size&&
                                              item.attributes.count(ResourceFork)!=0;
            const auto resourceSize=compressionResource?encoded->ResourceSize():current.forks[1].size;
            const MacBrowser::Reader read=[&](unsigned fork,uint64_t offset,void* bytes,size_t count){
                if(fork){
                    if(compressionResource)encoded->ReadResource(offset,bytes,count);
                    else Read(id,1,offset,bytes,count);
                    report();
                }else logical(offset,bytes,count);
            };
            ForkInput data(compress?0:size,0,read);
            ForkInput resource(resourceSize,1,read);
            const auto parent=current.parent;
            const auto parentOid=At(parent).oid,oid=item.oid;
            const auto sibling=item.siblingId;
            report();
            CommitMutation(parent,0,job,[&](auto& job){
                return ReplaceFileData(*source,WriteTarget(parent),parentOid,oid,data,job,{},
                                       nullptr,false,nullptr,limitedEdits||resourceSize||oldCompressionResource?&resource:nullptr,
                                       encoded?&encoded->Attribute():nullptr,limitedEdits,sibling,true);
            },id);
        },true);
    }
    void SetAttributes(uint32_t id,const Attributes& attributes)override{
        Storage::Job job;
        SetAttributesForJob(id,attributes,job);
    }
    void SetAttributesForJob(uint32_t id,const Attributes& attributes,Storage::Job& job)override{
        ValidateWritableAttributes(attributes);
        WriteMount([&]{
            if(job.cancelled())throw Storage::Cancelled();
            Check(id!=2,"APFS root attribute editing is unavailable");
            const auto& item=At(id);
            const auto parent=item.entry.parent;
            const auto& folder=At(parent);
            Check(!folder.entry.protectedFile,"The APFS destination folder is locked");
            const auto oid=item.oid,parentOid=folder.oid;
            CommitMutation(parent,0,job,[&](auto& job){
                return SetCatalogAttributes(*source,WriteTarget(parent),parentOid,oid,attributes,job,limitedEdits);
            },id);
        },true);
    }
    void SetBasicMetadata(uint32_t id,const Entry& desired)override{
        WriteMount([&]{
            const auto& item=At(id);
            const auto& current=item.entry;
            Check(id!=2&&current.name==desired.name&&current.directory==desired.directory&&
                  current.version==desired.version,
                  "APFS Properties can only change Finder information and timestamps on files and folders");
            Check(current.datesValid==desired.datesValid,"APFS timestamps cannot be removed");
            TimestampEdits dates;
            if(current.created!=desired.created)dates.created=desired.created;
            if(current.modified!=desired.modified)dates.modified=desired.modified;
            if(current.protectedFile==desired.protectedFile&&current.finder==desired.finder&&!dates.created&&!dates.modified)return;
            const auto parent=current.parent;
            const auto parentOid=At(parent).oid,oid=item.oid;
            Storage::Job job;
            CommitMutation(parent,0,job,[&](auto& job){
                if(limitedEdits){
                    const auto locked=current.protectedFile!=desired.protectedFile?
                        std::optional<bool>(desired.protectedFile):std::nullopt;
                    const auto* finder=current.finder!=desired.finder?&desired.finder:nullptr;
                    return SetExternalCatalogProperties(*source,WriteTarget(parent),parentOid,current.name,dates,job,locked,finder);
                }
                return SetEntryProperties(*source,WriteTarget(parent),parentOid,oid,desired.finder,dates,desired.protectedFile,job);
            },id);
        },true);
    }
    uint32_t TryCloneFileForTransfer(MacBrowser::Volume& from,const Entry& entry,uint32_t parent,Storage::Job& job)override{
        if(&from!=this||!writable||!limitedEdits||entry.directory||entry.symbolicLink||
           entry.hardLink||entry.compressed||entry.protectedFile||!entry.forks[0].size||
           entry.forks[1].size||!entry.attributes.empty())return 0;
        return WriteMount([&]{
            const auto& original=At(entry.id);
            const auto& target=At(parent);
            if(original.mount!=target.mount||mounts[original.mount].snapshotCreated||
               mounts[original.mount].cipher)return uint32_t(0);
            Check(original.entry.identity==entry.identity,"The APFS clone source changed");
            const auto inode=original.oid,parentOid=target.oid;
            try{
                CommitMutation(parent,0,job,[&](auto& work){
                    return CreateCatalogClone(*source,WriteTarget(parent),parentOid,entry.name,inode,
                                              entry.forks[0].size,CopyDates(entry),entry.finder,work);
                });
            }catch(const CloneUnavailable&){
                return uint32_t(0);
            }
            Entry copied;
            Check(FindChild(parent,entry.name,copied),"APFS clone was committed but could not be found");
            return copied.id;
        },true);
    }
    bool CanMoveEntryFrom(const MacBrowser::Volume& source,uint32_t id,uint32_t parent)override{
        if(&source!=this||!writable||!limitedEdits)return false;
        return Access([&]{
            const auto& item=At(id);
            const auto& folder=At(parent);
            return item.oid>2&&item.entry.parent!=parent&&
                   (item.symlink||item.entry.readError.empty())&&folder.oid&&folder.entry.directory&&
                   item.mount==folder.mount&&!mounts[item.mount].snapshotCreated;
        });
    }
    void MoveHereForJob(MacBrowser::Volume& source,uint32_t id,uint32_t parent,const Entry& entry,Storage::Job& job)override{
        WriteMount([&]{
            if(job.cancelled())throw Storage::Cancelled();
            Check(CanMoveEntryFrom(source,id,parent),"The APFS entry cannot be moved in place");
            const auto current=At(id).entry;
            Check(current.identity==entry.identity&&current.name==entry.name,"The APFS move source changed");
            const auto oldParentOid=At(current.parent).oid,destinationOid=At(parent).oid;
            CommitMutation(parent,0,job,[&](auto& work){
                return RenameExternalCatalogEntry(*this->source,WriteTarget(parent),oldParentOid,
                                                  current.name,current.name,work,destinationOid);
            },0,current.parent);
        },true);
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{
        Storage::Job job;
        RenameForJob(id,parent,name,job);
    }
    void RenameForJob(uint32_t id,uint32_t parent,const std::u16string& name,Storage::Job& job)override{
        WriteMount([&]{
            if(job.cancelled())throw Storage::Cancelled();
            const auto& entry=At(id);
            Check(entry.entry.parent==parent,"APFS rename must stay in the original folder");
            const auto oid=entry.oid,parentOid=At(parent).oid;
            CommitMutation(parent,0,job,[&](auto& job){
                if(limitedEdits)return RenameExternalCatalogEntry(*source,WriteTarget(parent),parentOid,entry.entry.name,name,job);
                return RenameEntry(*source,WriteTarget(parent),parentOid,oid,name,job,entry.aliasId);
            });
        },true);
    }
    bool RemoveTreeAtomically(uint32_t id,Storage::Job& job,bool)override{
        if(!limitedEdits)return false;
        WriteMount([&]{
            const auto& target=At(id);
            Check(id!=2,"Cannot delete the APFS root");
            const auto parent=target.entry.parent;
            const auto parentOid=At(parent).oid,oid=target.oid,sibling=target.siblingId;
            if(target.entry.directory){
                // Cache descendant identities so aliases outside the removed tree
                // can be invalidated when their link counts change.
                std::vector<uint32_t> folders{id};
                for(size_t i=0;i<folders.size();++i){
                    if(job.cancelled())throw Storage::Cancelled();
                    for(const auto& entry:ListDirectory(folders[i]))
                        if(entry.directory)folders.push_back(entry.id);
                }
            }
            CommitMutation(parent,id,job,[&](auto& work){
                return DeleteEntry(*source,WriteTarget(parent),parentOid,oid,work,sibling,true,true);
            });
        },true);
        return true;
    }
    void ValidateRemoval(uint32_t id)override{
        Access([&]{
            Check(writable&&id!=2,"APFS entry is read-only");
            const auto& item=At(id);
            Check(!mounts[item.mount].snapshotCreated,"APFS snapshots are read-only");
            const auto& entry=item.entry;
            Check(!entry.protectedFile&&!At(entry.parent).entry.protectedFile,"APFS entry or its parent folder is locked");
            if(limitedEdits)
                Check((entry.readError.empty()||At(id).symlink)&&
                      (!entry.directory||ListDirectory(id).empty()),
                      "This APFS image can only delete supported regular files, symbolic links, and empty folders");
            return true;
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
            CommitMutation(parent,id,job,[&](auto& job){return DeleteEntry(*source,WriteTarget(parent),parentOid,oid,job,entry.siblingId,limitedEdits);});
        },true);
    }
};
}

LockedVolume::LockedVolume(bool again,std::wstring passwordHint)
    :HfsPlus::Error(again?"The password did not unlock the APFS volume":LockedMessage),
     retry(again),hint(std::move(passwordHint)){}

std::vector<std::u16string> ListSnapshotNames(Source& source,const Container& container,std::u16string* volumeName)
{
    Check(!container.offset&&container.volumes.size()==1,"Snapshot management requires a single-volume raw APFS image");
    const auto oid=container.volumes.front();
    const ObjectMap map(source,container,container.objectMap,container.transaction);
    const auto address=map.Lookup(oid);
    Check(address.has_value(),"Missing APFS snapshot volume");
    const auto bytes=ReadObject(source,container,*address,oid,container.transaction,13);
    const auto live=ReadVolume(bytes,oid);
    Check(!live.locked,"Snapshot management requires an unencrypted APFS volume");
    if(volumeName)*volumeName=live.name;
    std::vector<Snapshot> snapshots;
    if(live.snapshotTree){
        std::set<uint64_t> visited;
        ReadSnapshotRecords(source,container,live,live.snapshotTree,{},TreeLayout{},visited,snapshots);
    }
    Check(snapshots.size()==Le64(bytes.data()+216)&&snapshots.size()<=1,
          "Snapshot management requires at most one supported data-bearing snapshot");
    std::vector<std::u16string> names;
    for(auto& snapshot:snapshots)names.push_back(std::move(snapshot.name));
    return names;
}

std::vector<std::u16string> MountedSnapshotNames(MacBrowser::Volume& volume,std::u16string* volumeName)
{
    auto* mounted=dynamic_cast<Volume*>(&volume);
    Check(mounted!=nullptr,"Select an APFS image");
    return mounted->SnapshotNames(volumeName);
}

std::vector<std::u16string> ChangeMountedSnapshot(MacBrowser::Volume& volume,const std::u16string& name,bool create,Storage::Job& job)
{
    auto* mounted=dynamic_cast<Volume*>(&volume);
    Check(mounted!=nullptr,"Select an APFS image");
    return mounted->ChangeSnapshot(name,create,job);
}

std::vector<uint32_t> RefreshedFolderRoute(MacBrowser::Volume& volume,const std::vector<uint32_t>& previous)
{
    auto* mounted=dynamic_cast<Volume*>(&volume);
    return mounted?mounted->FolderRoute(previous):std::vector<uint32_t>{};
}

bool CanRenameMountedVolume(MacBrowser::Volume& volume,uint32_t root)
{
    auto* mounted=dynamic_cast<Volume*>(&volume);
    if(!mounted)return false;
    try{return mounted->CanRenameRoot(root);}
    catch(const VolumeCoord::Busy&){return false;}
}

void RenameMountedVolume(MacBrowser::Volume& volume,uint32_t root,const std::u16string& name,Storage::Job& job)
{
    auto* mounted=dynamic_cast<Volume*>(&volume);
    Check(mounted!=nullptr,"Select a live APFS volume");
    mounted->RenameRoot(root,name,job);
}

bool CanRestoreSnapshotFolder(MacBrowser::Volume& volume,uint32_t folder)
{
    auto* mounted=dynamic_cast<Volume*>(&volume);
    if(!mounted)return false;
    try{return mounted->CanRestoreFolder(folder);}
    catch(const VolumeCoord::Busy&){return false;}
}
MacBrowser::Entry SnapshotRestoreDestination(MacBrowser::Volume& volume,uint32_t folder)
{
    auto* mounted=dynamic_cast<Volume*>(&volume);
    Check(mounted!=nullptr,"Select an APFS snapshot");
    return mounted->RestoreDestination(folder);
}

void Unlock(MacBrowser::Volume& volume,uint32_t id,const Dmg::Password& password)
{
    const auto apfs=dynamic_cast<Volume*>(&volume);
    Check(apfs!=nullptr,"Select an encrypted APFS volume");
    apfs->Unlock(id,password);
}

static std::shared_ptr<MacBrowser::Volume> OpenMounted(std::unique_ptr<Source>& source,const std::string& key,
                                                    const Container& container,bool writable,uint64_t freeBytes=0,bool limitedEdits=false)
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
    return std::make_shared<Volume>(std::move(source),container,std::move(volumes),key,writable,freeBytes,limitedEdits);
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
    const auto whole=Probe(*source);
    if(!whole)return {};
    const auto writablePartition=[&](const Container& container)->std::optional<DiskPartitions::MacPartition>{
        if(!container.offset)return {};
        const auto partitions=DiskPartitions::ProbeGptImage(*source,Disk::Read(*source,0,512),
                                                            DiskPartitions::GptType::AppleApfs);
        if(!partitions||partitions->size()!=1)return {};
        const auto& partition=partitions->front();
        if(partition.readOnly||partition.offset!=container.offset||
           partition.length!=container.blockCount*container.blockSize)return {};
        return partition;
    };
    const auto partition=writablePartition(*whole);
    if(whole->offset&&!partition)return OpenMounted(source,key,*whole,false);

    const auto slice=[&](bool writable){
        if(partition){
            auto disk=std::shared_ptr<Source>(std::move(source));
            source=std::make_unique<DiskImage::Slice>(std::move(disk),partition->offset,partition->length,writable);
        }
    };
    slice(false);
    const auto expected=partition?Probe(*source):whole;
    Check(bool(expected),"The APFS container could not be opened inside the selected partition");
    const bool encrypted=expected->keybagBlock&&expected->keybagBlocks&&
                         expected->blockSize==4096;
    bool limitedEdits=false;
    if(!encrypted)try{
        ValidateWritableLayout(*source,*expected);
        WritableFreeBytes(*source,*expected);
    }catch(const Error&){
        try{
            if(expected->volumes.size()==1)InspectFreeQueueAllocation(*source,*expected);
            else{
                const ObjectMap map(*source,*expected,expected->objectMap,expected->transaction);
                for(const auto oid:expected->volumes){
                    InspectFreeQueueAllocation(*source,*expected,oid);
                    const auto block=map.Lookup(oid);
                    Check(block.has_value(),"Missing writable APFS volume");
                    ReadVolume(ReadObject(*source,*expected,*block,oid,expected->transaction,13),oid);
                }
            }
            limitedEdits=true;
        }catch(const Error&){
            return OpenMounted(source,key,*expected,false);
        }
    }

    const auto verify=[&]{
        const auto mounted=Probe(*source);
        if(!mounted||mounted->uuid!=whole->uuid||mounted->transaction!=whole->transaction||
           mounted->checkpointBlock!=whole->checkpointBlock||mounted->offset!=whole->offset||
           mounted->blockCount!=whole->blockCount||mounted->blockSize!=whole->blockSize||
           !(writablePartition(*mounted)==partition))
            throw Storage::MediaChanged("The selected APFS checkpoint or partition changed while opening the image");
    };
    source.reset();
    try{
        source=reopen(true);
        verify();
        slice(true);
        if(encrypted){
            const auto free=WritableFreeBytes(*source,*expected);
            return OpenMounted(source,key,*expected,false,free,true);
        }
        if(limitedEdits){
            const auto free=WritableFreeBytes(*source,*expected);
            source->Flush();
            return OpenMounted(source,key,*expected,true,free,true);
        }
        return OpenWritable(source,key);
    }catch(const Storage::MediaChanged&){
        throw;
    }catch(const Error&){
        source.reset();
        source=reopen(false);
        verify();
        slice(false);
        return OpenMounted(source,key,*expected,false);
    }
}
}
