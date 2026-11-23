// SPDX-License-Identifier: GPL-2.0-or-later
#include "ApfsCatalogWrite.h"
#include "ApfsCrypto.h"
#include "ApfsTree.h"
#include "ApfsWriteDisk.h"
#include <windows.h>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>

namespace Apfs {
namespace {
using HfsPlus::Source;
using HfsPlus::Disk::Range;
using WriterDisk::Block;
using WriterDisk::Bytes;
using WriterDisk::Check;
using WriterDisk::DivideUp;
using WriterDisk::EncodeLeaf;
using WriterDisk::Set;
using OwnedRecord=WriterDisk::Record;
using Writes=std::vector<CheckpointBlockWrite>;
constexpr uint64_t Physical=0x40000000;
constexpr uint64_t IdMask=(uint64_t(1)<<60)-1;
constexpr size_t SpaceReserveCount=184,SpaceReserveAllocated=192;

bool IsSpecialKind(CatalogKind kind)
{
    return SpecialType(unsigned(kind))!=SpecialFileType::None;
}
uint16_t EntryMode(CatalogKind kind)
{
    const unsigned permissions=kind==CatalogKind::Directory?0755:kind==CatalogKind::SymbolicLink?0777:0644;
    return uint16_t((unsigned(kind)<<12)|permissions);
}
std::optional<uint32_t> DeviceNumber(const std::optional<PosixMetadata>& metadata)
{
    return metadata&&IsDevice(metadata->special)?std::optional<uint32_t>(metadata->device):std::nullopt;
}

void SetPosixMetadata(Bytes& inode,const std::optional<PosixMetadata>& metadata)
{
    if(!metadata)return;
    Check(metadata->mode<=PosixMetadata::ModeMask,"Invalid APFS POSIX permissions");
    Set(inode,72,metadata->uid,4);
    Set(inode,76,metadata->gid,4);
    Set(inode,80,(Le16(inode.data()+80)&0170000)|metadata->mode,2);
    const uint32_t hints=((metadata->flags&PosixMetadata::NoDump)?NoDump:0)|
                         ((metadata->flags&PosixMetadata::Opaque)?Opaque:0)|
                         ((metadata->flags&PosixMetadata::Archived)?Archived:0);
    Set(inode,68,(Le32(inode.data()+68)&~BsdHintMask)|hints,4);
}

void SetAdditionalDates(Bytes& inode,const HfsPlus::AdditionalFileDates& dates)
{
    if(dates.accessed)Set(inode,40,CatalogTimestamp(*dates.accessed),8);
    if(dates.changed)Set(inode,32,CatalogTimestamp(*dates.changed),8);
}

void SetFileDates(Bytes& inode,const FileDates& dates)
{
    if(dates.exact){
        for(size_t i=0;i<dates.exact->size();++i)Set(inode,16+i*8,(*dates.exact)[i],8);
        return;
    }
    Set(inode,16,CatalogTimestamp(dates.created),8);
    Set(inode,24,CatalogTimestamp(dates.modified),8);
    SetAdditionalDates(inode,dates.additionalDates);
}

void ApplyTimestampEdits(Bytes& inode,const TimestampEdits& dates)
{
    if(dates.exact){
        for(size_t i=0;i<dates.exact->size();++i)Set(inode,16+i*8,(*dates.exact)[i],8);
        return;
    }
    if(dates.created)Set(inode,16,CatalogTimestamp(*dates.created),8);
    if(dates.modified)Set(inode,24,CatalogTimestamp(*dates.modified),8);
    SetAdditionalDates(inode,dates.additionalDates);
}

struct Mapping {
    uint64_t oid,block,xid=0;
    uint32_t flags=0;
};
struct WritableMap {
    uint64_t headerBlock,rootBlock;
    Bytes header;
    std::vector<Mapping> mappings;
    std::vector<uint64_t> leaves;
};
WritableMap ReadWritableMap(Source& source,const Container& container,uint64_t block,bool historical=false)
{
    auto header=ReadObject(source,container,block,block,container.transaction,11,0,true);
    const auto root=Le64(header.data()+48);
    Check(Le32(header.data()+40)==(Physical|2)&&
          (historical?(Le32(header.data()+36)==1&&Le64(header.data()+56)):
                      (!Le32(header.data()+36)&&!Le64(header.data()+56))),
          "APFS object map contains unsupported snapshot references");
    const Node node(ReadObject(source,container,root,root,container.transaction,2,11,true),true,{});
    Check(node.Level()<=1&&node.Layout().keySize==16&&node.Layout().valueSize==16,
          "APFS writes require a supported object map");
    WritableMap map{block,root,std::move(header),{},{}};
    std::set<uint64_t> physicalBlocks{block,root};
    std::pair<uint64_t,uint64_t> previous{};
    const auto append=[&](const Node& leaf){
        Check(!leaf.Level()&&leaf.Count(),"Invalid APFS object map leaf");
        for(size_t i=0;i<leaf.Count();++i){
            const auto record=leaf.At(i);
            const auto [oid,xid]=ReadObjectMapKey(record.key);
            Check(record.value.size==16,"Invalid APFS writable object-map record size");
            const auto physical=Le64(record.value.data+8);
            const auto key=std::make_pair(oid,xid);
            const auto flags=Le32(record.value.data),size=Le32(record.value.data+4);
            Check(key>previous&&(historical||oid>previous.first)&&xid<=container.transaction&&
                  ((historical&&flags==1&&!size&&!physical)||
                   ((!flags||(historical&&flags==2)||(container.writeCipher&&flags==4))&&size==Block&&physical&&physical<container.blockCount&&
                    physicalBlocks.insert(physical).second)),"Invalid APFS writable object mapping");
            map.mappings.push_back({oid,physical,xid,flags});
            previous=key;
        }
    };
    if(!node.Level())append(node);
    else{
        Check(node.Count()>0,"Empty APFS object map index");
        for(size_t i=0;i<node.Count();++i){
            const auto separator=node.At(i);
            Check(separator.key.size==16&&separator.value.size==8,"Invalid APFS object map separator");
            const auto child=Le64(separator.value.data);
            Check(physicalBlocks.insert(child).second,"Repeated APFS object map leaf");
            const Node leaf(ReadObject(source,container,child,child,container.transaction,3,11,true),false,node.Layout());
            Check(leaf.Count()&&!memcmp(leaf.At(0).key.data,separator.key.data,16),
                  "APFS object map separator does not match its leaf");
            map.leaves.push_back(child);
            append(leaf);
        }
    }
    return map;
}

void ProtectKeybags(const Container& container,std::vector<DataRun>& runs)
{
    if(!container.keybagBlock&&!container.keybagBlocks)return;
    Check(container.keybagBlock&&container.keybagBlocks,"Missing writable APFS keybag");
    runs.push_back({container.keybagBlock,container.keybagBlocks});
    const auto protect=[&](const VolumeCipher& cipher){
        const auto [block,count]=cipher.UnlockBlocks();
        Check(block&&count&&Range(block,count,container.blockCount),"Invalid APFS volume keybag");
        runs.push_back({block,count});
    };
    if(container.writeCipher)protect(*container.writeCipher);
    for(const auto& sibling:container.siblingCiphers)protect(*sibling.second);
    std::sort(runs.begin(),runs.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    for(size_t i=1;i<runs.size();++i)
        Check(runs[i].block>=runs[i-1].block&&runs[i].block-runs[i-1].block>=runs[i-1].count,"Overlapping APFS protected blocks");
}

constexpr size_t MapLeafCapacity=(Block-WriterDisk::NodeHeader-WriterDisk::TreeFooter)/(4+16+16);
size_t MapLeaves(size_t mappings)
{
    return mappings<=MapLeafCapacity?0:size_t(DivideUp(mappings,MapLeafCapacity));
}

void StageMap(Writes& writes,WritableMap map,uint64_t headerBlock,uint64_t rootBlock,
              const std::vector<Mapping>& mappings,uint64_t transaction,
              const std::vector<uint64_t>& allocation,size_t firstLeaf)
{
    Set(map.header,48,rootBlock,8);
    StageObject(writes,headerBlock,std::move(map.header),headerBlock,transaction,Physical|11);
    std::vector<OwnedRecord> records;
    for(const auto& item:mappings){
        OwnedRecord mapping{Bytes(16),Bytes(16)};
        Set(mapping.key,0,item.oid,8);
        Set(mapping.key,8,item.xid?item.xid:transaction,8);
        Set(mapping.value,0,item.flags,4);
        Set(mapping.value,4,Block,4);
        Set(mapping.value,8,item.block,8);
        records.push_back(std::move(mapping));
    }
    const auto count=MapLeaves(records.size());
    if(!count){
        StageObject(writes,rootBlock,EncodeLeaf(records,0x10,16,16),rootBlock,transaction,Physical|2,11);
        return;
    }
    std::vector<OwnedRecord> index;
    for(size_t i=0;i<count;++i){
        const auto begin=i*MapLeafCapacity,end=(std::min)(begin+MapLeafCapacity,records.size());
        const auto child=allocation[firstLeaf+i];
        OwnedRecord separator{records[begin].key,Bytes(8)};
        Set(separator.value,0,child,8);
        index.push_back(std::move(separator));
        const std::vector<OwnedRecord> leaf(records.begin()+begin,records.begin()+end);
        StageObject(writes,child,WriterDisk::EncodeNode(leaf,0x10,false,0,16,16),child,transaction,Physical|3,11);
    }
    auto node=WriterDisk::EncodeNode(index,0x10,true,1,16,8);
    constexpr auto footer=Block-WriterDisk::TreeFooter;
    Set(node,footer+12,16,4);
    Set(node,footer+20,16,4);
    Set(node,footer+24,records.size(),8);
    Set(node,footer+32,count+1,8);
    StageObject(writes,rootBlock,std::move(node),rootBlock,transaction,Physical|2,11);
}

struct CatalogRecords {
    std::vector<OwnedRecord> records;
    std::vector<uint64_t> blocks;
    bool verifiedPhysicalReferences=false;
    std::shared_ptr<const VolumeCipher> cipher;
};
CatalogRecords ReadWritableCatalog(Source& source,const Container& container,const WritableMap& map,uint64_t oid,bool historical=false)
{
    const auto find=[&](uint64_t wanted){
        const auto item=std::lower_bound(map.mappings.begin(),map.mappings.end(),wanted,
            [](const auto& entry,uint64_t id){return entry.oid<id;});
        Check(item!=map.mappings.end()&&item->oid==wanted,"Missing APFS catalog object mapping");
        return *item;
    };
    CatalogRecords catalog;
    const auto append=[&](const Node& leaf){
        Check(!leaf.Level()&&leaf.Count(),"Invalid APFS writable catalog leaf");
        for(size_t i=0;i<leaf.Count();++i){
            const auto record=leaf.At(i);
            catalog.records.push_back({Bytes(record.key.data,record.key.data+record.key.size),
                                       Bytes(record.value.data,record.value.data+record.value.size)});
        }
    };
    const auto rootMapping=find(oid);
    const auto rootBlock=rootMapping.block;
    const auto cipherFor=[&](const Mapping& mapping){
        return mapping.flags&4?container.writeCipher.get():nullptr;
    };
    const Node root(ReadObject(source,container,rootBlock,oid,container.transaction,2,14,false,cipherFor(rootMapping)),true,{});
    Check(root.Level()<=2&&!root.Layout().keySize&&!root.Layout().valueSize,
          "APFS catalog writes currently support at most three tree levels");
    catalog.blocks.push_back(rootBlock);
    std::set<uint64_t> seen{oid};
    const auto visit=[&](const auto& visit,const Node& node)->void{
        if(!node.Level()){
            append(node);
            return;
        }
        Check(node.Count()>0,"Empty APFS catalog index");
        for(size_t i=0;i<node.Count();++i){
            const auto record=node.At(i);
            Check(record.value.size==8,"Invalid APFS catalog index record");
            const auto child=Le64(record.value.data);
            Check(seen.insert(child).second,"Repeated APFS catalog child");
            const auto mapping=find(child);
            const auto block=mapping.block;
            const Node next(ReadObject(source,container,block,child,container.transaction,3,14,false,cipherFor(mapping)),false,{});
            Check(next.Level()+1==node.Level()&&next.Count(),"Invalid APFS catalog child level or count");
            const auto first=next.At(0).key;
            Check(first.size==record.key.size&&!memcmp(first.data,record.key.data,first.size),
                  "APFS catalog separator does not match its first child key");
            catalog.blocks.push_back(block);
            visit(visit,next);
        }
    };
    visit(visit,root);
    Check(historical||catalog.blocks.size()==map.mappings.size(),"APFS volume map has unrelated live objects");
    return catalog;
}

OwnedRecord StreamReference(uint64_t inode)
{
    OwnedRecord record{Bytes(8),Bytes(4)};
    Set(record.key,0,(uint64_t(6)<<60)|inode,8);
    Set(record.value,0,1,4);
    return record;
}

OwnedRecord FileExtent(const FileRun& run,uint64_t logicalBlock,bool encrypted=false)
{
    OwnedRecord record{Bytes(16),Bytes(24)};
    Set(record.key,0,(uint64_t(8)<<60)|(run.stream?run.stream:run.owner),8);
    Set(record.key,8,logicalBlock*Block,8);
    Set(record.value,0,run.count*Block,8);
    Set(record.value,8,run.block,8);
    if(encrypted){
        Set(record.value,0,(uint64_t(1)<<56)|(run.count*Block),8);
        Set(record.value,16,run.block,8);
    }
    return record;
}

OwnedRecord ExtentReference(const FileRun& run)
{
    OwnedRecord record{Bytes(8),Bytes(20)};
    Set(record.key,0,(uint64_t(2)<<60)|run.block,8);
    Set(record.value,0,(uint64_t(1)<<60)|run.count,8);
    Set(record.value,8,run.owner,8);
    Set(record.value,16,1,4);
    return record;
}

bool Matches(View view,const Bytes& expected)
{
    return view.size==expected.size()&&!memcmp(view.data,expected.data(),view.size);
}

CatalogRecords ReadExtentTree(Source& source,const Container& container,uint64_t block,bool updates=false)
{
    CatalogRecords tree;
    tree.blocks.push_back(block);
    const Node root(ReadObject(source,container,block,block,container.transaction,2,15,true),true,{});
    Check(root.Level()<=1&&!root.Layout().keySize&&!root.Layout().valueSize,
          "Unsupported APFS extent reference tree layout");
    uint64_t previousEnd=0;
    const auto append=[&](const Node& leaf){
        Check(!leaf.Level(),"Invalid APFS extent reference leaf level");
        for(size_t i=0;i<leaf.Count();++i){
            const auto record=leaf.At(i);
            Check(record.key.size==8&&record.value.size==20,"Invalid APFS extent reference record size");
            const auto key=Le64(record.key.data),length=Le64(record.value.data);
            const auto start=key&IdMask,count=length&IdMask;
            const auto kind=length>>60;
            const bool update=updates&&kind==2&&Le64(record.value.data+8)==UINT64_MAX&&
                               Le32(record.value.data+16)!=0;
            Check(key>>60==2&&(kind==1||update)&&start&&count&&start>=previousEnd&&
                  Range(start,count,container.blockCount)&&Le32(record.value.data+16)>0,
                  "Invalid or overlapping APFS physical extent range");
            previousEnd=start+count;
            tree.records.push_back({Bytes(record.key.data,record.key.data+8),Bytes(record.value.data,record.value.data+20)});
        }
    };
    if(!root.Level())append(root);
    else{
        Check(root.Count()>0,"Empty APFS extent reference index");
        std::set<uint64_t> seen{block};
        for(size_t i=0;i<root.Count();++i){
            const auto separator=root.At(i);
            Check(separator.key.size==8&&separator.value.size==8,"Invalid APFS extent reference index key");
            const auto child=Le64(separator.value.data);
            Check(seen.insert(child).second,"Repeated APFS extent reference leaf");
            const Node leaf(ReadObject(source,container,child,child,container.transaction,3,15,true),false,{});
            Check(leaf.Count()>0,"Empty APFS extent reference leaf");
            const auto first=leaf.At(0).key;
            Check(first.size==8&&!memcmp(first.data,separator.key.data,8),"Invalid APFS extent reference separator");
            append(leaf);
            tree.blocks.push_back(child);
        }
    }
    return tree;
}

void ValidateExtentReferences(const CatalogRecords& tree,std::vector<FileRun>& files)
{
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    Check(tree.records.size()==files.size(),"APFS extent reference count disagrees with the catalog");
    uint64_t end=0;
    for(size_t i=0;i<files.size();++i){
        const auto& run=files[i];
        const auto& record=tree.records[i];
        const auto expected=ExtentReference(run);
        Check(run.block>=end&&record.key==expected.key&&record.value==expected.value,
              "Invalid, shared or overlapping APFS extent reference");
        end=run.block+run.count;
    }
}

constexpr auto ResourceAttribute="com.apple.ResourceFork";
constexpr auto CompressionAttribute="com.apple.decmpfs";
constexpr auto SymlinkAttribute="com.apple.fs.symlink";
constexpr uint64_t HasUncompressedSize=0x40000;
constexpr uint32_t BsdCompressed=0x20;
void SetCompressionFlags(Bytes& inode,uint64_t size,bool compressed)
{
    const auto flags=Le64(inode.data()+48)&~HasUncompressedSize;
    Set(inode,48,flags|(compressed?HasUncompressedSize:0),8);
    const auto bsd=Le32(inode.data()+68)&~BsdCompressed;
    Set(inode,68,bsd|(compressed?BsdCompressed:0),4);
    Set(inode,84,compressed?size:0,8);
}
constexpr uint64_t HasResourceFork=0x4000,NoResourceFork=0x8000;
void SetResourceFlag(Bytes& inode,bool present)
{
    const auto flags=Le64(inode.data()+48)&~(HasResourceFork|NoResourceFork);
    Set(inode,48,flags|(present?HasResourceFork:NoResourceFork),8);
}
constexpr uint64_t HasSecurityAttribute=0x40;
constexpr auto SecurityAttribute=u"com.apple.system.Security";
void SetSecurityFlag(Bytes& inode,const HfsPlus::Attributes& attributes)
{
    const auto flags=Le64(inode.data()+48)&~HasSecurityAttribute;
    Set(inode,48,flags|(attributes.count(SecurityAttribute)?HasSecurityAttribute:0),8);
}
bool OrdinaryAttribute(std::string_view name)
{
    return name!="com.apple.FinderInfo"&&name!="com.apple.ResourceFork"&&name!="com.apple.decmpfs"&&
           name!="com.apple.fs.symlink";
}

OwnedRecord EmbeddedAttribute(uint64_t inode,std::string_view name,size_t size)
{
    OwnedRecord record{Bytes(11+name.size()),Bytes(4+size)};
    Set(record.key,0,(uint64_t(4)<<60)|inode,8);
    Set(record.key,8,name.size()+1,2);
    std::copy(name.begin(),name.end(),record.key.begin()+10);
    Set(record.value,0,2,2);
    Set(record.value,2,size,2);
    return record;
}

void SortAttributes(std::vector<OwnedRecord>& records)
{
    std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){
        return std::lexicographical_compare(a.key.begin()+10,a.key.end(),b.key.begin()+10,b.key.end());
    });
}
using AttributeStreams=std::map<std::u16string,uint64_t>;
class AttributeInput final:public Source {
    const HfsPlus::AttributeData value;
public:
    explicit AttributeInput(HfsPlus::AttributeData data):value(std::move(data)){}
    uint64_t Size()const override{return value.size();}
    bool Writable()const override{return false;}
    void Read(uint64_t offset,void* data,size_t size)override{value.Read(offset,data,size);}
    void Write(uint64_t,const void*,size_t)override{throw HfsPlus::Error("APFS attribute source is read-only");}
    void Flush()override{}
};
OwnedRecord StreamAttribute(uint64_t inode,std::string_view name,uint64_t id,uint64_t size)
{
    auto record=EmbeddedAttribute(inode,name,48);
    Set(record.value,0,1,2);
    Set(record.value,4,id,8);
    Set(record.value,12,size,8);
    Set(record.value,20,DivideUp(size,Block)*Block,8);
    return record;
}
struct ResourceWrite {
    Source* input;
    uint64_t size,id;
    uint64_t allocatedBytes=0;
};
std::vector<OwnedRecord> EncodeAttributes(uint64_t inode,const HfsPlus::Attributes* attributes,
                                          const ResourceWrite* resource,const AttributeStreams& streams)
{
    std::vector<OwnedRecord> records;
    if(attributes){
        ValidateWritableAttributes(*attributes);
        for(const auto& [wide,value]:*attributes){
            const auto name=EncodeAttributeName(wide);
            const auto stream=streams.find(wide);
            if(stream!=streams.end())records.push_back(StreamAttribute(inode,name,stream->second,value.size()));
            else{
                Check(value.size()<EmbeddedAttributeLimit,"Missing APFS attribute stream allocation");
                auto record=EmbeddedAttribute(inode,name,size_t(value.size()));
                if(value.size())value.Read(0,record.value.data()+4,size_t(value.size()));
                records.push_back(std::move(record));
            }
        }
    }
    if(resource&&resource->size){
        if(resource->id){
            auto record=StreamAttribute(inode,ResourceAttribute,resource->id,resource->size);
            Set(record.value,20,resource->allocatedBytes,8);
            records.push_back(std::move(record));
        }
        else{
            auto record=EmbeddedAttribute(inode,ResourceAttribute,size_t(resource->size));
            resource->input->Read(0,record.value.data()+4,size_t(resource->size));
            records.push_back(std::move(record));
        }
    }
    return records;
}

OwnedRecord LinkTargetRecord(uint64_t inode,const Bytes& path)
{
    Check(path.size()>=2&&path.size()<EmbeddedAttributeLimit&&path.back()==0&&
          !memchr(path.data(),0,path.size()-1),"Invalid APFS symbolic link target");
    auto record=EmbeddedAttribute(inode,SymlinkAttribute,path.size());
    Set(record.value,0,6,2);
    std::copy(path.begin(),path.end(),record.value.begin()+4);
    return record;
}

void WriteCompressionAttribute(std::vector<OwnedRecord>& attributes,Bytes& inode,
                               uint64_t id,uint64_t dataSize,const Bytes* compression,const AttributeStreams& streams)
{
    uint64_t logicalSize=0;
    if(compression){
        Check(compression->size()>=16&&compression->size()<EmbeddedAttributeLimit&&
              Le32(compression->data())==0x636d7066&&!dataSize,"Invalid APFS compressed file output");
        const auto stream=streams.find(u"com.apple.decmpfs");
        if(stream!=streams.end())
            attributes.push_back(StreamAttribute(id,CompressionAttribute,stream->second,compression->size()));
        else{
            Check(compression->size()<=WriterDisk::InlineCompressionCapacity(),
                  "Missing APFS compression attribute stream");
            auto record=EmbeddedAttribute(id,CompressionAttribute,compression->size());
            std::copy(compression->begin(),compression->end(),record.value.begin()+4);
            attributes.push_back(std::move(record));
        }
        logicalSize=Le64(compression->data()+8);
    }
    SetCompressionFlags(inode,logicalSize,compression!=nullptr);
}

void SetSibling(Bytes& value,uint64_t sibling)
{
    value.resize(34);
    Set(value,18,1,2);
    Set(value,20,8,2);
    value[22]=1;
    Set(value,24,8,2);
    Set(value,26,sibling,8);
}

struct CatalogSibling {
    uint64_t inode,parent;
    std::string name;
    std::optional<size_t> linkRecord,mapRecord;
    size_t directoryRecord=0;

    OwnedRecord LinkRecord(uint64_t id)const
    {
        OwnedRecord record{Bytes(16),Bytes(11+name.size())};
        Set(record.key,0,(uint64_t(5)<<60)|inode,8);
        Set(record.key,8,id,8);
        Set(record.value,0,parent,8);
        Set(record.value,8,name.size()+1,2);
        std::copy(name.begin(),name.end(),record.value.begin()+10);
        return record;
    }
    OwnedRecord MapRecord(uint64_t id)const
    {
        OwnedRecord record{Bytes(8),Bytes(8)};
        Set(record.key,0,(uint64_t(12)<<60)|id,8);
        Set(record.value,0,inode,8);
        return record;
    }
};

std::vector<OwnedRecord> EditCatalog(std::vector<OwnedRecord> records,const CatalogMutation& request,const std::string& name,
    uint64_t nextId,uint64_t entryId,uint64_t time,bool fold,uint64_t directoryCount,uint64_t fileCount,uint64_t symlinkCount,uint64_t specialCount,
    uint64_t dataSize,uint64_t blockCount,std::vector<FileRun>& fileRuns,std::optional<CatalogKind>& removedKind,
    size_t& streamAt,const std::optional<FileDates>& dates,const ResourceWrite* resourceWrite,const AttributeStreams& streams,std::set<uint64_t>& retiredStreams)
{
    Check(directoryCount<=records.size()&&fileCount<=records.size(),"Invalid APFS catalog entry counts");
    const auto at=[&](size_t i){
        const auto& record=records[i];
        return Record{{record.key.data(),record.key.size()},{record.value.data(),record.value.size()}};
    };
    const auto parent=std::visit([](const auto& edit)->uint64_t{
        if constexpr((std::is_same_v<std::decay_t<decltype(edit)>,CatalogSnapshot>||std::is_same_v<std::decay_t<decltype(edit)>,CatalogVolumeRename>))return 0;
        else return edit.parent;
    },request);
    const auto* create=std::get_if<CatalogCreate>(&request);
    const auto* rename=std::get_if<CatalogRename>(&request);
    const auto* remove=std::get_if<CatalogDelete>(&request);
    const auto* replace=std::get_if<CatalogReplaceData>(&request);
    const auto* metadata=std::get_if<CatalogMetadata>(&request);
    const auto* attributesEdit=std::get_if<CatalogAttributes>(&request);
    const auto* resourceEdit=std::get_if<CatalogResourceFork>(&request);
    const auto* linkEdit=std::get_if<CatalogReplaceSymlink>(&request);
    const auto copyDates=[&](Bytes& inode){
        if(!dates)return;
        SetFileDates(inode,*dates);
    };
    const auto editDates=[&](Bytes& inode){
        ApplyTimestampEdits(inode,metadata->dates);
    };
    const bool privateFirst=WriterDisk::DirectoryHash("private-dir")<WriterDisk::DirectoryHash("root");
    const uint64_t reserved[]={privateFirst?3u:2u,privateFirst?2u:3u};
    // APFS orders equal-hash directory keys by their stored names.
    using Key=std::tuple<uint64_t,uint64_t,uint32_t,std::string>;
    const auto requestedName=NormalizedName(name,fold);
    std::map<uint64_t,std::set<std::u32string>> directoryNames;
    OwnedRecord added;
    std::optional<Key> addedKey;
    const auto* hardLink=std::get_if<CatalogHardLink>(&request);
    if(create||rename||hardLink){
        added=WriterDisk::DirectoryEntry(name,entryId,time,parent,fold,
                                         uint16_t(create?create->kind:hardLink?CatalogKind::RegularFile:CatalogKind::Directory));
        addedKey=Key{parent,9,Le32(added.key.data()+8)>>10,name};
    }
    Key previous{};
    size_t insertAt=2;
    struct Directory {
        size_t record;
        uint32_t children=0;
    };
    std::map<uint64_t,Directory> directories;
    const auto readFinder=[](const uint8_t* value,size_t size)->std::optional<FinderBytes>{
        if(!(Le64(value+48)&WriterDisk::HasFinderInfo))return {};
        FinderBytes finder{};
        const auto kind=SpecialType(Le16(value+80)>>12);
        const size_t deviceBytes=IsDevice(kind)?8:0;
        Check(size>=100+WriterDisk::FinderFieldSize+deviceBytes,"Truncated APFS Finder information");
        std::copy_n(value+size-finder.size()-deviceBytes,finder.size(),finder.begin());
        return finder;
    };
    struct Link {
        uint64_t parent;
        CatalogKind kind;
        std::string name;
        size_t entryRecord;
        size_t inodeRecord=0,attributes=0,streamRecords=0;
        uint64_t dataSize=0;
        std::optional<FinderBytes> finder;
        std::optional<size_t> resourceRecord,compressionRecord;
    };
    std::map<uint64_t,Link> links;
    std::optional<size_t> selectedRecord,replacedRecord;
    uint64_t selectedSibling=0;
    std::map<uint64_t,CatalogSibling> siblings;
    std::map<uint64_t,std::set<uint64_t>> siblingGroups;
    for(size_t i=2;i<records.size();++i){
        const auto item=at(i);
        if(item.key.size<14||Le64(item.key.data)>>60!=9||item.value.size!=34)continue;
        const auto sibling=Le64(item.value.data+26);
        const auto inode=Le64(item.value.data);
        Bytes expected(item.value.data,item.value.data+18);
        SetSibling(expected,sibling);
        Check(sibling>inode&&sibling<nextId&&Matches(item.value,expected),
              "Invalid APFS sibling directory record");
        const auto name=reinterpret_cast<const char*>(item.key.data+12);
        Check(siblings.emplace(sibling,CatalogSibling{inode,Le64(item.key.data)&IdMask,
              std::string(name,item.key.size-13),{},{},i}).second,"Repeated APFS sibling ID");
        siblingGroups[inode].insert(sibling);
    }
    // A promoted regular-file name can be in a folder with a later object ID.
    for(size_t i=2;i<records.size();++i){
        const auto item=at(i);
        if(item.key.size<14||Le64(item.key.data)>>60!=9||
           (item.value.size!=18&&item.value.size!=34)||
           Le16(item.value.data+16)!=uint16_t(CatalogKind::RegularFile))continue;
        const auto inode=Le64(item.value.data);
        if(item.value.size==34&&Le64(item.value.data+26)!=*siblingGroups.at(inode).begin())continue;
        Check(links.emplace(inode,Link{Le64(item.key.data)&IdMask,CatalogKind::RegularFile,
              std::string(reinterpret_cast<const char*>(item.key.data+12),item.key.size-13),i}).second,
              "Repeated APFS primary file name");
    }
    struct AttributeStream {uint64_t inode,blocks;};
    std::map<uint64_t,AttributeStream> attributeStreams;
    uint64_t files=0,symlinks=0,special=0;
    for(size_t i=0;i<records.size();++i){
        const auto view=at(i);
        size_t streamRecords=0;
        bool renamingThisEntry=false;
        if(i<2){
            const auto expected=WriterDisk::DirectoryEntry(reserved[i]==2?"root":"private-dir",reserved[i],0);
            Check(view.key.size==expected.key.size()&&!memcmp(view.key.data,expected.key.data(),view.key.size)&&
                  view.value.size==18&&Le64(view.value.data)==reserved[i]&&Le16(view.value.data+16)==4,
                  "Invalid APFS reserved directory entry");
        }else{
            Check(view.key.size>=8,"Invalid APFS directory key");
            const auto key=Le64(view.key.data);
            const auto inode=key&IdMask,type=key>>60;
            Check(type==12||!siblings.count(inode),"APFS sibling ID is also used by another catalog object");
            uint32_t hash=0;
            std::string storedName;
            if(type==9){
                const auto folder=directories.find(inode);
                Check(inode!=3&&folder!=directories.end()&&view.key.size>=14&&
                      (view.value.size==18||view.value.size==34),"Invalid APFS directory entry");
                const auto kind=CatalogKind(Le16(view.value.data+16));
                Check(kind==CatalogKind::Directory||kind==CatalogKind::RegularFile||kind==CatalogKind::SymbolicLink||IsSpecialKind(kind),
                      "APFS catalog contains an unsupported entry type");
                const auto nameKey=Le32(view.key.data+8);
                Check((nameKey&1023u)==view.key.size-12&&view.key.data[view.key.size-1]==0,
                      "Invalid APFS entry name");
                auto childName=std::string(reinterpret_cast<const char*>(view.key.data+12),view.key.size-13);
                Check(childName.size()<=255&&childName.find_first_of("/:")==childName.npos&&
                      childName.find('\0')==childName.npos&&childName!="."&&childName!="..",
                      "Invalid APFS directory name");
                hash=nameKey>>10;
                Check((WriterDisk::DirectoryHash(childName,fold)>>10)==hash,"Invalid APFS directory name hash");
                const auto child=Le64(view.value.data);
                const auto group=siblingGroups.find(child);
                const auto sibling=view.value.size==34?Le64(view.value.data+26):0;
                const auto primary=group==siblingGroups.end()?0:*group->second.begin();
                const auto aliasId=rename?rename->alias:remove?remove->alias:0;
                const bool selected=(rename||remove)&&entryId==child&&parent==inode&&
                                    sibling==(aliasId?aliasId:primary);
                renamingThisEntry=rename&&selected;
                if(selected){
                    selectedRecord=i;
                    selectedSibling=sibling;
                }
                const auto canonicalName=NormalizedName(childName,fold);
                Check(directoryNames[inode].insert(canonicalName).second,"Duplicate APFS directory name");
                if(hardLink&&hardLink->replaced==child&&parent==inode&&canonicalName==requestedName)
                    replacedRecord=i;
                Check(!addedKey||parent!=inode||renamingThisEntry||replacedRecord==i||canonicalName!=requestedName,
                      "The APFS entry name already exists");
                storedName=childName;
                const bool alias=view.value.size==34;
                Check(child>=16&&child<nextId&&alias==(group!=siblingGroups.end())&&
                      (!alias||(kind==CatalogKind::RegularFile&&group->second.size()>=2)),
                      "Invalid APFS directory entry ID or sibling group");
                if(kind!=CatalogKind::RegularFile){
                    Check(child>inode&&links.emplace(child,Link{inode,kind,std::move(childName),i}).second,
                          "Invalid or duplicate APFS primary directory entry");
                }
                ++folder->second.children;
            }else if(type==12){
                const auto sibling=siblings.find(inode);
                Check(sibling!=siblings.end()&&!sibling->second.mapRecord,"Unexpected APFS sibling map");
                const auto expected=sibling->second.MapRecord(inode);
                Check(Matches(view.key,expected.key)&&Matches(view.value,expected.value),"Invalid APFS sibling map");
                sibling->second.mapRecord=i;
            }else if(type==8){
                const auto stream=attributeStreams.find(inode);
                Check(stream!=attributeStreams.end(),"APFS extent has no attribute stream");
                const auto size=stream->second.blocks;
                for(uint64_t logical=0;logical<size;++streamRecords){
                    const auto index=i+streamRecords;
                    Check(index<records.size(),"Missing APFS attribute extent");
                    const auto extent=at(index);
                    Check(extent.value.size==24,"Invalid APFS attribute extent size");
                    const auto length=Le64(extent.value.data);
                    const FileRun run{stream->second.inode,Le64(extent.value.data+8),length/Block,inode};
                    Check(run.block&&run.count&&!(length%Block)&&run.count<=size-logical&&
                          Range(run.block,run.count,blockCount),"Invalid APFS attribute extent");
                    const auto encoded=FileExtent(run,logical);
                    Check(Matches(extent.key,encoded.key)&&Matches(extent.value,encoded.value),
                          "Invalid APFS attribute extent order");
                    fileRuns.push_back(run);
                    logical+=run.count;
                }
                --streamRecords;
                attributeStreams.erase(stream);
            }else{
                Check(type==3&&view.key.size==8&&view.value.size>=92&&Le64(view.value.data+8)==inode,
                      "Invalid APFS inode");
                const auto parentId=Le64(view.value.data);
                const bool reservedInode=inode==2||inode==3;
                const auto link=links.find(inode);
                Check(reservedInode?parentId==1:link!=links.end()&&link->second.parent==parentId,
                      "Invalid APFS inode parent");
                const auto kind=reservedInode?CatalogKind::Directory:link->second.kind;
                if(!reservedInode){
                    link->second.inodeRecord=i;
                    link->second.finder=readFinder(view.value.data,view.value.size);
                }
                bool hasSecurity=false,hasTarget=false;
                if(!reservedInode){
                    std::string previousAttribute;
                    for(;i+1+streamRecords<records.size();++streamRecords){
                        const auto attr=at(i+1+streamRecords);
                        if(attr.key.size<8||Le64(attr.key.data)!=((uint64_t(4)<<60)|inode))break;
                        Check(attr.key.size>=12&&attr.key.size<=138&&attr.key.data[attr.key.size-1]==0&&
                              Le16(attr.key.data+8)==attr.key.size-10&&attr.value.size>=4&&
                              Le16(attr.value.data+2)==attr.value.size-4&&
                              attr.value.size-4<EmbeddedAttributeLimit,"Invalid APFS embedded attribute");
                        const std::string name(reinterpret_cast<const char*>(attr.key.data+10),attr.key.size-11);
                        Check(name>previousAttribute&&name.find('\0')==name.npos&&(OrdinaryAttribute(name)||name==ResourceAttribute||name==CompressionAttribute||name==SymlinkAttribute),
                              "Invalid APFS attribute name or order");
                        hasSecurity|=name=="com.apple.system.Security";
                        if(name==ResourceAttribute){
                            Check(kind==CatalogKind::RegularFile&&attr.value.size>4,"Invalid APFS resource fork");
                            link->second.resourceRecord=i+1+streamRecords;
                        }
                        const auto storage=Le16(attr.value.data);
                        if(name==CompressionAttribute){
                            Check(kind==CatalogKind::RegularFile&&storage==2&&attr.value.size>=20&&
                                  Le32(attr.value.data+4)==0x636d7066,"Invalid APFS compression header");
                            link->second.compressionRecord=i+1+streamRecords;
                        }
                        if(name==SymlinkAttribute){
                            Check(kind==CatalogKind::SymbolicLink&&storage==6&&attr.value.size>=6&&
                                  attr.value.data[attr.value.size-1]==0&&
                                  !memchr(attr.value.data+4,0,attr.value.size-5),"Invalid APFS symbolic link target");
                            hasTarget=true;
                        }else if(storage==1){
                            Check(attr.value.size==52,"Invalid APFS attribute stream descriptor");
                            const auto id=Le64(attr.value.data+4),size=Le64(attr.value.data+12);
                            const auto blocks=DivideUp(size,Block);
                            Check(id>inode&&id<nextId&&size&&blocks<=blockCount&&
                                  Le64(attr.value.data+20)==blocks*Block&&
                                  !Le64(attr.value.data+28)&&!Le64(attr.value.data+36)&&!Le64(attr.value.data+44)&&
                                  attributeStreams.emplace(id,AttributeStream{inode,blocks}).second,
                                  "Invalid APFS attribute stream");
                        }else Check(storage==2,"Invalid APFS embedded attribute flags");
                        previousAttribute=name;
                    }
                    link->second.attributes=streamRecords;
                    for(;i+1+streamRecords<records.size();++streamRecords){
                        const auto item=at(i+1+streamRecords);
                        if(item.key.size<8||Le64(item.key.data)!=((uint64_t(5)<<60)|inode))break;
                        Check(item.key.size==16,"Invalid APFS sibling link key");
                        const auto id=Le64(item.key.data+8);
                        const auto sibling=siblings.find(id);
                        Check(sibling!=siblings.end()&&!sibling->second.linkRecord,"Unexpected APFS sibling link");
                        const auto expected=sibling->second.LinkRecord(id);
                        Check(Matches(item.key,expected.key)&&Matches(item.value,expected.value),"Invalid APFS sibling link");
                        sibling->second.linkRecord=i+1+streamRecords;
                    }
                }
                Check(bool(Le64(view.value.data+48)&HasSecurityAttribute)==hasSecurity,
                      "APFS security attribute and inode flag disagree");
                Check(hasTarget==(kind==CatalogKind::SymbolicLink),"APFS symbolic link target is missing");
                const auto mode=Le16(view.value.data+80)&0170000;
                if(kind==CatalogKind::Directory){
                    Check(mode==0040000&&directories.emplace(inode,Directory{i}).second,"Invalid APFS directory inode");
                }else{
                    const auto& primaryName=link->second.name;
                    const auto value=view.value.data;
                    const auto& finder=link->second.finder;
                    const size_t stream=104+(finder?4:0)+size_t(DivideUp(primaryName.size()+1,8)*8);
                    const size_t streamEnd=stream+40+(finder?finder->size():0);
                    const auto size=view.value.size==streamEnd?Le64(value+stream):0;
                    Check(DivideUp(size,Block)<=blockCount,"Unsupported APFS file size in writable catalog");
                    const bool symlink=kind==CatalogKind::SymbolicLink;
                    Check(kind==CatalogKind::RegularFile||(!size&&!link->second.resourceRecord&&!link->second.compressionRecord),
                          "APFS special entry cannot have file streams");
                    const auto group=siblingGroups.find(inode);
                    const auto count=group==siblingGroups.end()?1:group->second.size();
                    Check(count<=INT32_MAX,"Too many APFS hard links");
                    std::optional<uint32_t> device;
                    if(IsDevice(SpecialType(unsigned(kind))))device=Le32(value+view.value.size-8);
                    auto expected=WriterDisk::Inode(primaryName,inode,0,parentId,EntryMode(kind),
                                                    uint32_t(count),size,finder?&*finder:nullptr,device);
                    SetResourceFlag(expected.value,link->second.resourceRecord.has_value());
                    if(hasSecurity)Set(expected.value,48,Le64(expected.value.data()+48)|HasSecurityAttribute,8);
                    SetPosixMetadata(expected.value,PosixMetadata{
                        Le32(value+72),Le32(value+76),uint16_t(Le16(value+80)&PosixMetadata::ModeMask)});
                    Set(expected.value,68,Le32(value+68)&(WriteProtectionMask|BsdHintMask),4);
                    const auto compression=link->second.compressionRecord;
                    if(compression){
                        Check(!size,"Compressed APFS files cannot have a data stream");
                        SetCompressionFlags(expected.value,Le64(records[*compression].value.data()+12),true);
                    }
                    Check(mode==(uint16_t(kind)<<12)&&view.value.size==expected.value.size()&&
                          !memcmp(value+48,expected.value.data()+48,view.value.size-48),
                          "APFS writes require native regular-file data streams");
                    if(size){
                        Check(i+2+streamRecords<records.size(),"Missing APFS data-stream records");
                        const auto reference=at(i+1+streamRecords);
                        const auto streamRef=StreamReference(inode);
                        Check(Matches(reference.key,streamRef.key)&&Matches(reference.value,streamRef.value),
                              "Invalid APFS data-stream record");
                        ++streamRecords;
                        const auto blocks=DivideUp(size,Block);
                        for(uint64_t logical=0;logical<blocks;){
                            const auto index=i+1+streamRecords;
                            Check(index<records.size(),"Missing APFS file extent");
                            const auto extent=at(index);
                            Check(extent.value.size==24,"Invalid APFS file extent size");
                            const auto length=Le64(extent.value.data);
                            const FileRun run{inode,Le64(extent.value.data+8),length/Block};
                            Check(run.block&&run.count&&!(length%Block)&&run.count<=blocks-logical&&
                                  Range(run.block,run.count,blockCount),"Invalid length or address in APFS file extent");
                            const auto encoded=FileExtent(run,logical);
                            Check(Matches(extent.key,encoded.key)&&Matches(extent.value,encoded.value),
                                  "Invalid or noncontiguous logical APFS file extent");
                            logical+=run.count;
                            fileRuns.push_back(run);
                            ++streamRecords;
                        }
                    }
                    link->second.dataSize=size;
                    link->second.streamRecords=streamRecords-link->second.attributes;
                    if(symlink)++symlinks;else if(IsSpecialKind(kind))++special;else ++files;
                }
            }
            // Catalog keys sort by inode ID, record type, then directory name hash.
            const Key order{inode,type,hash,std::move(storedName)};
            Check(order>previous,"Invalid or unsupported APFS directory key order");
            // A regular file cannot be a destination parent, so its stream
            // records always sort on the same side of the insertion point.
            if(addedKey&&!renamingThisEntry&&replacedRecord!=i&&order<*addedKey)insertAt+=1+streamRecords;
            previous=order;
        }
        i+=streamRecords;
    }
    Check(attributeStreams.empty()&&directories.count(2)&&directories.count(3)&&directories.size()==directoryCount+2&&
          files==fileCount&&symlinks==symlinkCount&&special==specialCount&&
          links.size()==directoryCount+fileCount+symlinkCount+specialCount,"APFS catalog records and volume counts disagree");
    for(const auto& entry:directories){
        const auto& folder=entry.second;
        Check(Le32(records[folder.record].value.data()+56)==folder.children,"Invalid APFS directory child count");
    }
    for(const auto& [id,sibling]:siblings)
        Check(sibling.linkRecord&&sibling.mapRecord,"Missing APFS sibling link or map");
    const auto target=directories.find(parent);
    Check(parent!=3&&target!=directories.end(),"The APFS destination folder does not exist");
    auto& parentInode=records[target->second.record].value;
    Check(!(Le32(parentInode.data()+68)&WriteProtectionMask),
          "Unlock the APFS destination folder before making changes");
    if(!replace&&!metadata&&!attributesEdit&&!resourceEdit&&!linkEdit){
        Set(parentInode,24,time,8);
        Set(parentInode,32,time,8);
    }
    if(hardLink){
        const auto found=links.find(entryId);
        Check(found!=links.end()&&found->second.kind==CatalogKind::RegularFile,
              "APFS hard-link source must be a regular file");
        const auto& link=found->second;
        auto& inode=records[link.inodeRecord].value;
        const auto flags=Le32(inode.data()+68);
        Check(!(flags&WriteProtectionMask)||hardLink->copiedLockedInode,
              "Unlock the APFS file before creating a hard link");
        const auto count=Le32(inode.data()+56);
        Check(count<INT32_MAX,"Too many APFS hard links");
        Set(inode,56,count+1,4);
        if(!hardLink->copiedLockedInode)Set(inode,32,time,8);
        if(!hardLink->replaced)Set(parentInode,56,target->second.children+1,4);
        auto afterLinks=link.inodeRecord+1+link.attributes+(count==1?0:count);
        std::vector<OwnedRecord> newLinks,newMaps;
        const auto append=[&](uint64_t id,uint64_t folder,const std::string& filename){
            const CatalogSibling sibling{entryId,folder,filename};
            newLinks.push_back(sibling.LinkRecord(id));
            newMaps.push_back(sibling.MapRecord(id));
        };
        if(count==1){
            SetSibling(records[link.entryRecord].value,nextId);
            append(nextId,link.parent,link.name);
        }
        const auto aliasId=nextId+1;
        SetSibling(added.value,aliasId);
        append(aliasId,parent,name);
        if(hardLink->replaced){
            const auto victim=links.find(hardLink->replaced);
            Check(replacedRecord&&victim!=links.end()&&victim->second.entryRecord==*replacedRecord&&
                  victim->second.kind==CatalogKind::RegularFile&&victim->first!=entryId&&
                  !siblingGroups.count(victim->first),"APFS can only replace an unlinked regular file with a hard link");
            const auto& old=victim->second;
            Check(!Le32(records[old.inodeRecord].value.data()+68),"Unlock the APFS destination before replacing it");
            const auto first=old.inodeRecord+1;
            for(size_t index=first;index<first+old.attributes;++index){
                const auto& value=records[index].value;
                if(Le16(value.data())==1)retiredStreams.insert(Le64(value.data()+4));
            }
            const auto length=1+old.attributes+old.streamRecords;
            const auto oldAfterLinks=afterLinks;
            if(*replacedRecord<oldAfterLinks)--afterLinks;
            if(old.inodeRecord<oldAfterLinks)afterLinks-=length;
            if(old.inodeRecord<*replacedRecord){
                records.erase(records.begin()+*replacedRecord);
                records.erase(records.begin()+old.inodeRecord,records.begin()+old.inodeRecord+length);
            }else{
                records.erase(records.begin()+old.inodeRecord,records.begin()+old.inodeRecord+length);
                records.erase(records.begin()+*replacedRecord);
            }
            // The directory record was already excluded from the insertion position.
            if(victim->first<parent)insertAt-=length;
            removedKind=CatalogKind::RegularFile;
        }
        records.insert(records.begin()+afterLinks,std::make_move_iterator(newLinks.begin()),
                       std::make_move_iterator(newLinks.end()));
        if(insertAt>=afterLinks)insertAt+=newLinks.size();
        records.insert(records.begin()+insertAt,std::move(added));
        records.insert(records.end(),std::make_move_iterator(newMaps.begin()),
                       std::make_move_iterator(newMaps.end()));
        return records;
    }
    if(create){
        const bool isDirectory=create->kind==CatalogKind::Directory;
        const bool symlink=create->kind==CatalogKind::SymbolicLink;
        Set(parentInode,56,target->second.children+1,4);
        records.insert(records.begin()+insertAt,std::move(added));
        records.push_back(WriterDisk::Inode(name,entryId,time,parent,EntryMode(create->kind),isDirectory?0:1,dataSize,create->finder,DeviceNumber(create->posix)));
        SetPosixMetadata(records.back().value,create->posix);
        if(create->locked)Set(records.back().value,68,Le32(records.back().value.data()+68)|CopiedProtectionFlags(create->posix),4);
        if(create->attributes)SetSecurityFlag(records.back().value,*create->attributes);
        copyDates(records.back().value);
        auto attributes=EncodeAttributes(entryId,create->attributes,resourceWrite,streams);
        if(resourceWrite)SetResourceFlag(records.back().value,resourceWrite->size!=0);
        if(create->compression)WriteCompressionAttribute(attributes,records.back().value,entryId,dataSize,create->compression,streams);
        if(symlink){
            Check(create->linkTarget!=nullptr,"Missing APFS symbolic link target");
            attributes.push_back(LinkTargetRecord(entryId,*create->linkTarget));
        }
        SortAttributes(attributes);
        records.insert(records.end(),std::make_move_iterator(attributes.begin()),std::make_move_iterator(attributes.end()));
        streamAt=records.size();
    }else{
        const auto found=links.find(entryId);
        Check(found!=links.end(),"The APFS entry no longer exists");
        const auto& link=found->second;
        const auto group=siblingGroups.find(entryId);
        const bool shared=group!=siblingGroups.end();
        const bool inFolder=link.parent==parent||(shared&&
            std::any_of(group->second.begin(),group->second.end(),
                        [&](uint64_t id){return siblings.at(id).parent==parent;}));
        Check((rename||remove)?selectedRecord.has_value():inFolder,
              "The APFS entry is no longer in the selected folder");
        Check(!replace||link.kind==CatalogKind::RegularFile,"APFS data replacement requires a regular file");
        auto& inode=records[link.inodeRecord].value;
        const auto bsdFlags=Le32(inode.data()+68);
        Check(!(bsdFlags&(SystemImmutable|AppendOnlyMask))&&(!(bsdFlags&UserImmutable)||(metadata&&metadata->finder&&!metadata->locked)),
              "Unlock the APFS entry before making changes");
        if(remove&&selectedSibling){
            const auto& deleted=siblings.at(selectedSibling);
            const auto& group=siblingGroups.at(entryId);
            const auto first=*group.begin();
            const auto survivorId=first==selectedSibling?*std::next(group.begin()):first;
            const auto& survivor=siblings.at(survivorId);
            const auto count=uint32_t(group.size()-1);
            Set(inode,56,count,4);
            Set(inode,32,time,8);
            if(first==selectedSibling){
                const auto mode=Le16(inode.data()+80);
                auto updated=WriterDisk::Inode(survivor.name,entryId,0,survivor.parent,mode,count,
                                                link.dataSize,link.finder?&*link.finder:nullptr);
                std::copy_n(inode.begin(),size_t(92),updated.value.begin());
                Set(updated.value,0,survivor.parent,8);
                inode=std::move(updated.value);
            }
            Set(parentInode,56,target->second.children-1,4);
            std::vector<size_t> discard{*selectedRecord,*deleted.linkRecord,*deleted.mapRecord};
            if(count==1){
                records[survivor.directoryRecord].value.resize(18);
                discard.push_back(*survivor.linkRecord);
                discard.push_back(*survivor.mapRecord);
            }
            std::sort(discard.rbegin(),discard.rend());
            for(const auto index:discard)records.erase(records.begin()+index);
            return records;
        }
        if(rename&&selectedSibling){
            auto& sibling=siblings.at(selectedSibling);
            sibling.name=name;
            records[*sibling.linkRecord]=sibling.LinkRecord(selectedSibling);
            if(*selectedRecord!=link.entryRecord){
                Set(inode,32,time,8);
                added.value=std::move(records[*selectedRecord].value);
                records.erase(records.begin()+*selectedRecord);
                records.insert(records.begin()+insertAt,std::move(added));
                return records;
            }
        }
        if(metadata&&!metadata->finder){
            Check(link.kind==CatalogKind::Directory,"APFS folder timestamps require a directory");
            Set(inode,32,time,8);
            editDates(inode);
            return records;
        }

        const auto afterInode=link.inodeRecord+1;
        const auto retireAttribute=[&](size_t index){
            const auto& value=records[index].value;
            if(Le16(value.data())==1)retiredStreams.insert(Le64(value.data()+4));
        };
        const auto rewriteAttributes=[&](const HfsPlus::Attributes* ordinary,const ResourceWrite* resource,size_t fileRecords){
            auto attributes=EncodeAttributes(entryId,ordinary,resource,streams);
            for(size_t index=afterInode;index<afterInode+link.attributes;++index){
                if(link.compressionRecord==index){
                    if(!replace)attributes.push_back(std::move(records[index]));
                    continue;
                }
                const auto& oldAttribute=records[index];
                if(Le16(oldAttribute.value.data())==6){
                    if(!linkEdit)attributes.push_back(std::move(records[index]));
                    continue;
                }
                const bool isResource=link.resourceRecord==index;
                if(isResource?!resource:!ordinary)attributes.push_back(std::move(records[index]));
                else retireAttribute(index);
            }
            if(linkEdit)attributes.push_back(LinkTargetRecord(entryId,linkEdit->target));
            if(ordinary)SetSecurityFlag(inode,*ordinary);
            if(resource)SetResourceFlag(inode,resource->size!=0);
            if(replace)WriteCompressionAttribute(attributes,inode,entryId,dataSize,replace->compression,streams);
            SortAttributes(attributes);
            // Replacing the data stream also removes the records following the
            // attributes. Keep sibling links ahead of the new stream records.
            if(replace&&shared){
                for(const auto id:group->second)
                    attributes.push_back(std::move(records[*siblings.at(id).linkRecord]));
            }
            records.erase(records.begin()+afterInode,records.begin()+afterInode+link.attributes+fileRecords);
            records.insert(records.begin()+afterInode,std::make_move_iterator(attributes.begin()),
                           std::make_move_iterator(attributes.end()));
            return afterInode+attributes.size();
        };
        if(linkEdit){
            Check(link.kind==CatalogKind::SymbolicLink,"Can only replace an APFS symbolic link");
            auto updated=WriterDisk::Inode(link.name,entryId,0,parent,0120777,1,0,&linkEdit->finder);
            const auto flags=(Le64(inode.data()+48)&~WriterDisk::HasFinderInfo)|
                             (Le64(updated.value.data()+48)&WriterDisk::HasFinderInfo);
            std::copy_n(inode.begin(),92,updated.value.begin());
            Set(updated.value,48,flags,8);
            Set(updated.value,68,linkEdit->locked?bsdFlags|CopiedProtectionFlags(linkEdit->posix):bsdFlags,4);
            SetPosixMetadata(updated.value,linkEdit->posix);
            Set(updated.value,24,time,8);
            Set(updated.value,32,time,8);
            copyDates(updated.value);
            inode=std::move(updated.value);
            rewriteAttributes(&linkEdit->attributes,nullptr,0);
            return records;
        }
        if(attributesEdit){
            Set(inode,32,time,8);
            rewriteAttributes(&attributesEdit->attributes,nullptr,0);
            return records;
        }
        if(resourceEdit){
            Check(link.kind==CatalogKind::RegularFile,"Select a regular file for APFS resource import");
            Set(inode,24,CatalogTimestamp(resourceEdit->modified),8);
            Set(inode,32,time,8);
            rewriteAttributes(nullptr,resourceWrite,0);
            return records;
        }
        const auto mode=Le16(inode.data()+80);
        const auto count=Le32(inode.data()+56);
        const auto existingFinder=link.finder?&*link.finder:nullptr;
        const auto device=IsDevice(SpecialType(mode>>12))?
            std::optional<uint32_t>(Le32(inode.data()+inode.size()-8)):std::nullopt;
        const auto old=WriterDisk::Inode(link.name,entryId,0,link.parent,mode,count,link.dataSize,existingFinder,device);
        constexpr size_t inodeHeader=92;
        Check(inode.size()==old.value.size()&&
              !memcmp(inode.data()+inodeHeader,old.value.data()+inodeHeader,inode.size()-inodeHeader),
              "APFS writes require native inode name fields");
        if(remove){
            Check(link.kind!=CatalogKind::Directory||directories.at(entryId).children==0,
                  "The APFS folder is not empty");
            for(size_t index=afterInode;index<afterInode+link.attributes;++index)retireAttribute(index);
            removedKind=link.kind;
            Set(parentInode,56,target->second.children-1,4);
            const auto first=records.begin()+link.inodeRecord;
            const auto end=first+1+link.attributes+link.streamRecords;
            if(link.entryRecord>link.inodeRecord){
                records.erase(records.begin()+link.entryRecord);
                records.erase(records.begin()+link.inodeRecord,
                              records.begin()+link.inodeRecord+1+link.attributes+link.streamRecords);
            }else{
                records.erase(first,end);
                records.erase(records.begin()+link.entryRecord);
            }
            return records;
        }
        const auto updatedFinder=metadata?metadata->finder:replace&&replace->finder?replace->finder:existingFinder;
        const auto& updatedName=(replace||metadata)?link.name:name;
        auto updated=WriterDisk::Inode(updatedName,entryId,0,link.parent,mode,count,
                                       replace?dataSize:link.dataSize,updatedFinder,device);
        const auto newFinderFlag=Le64(updated.value.data()+48)&WriterDisk::HasFinderInfo;
        std::copy_n(inode.begin(),inodeHeader,updated.value.begin());
        Set(updated.value,48,(Le64(inode.data()+48)&~WriterDisk::HasFinderInfo)|newFinderFlag,8);
        Set(updated.value,32,time,8);
        if(replace){
            SetPosixMetadata(updated.value,replace->posix);
            if(replace->locked)Set(updated.value,68,Le32(updated.value.data()+68)|CopiedProtectionFlags(replace->posix),4);
            if(!replace->preserveDates){
                Set(updated.value,24,time,8);
                copyDates(updated.value);
            }
        }
        inode=std::move(updated.value);
        if(metadata){
            Set(inode,68,(bsdFlags&~UserImmutable)|(metadata->locked?metadata->lockFlags:0),4);
            editDates(inode);
            return records;
        }
        if(replace){
            streamAt=rewriteAttributes(replace->attributes,resourceWrite,link.streamRecords);
            return records;
        }

        added.value=std::move(records[link.entryRecord].value);
        records.erase(records.begin()+link.entryRecord);
        records.insert(records.begin()+insertAt,std::move(added));
    }
    return records;
}

using CatalogLeaves=std::vector<std::vector<OwnedRecord>>;
CatalogLeaves PartitionCatalog(std::vector<OwnedRecord> records)
{
    size_t totalBytes=0;
    for(const auto& record:records)totalBytes+=record.key.size()+record.value.size();
    CatalogLeaves leaves;
    if(WriterDisk::FitsCatalogLeaf(records.size(),totalBytes)){
        leaves.push_back(std::move(records));
        return leaves;
    }
    leaves.emplace_back();
    size_t bytes=0;
    for(auto& record:records){
        const auto size=record.key.size()+record.value.size();
        if(!WriterDisk::FitsCatalogLeaf(leaves.back().size()+1,bytes+size)){
            leaves.emplace_back();
            bytes=0;
        }
        Check(WriterDisk::FitsCatalogLeaf(1,size),"APFS catalog record exceeds a leaf");
        bytes+=size;
        leaves.back().push_back(std::move(record));
    }
    Check(leaves.size()>1,"APFS catalog root has insufficient space");
    return leaves;
}

OwnedRecord CatalogSeparator(const std::vector<OwnedRecord>& child,uint64_t oid)
{
    OwnedRecord record{child.front().key,Bytes(8)};
    Set(record.value,0,oid,8);
    return record;
}

CatalogLeaves CatalogIndex(const CatalogLeaves& leaves,uint64_t firstOid)
{
    std::vector<OwnedRecord> separators;
    for(size_t i=0;i<leaves.size();++i)separators.push_back(CatalogSeparator(leaves[i],firstOid+i));
    return PartitionCatalog(std::move(separators));
}

std::vector<Mapping> StageCatalog(Writes& writes,const CatalogLeaves& leaves,const std::vector<uint64_t>& blocks,
                                  size_t firstChild,uint64_t rootOid,uint64_t firstOid,uint64_t tx)
{
    std::vector<Mapping> mappings{{rootOid,blocks[0]}};
    if(leaves.size()==1){
        StageObject(writes,blocks[0],EncodeLeaf(leaves.front(),0x40),rootOid,tx,2,14);
        return mappings;
    }
    auto index=CatalogIndex(leaves,firstOid);
    size_t keys=0,longestKey=0,longestValue=8;
    for(size_t i=0;i<leaves.size();++i){
        const auto& leaf=leaves[i];
        const auto oid=firstOid+i,block=blocks[firstChild+i];
        keys+=leaf.size();
        for(const auto& record:leaf){
            longestKey=(std::max)(longestKey,record.key.size());
            longestValue=(std::max)(longestValue,record.value.size());
        }
        StageObject(writes,block,WriterDisk::EncodeNode(leaf,0x40,false,0),oid,tx,3,14);
        mappings.push_back({oid,block});
    }
    std::vector<OwnedRecord> rootIndex;
    if(index.size()==1)rootIndex=std::move(index.front());
    else for(size_t i=0;i<index.size();++i){
        const auto offset=leaves.size()+i;
        const auto oid=firstOid+offset,block=blocks[firstChild+offset];
        rootIndex.push_back(CatalogSeparator(index[i],oid));
        StageObject(writes,block,WriterDisk::EncodeNode(index[i],0x40,false,1),oid,tx,3,14);
        mappings.push_back({oid,block});
    }
    auto root=WriterDisk::EncodeNode(rootIndex,0x40,true,index.size()==1?1:2);
    constexpr auto footer=Block-WriterDisk::TreeFooter;
    Set(root,footer+16,longestKey,4);
    Set(root,footer+20,longestValue,4);
    Set(root,footer+24,keys,8);
    Set(root,footer+32,mappings.size(),8);
    StageObject(writes,blocks[0],std::move(root),rootOid,tx,2,14);
    return mappings;
}

void StageExtentTree(Writes& writes,const CatalogLeaves& leaves,const std::vector<uint64_t>& allocation,
                     size_t firstLeaf,uint64_t transaction)
{
    constexpr uint32_t flags=0x50;
    const auto rootBlock=allocation[6];
    if(leaves.size()==1){
        StageObject(writes,rootBlock,EncodeLeaf(leaves.front(),flags),rootBlock,transaction,Physical|2,15);
        return;
    }
    std::vector<OwnedRecord> separators;
    size_t records=0;
    for(size_t index=0;index<leaves.size();++index){
        const auto block=allocation[firstLeaf+index];
        const auto& leaf=leaves[index];
        OwnedRecord separator{leaf.front().key,Bytes(8)};
        Set(separator.value,0,block,8);
        separators.push_back(std::move(separator));
        records+=leaf.size();
        StageObject(writes,block,WriterDisk::EncodeNode(leaf,flags,false,0),block,transaction,Physical|3,15);
    }
    auto root=WriterDisk::EncodeNode(separators,flags,true,1);
    constexpr auto footer=Block-WriterDisk::TreeFooter;
    Set(root,footer+20,20,4);
    Set(root,footer+24,records,8);
    Set(root,footer+32,leaves.size()+1,8);
    StageObject(writes,rootBlock,std::move(root),rootBlock,transaction,Physical|2,15);
}

struct CatalogVolume {
    WritableMap containerMap;
    uint64_t address;
    Bytes volume;
};

CatalogVolume ReadCatalogVolume(Source& source,const Container& container,uint64_t selected=0)
{
    Check(!container.offset&&container.checkpointBlock&&container.blockSize==Block&&
          !container.volumes.empty()&&
          (selected||container.volumes.size()==1)&&source.Size()==container.blockCount*Block,
          "APFS writes require a raw image and an unambiguous volume selection");
    const auto oid=selected?selected:container.volumes.front();
    auto map=ReadWritableMap(source,container,container.objectMap);
    Check(map.mappings.size()==container.volumes.size()&&!map.leaves.size()&&
          std::all_of(map.mappings.begin(),map.mappings.end(),[&](const auto& mapping){
              return std::count(container.volumes.begin(),container.volumes.end(),mapping.oid)==1;
          }),"Unsupported APFS container volume mappings");
    const auto mapping=std::find_if(map.mappings.begin(),map.mappings.end(),[&](const auto& item){return item.oid==oid;});
    Check(mapping!=map.mappings.end(),"APFS selected volume is no longer present");
    const auto address=mapping->block;
    auto volume=ReadObject(source,container,address,oid,container.transaction,13);

    return {std::move(map),address,std::move(volume)};
}

void RenameExternalInode(Bytes& inode,const std::string& name,const std::string& replacement)
{
    Check(inode.size()>=96,"Missing APFS inode name fields");
    const auto descriptors=Le16(inode.data()+92);
    size_t offset=96+size_t(descriptors)*4;
    Check(offset<=inode.size()&&Le16(inode.data()+94)==inode.size()-offset,
          "Invalid APFS inode field table size");
    const auto valuesStart=offset;
    bool found=false;
    for(size_t i=0;i<descriptors;++i){
        const auto field=96+i*4;
        const auto length=Le16(inode.data()+field+2);
        auto padded=size_t(DivideUp(length,8)*8);
        Check(Range(offset,padded,inode.size()),"Truncated APFS inode field");
        if(inode[field]==4){
            Check(!found&&length==name.size()+1&&!memcmp(inode.data()+offset,name.c_str(),length),
                  "APFS inode name differs from its directory entry");
            const auto replacementLength=replacement.size()+1;
            Bytes fieldBytes(size_t(DivideUp(replacementLength,8)*8));
            std::copy(replacement.begin(),replacement.end(),fieldBytes.begin());
            inode.erase(inode.begin()+offset,inode.begin()+offset+padded);
            inode.insert(inode.begin()+offset,fieldBytes.begin(),fieldBytes.end());
            const auto used=inode.size()-valuesStart;
            Check(used<=UINT16_MAX,"APFS inode fields exceed the on-disk size limit");
            Set(inode,field+2,replacementLength,2);
            Set(inode,94,used,2);
            padded=fieldBytes.size();
            found=true;
        }
        offset+=padded;
    }
    Check(found&&offset==inode.size(),"Invalid APFS inode name fields");
}

void SetExternalFinderInfo(Bytes& inode,const FinderBytes& finder)
{
    Check(inode.size()>=96,"Missing APFS inode extended fields");
    const auto fields=Le16(inode.data()+92);
    const size_t payload=96+size_t(fields)*4;
    Check(payload<=inode.size()&&Le16(inode.data()+94)==inode.size()-payload,
          "Invalid APFS inode field table size");
    size_t offset=payload;
    std::optional<std::pair<size_t,size_t>> found;
    for(size_t i=0;i<fields;++i){
        const auto descriptor=96+i*4;
        const auto length=Le16(inode.data()+descriptor+2);
        const auto padded=size_t(DivideUp(length,8)*8);
        Check(Range(offset,padded,inode.size()),"Truncated APFS inode field");
        if(inode[descriptor]==7){
            Check(!found&&length==finder.size(),"Invalid APFS Finder information field");
            found.emplace(descriptor,offset);
        }
        offset+=padded;
    }
    auto flags=Le64(inode.data()+48);
    Check(offset==inode.size()&&bool(flags&WriterDisk::HasFinderInfo)==found.has_value(),
          "APFS Finder flag and extended fields disagree");
    const bool present=std::any_of(finder.begin(),finder.end(),[](uint8_t byte){return byte!=0;});
    if(found&&present){
        std::copy(finder.begin(),finder.end(),inode.begin()+found->second);
    }else if(found){
        inode.erase(inode.begin()+found->second,inode.begin()+found->second+finder.size());
        inode.erase(inode.begin()+found->first,inode.begin()+found->first+4);
        Set(inode,92,fields-1,2);
        Set(inode,94,inode.size()-(payload-4),2);
    }else if(present){
        Check(inode.size()-payload<=UINT16_MAX-finder.size(),"APFS inode fields exceed the on-disk size limit");
        inode.insert(inode.begin()+payload,{7,0x20,uint8_t(finder.size()),0});
        inode.insert(inode.end(),finder.begin(),finder.end());
        Set(inode,92,fields+1,2);
        Set(inode,94,inode.size()-(payload+4),2);
    }
    flags=(flags&~WriterDisk::HasFinderInfo)|(present?WriterDisk::HasFinderInfo:0);
    Set(inode,48,flags,8);
}

void InsertExternalDirectoryRecord(std::vector<OwnedRecord>& catalog,OwnedRecord record,
                                   uint64_t parent,const std::string& name,bool fold)
{
    const auto parentKey=(uint64_t(9)<<60)|parent;
    const auto hash=Le32(record.key.data()+8)&~1023u;
    const auto comparison=NormalizedName(name,fold);
    auto insert=catalog.end();
    for(auto entry=catalog.begin();entry!=catalog.end();++entry){
        Check(entry->key.size()>=8,"Truncated APFS catalog key");
        const auto raw=Le64(entry->key.data());
        if((raw&IdMask)>parent||((raw&IdMask)==parent&&raw>>60>9)){
            if(insert==catalog.end())insert=entry;
            break;
        }
        if(raw!=parentKey)continue;
        const auto& key=entry->key;
        Check(key.size()>=14&&(Le32(key.data()+8)&1023)==key.size()-12&&!key.back(),
              "Invalid APFS directory name");
        const std::string existing(reinterpret_cast<const char*>(key.data()+12),key.size()-13);
        Check(NormalizedName(existing,fold)!=comparison,"The APFS entry name already exists");
        const auto existingHash=Le32(key.data()+8)&~1023u;
        if(insert==catalog.end()&&(existingHash>hash||(existingHash==hash&&existing>name)))insert=entry;
    }
    catalog.insert(insert,std::move(record));
}

void TouchExternalParent(std::vector<OwnedRecord>& catalog,uint64_t parent,uint64_t time,int childDelta)
{
    const auto parentInode=(uint64_t(3)<<60)|parent;
    Bytes* value=nullptr;
    for(auto& entry:catalog){
        if(entry.key.size()!=8||Le64(entry.key.data())!=parentInode)continue;
        Check(!value&&entry.value.size()>=92&&(Le16(entry.value.data()+80)&0170000)==0040000,
              "Invalid APFS parent inode");
        value=&entry.value;
    }
    Check(value,"APFS parent inode was not found");
    Check(!(Le32(value->data()+68)&WriteProtectionMask),
          "Unlock the APFS folder before changing its entries");
    if(childDelta){
        const auto children=int64_t(Le32(value->data()+56))+childDelta;
        Check(children>=0&&children<=INT32_MAX,"Invalid APFS directory child count");
        Set(*value,56,uint64_t(children),4);
    }
    Set(*value,24,time,8);
    Set(*value,32,time,8);
}

void CollectExternalRetiredExtents(const std::vector<OwnedRecord>& catalog,uint64_t inode,
                                   const std::set<uint64_t>& removeStreams,const CatalogRecords& extentTree,
                                   std::set<uint64_t>& retired)
{
    if(removeStreams.empty())return;
    // Extent reference counts account for physical ranges, not for two
    // catalog objects claiming the same logical stream ID.
    for(const auto& record:catalog){
        const auto key=Le64(record.key.data());
        if((key&IdMask)==inode)continue;
        if(key>>60==3){
            Check(record.value.size()>=16&&!removeStreams.count(Le64(record.value.data()+8)),
                  "APFS edit would retire another inode's data stream");
        }else if(key>>60==4){
            Check(record.value.size()>=4,"Invalid APFS attribute record");
            if((Le16(record.value.data())&3)==1)
                Check(record.value.size()==52&&!removeStreams.count(Le64(record.value.data()+4)),
                      "APFS edit would retire another inode's attribute stream");
        }
    }
    // The shared-extent path reconciles references against the edited catalog.
    if(extentTree.verifiedPhysicalReferences)return;
    std::map<uint64_t,uint64_t> owned;
    for(const auto& extent:extentTree.records){
        if(Le64(extent.value.data()+8)==inode&&Le32(extent.value.data()+16)==1)
            owned.emplace(Le64(extent.key.data())&IdMask,Le64(extent.value.data())&IdMask);
    }
    for(const auto& record:catalog){
        const auto key=Le64(record.key.data());
        if(key>>60!=8||!removeStreams.count(key&IdMask))continue;
        Check(record.key.size()==16&&record.value.size()==24,"Invalid APFS removed extent");
        const auto block=Le64(record.value.data()+8),bytes=Le64(record.value.data())&((uint64_t(1)<<56)-1);
        const auto run=owned.find(block);
        Check(run!=owned.end()&&bytes==run->second*Block&&retired.insert(block).second,
              "APFS removed extent does not have an exclusive reference");
    }
}

uint32_t ReadExternalCompressionMode(Source& source,const std::vector<OwnedRecord>& catalog,
                                     const CatalogRecords& extentTree,const Bytes& attribute,
                                     uint64_t inode,uint64_t logicalSize,std::set<uint64_t>& streams)
{
    Check(attribute.size()>=4&&Le16(attribute.data()+2)==attribute.size()-4,
          "Invalid APFS compression attribute length");
    const auto storage=Le16(attribute.data());
    Bytes streamedHeader;
    const auto* header=attribute.data()+4;
    if(storage==1){
        Check(attribute.size()==52&&Le64(header+8)>=16,
              "Invalid APFS compression attribute stream");
        const auto stream=Le64(header);
        Check(stream>inode&&streams.insert(stream).second,"Invalid APFS compression stream ID");
        const auto first=std::find_if(catalog.begin(),catalog.end(),[&](const auto& record){
            return record.key.size()==16&&Le64(record.key.data())==((uint64_t(8)<<60)|stream)&&
                   !Le64(record.key.data()+8);
        });
        Check(first!=catalog.end()&&first->value.size()==24,"Missing APFS compression stream header extent");
        const auto block=Le64(first->value.data()+8);
        const auto owned=std::find_if(extentTree.records.begin(),extentTree.records.end(),[&](const auto& record){
            return (Le64(record.key.data())&IdMask)==block&&Le64(record.value.data()+8)==inode&&
                   Le32(record.value.data()+16)==1;
        });
        Check(block&&block<source.Size()/Block&&owned!=extentTree.records.end(),
              "APFS compression header has no exclusive extent");
        if(extentTree.cipher){
            const auto crypto=Le64(first->value.data()+16);
            Check((Le64(first->value.data())>>56)==1&&crypto&&crypto<=UINT64_MAX/(Block/512),
                  "Invalid encrypted APFS compression stream extent");
            streamedHeader=HfsPlus::Disk::Read(source,block*Block,512);
            extentTree.cipher->Decrypt(crypto*(Block/512),streamedHeader.data(),streamedHeader.size());
        }else streamedHeader=HfsPlus::Disk::Read(source,block*Block,16);
        header=streamedHeader.data();
    }else Check(storage==2&&attribute.size()>=20,"Truncated APFS compression header");
    Check(Le32(header)==0x636d7066&&Le64(header+8)==logicalSize,
          "APFS compression header disagrees with the inode");
    return Le32(header+4);
}

void ReplaceExternalAttributes(std::vector<OwnedRecord>& catalog,uint64_t parent,uint64_t id,
                               const HfsPlus::Attributes* ordinary,const ResourceWrite* resource,
                               const AttributeStreams& attributeStreams,uint64_t modified,uint64_t time,
                               const CatalogRecords& extentTree,std::set<uint64_t>& retired)
{
    const auto inodeKey=(uint64_t(3)<<60)|id;
    const auto parentKey=(uint64_t(9)<<60)|parent;
    const auto inode=std::find_if(catalog.begin(),catalog.end(),[&](const auto& record){
        return record.key.size()==8&&Le64(record.key.data())==inodeKey;
    });
    Check(inode!=catalog.end()&&inode->value.size()>=92,"Missing APFS resource fork inode");
    auto& value=inode->value;
    const auto kind=(Le16(value.data()+80)&0170000)>>12;
    const auto flags=Le32(value.data()+68);
    const bool symlink=kind==uint16_t(CatalogKind::SymbolicLink);
    Check((kind==8||((kind==4||symlink)&&!resource))&&!(flags&WriteProtectionMask)&&
          (!resource||!(flags&BsdCompressed)),
          "The APFS entry is locked or does not support this attribute edit");
    Check(std::any_of(catalog.begin(),catalog.end(),[&](const auto& record){
        return record.key.size()>=14&&Le64(record.key.data())==parentKey&&record.value.size()>=18&&
               Le64(record.value.data())==id&&Le16(record.value.data()+16)==kind;
    }),"The APFS resource fork target no longer exists");

    auto attributes=EncodeAttributes(id,ordinary,resource,attributeStreams);
    std::set<uint64_t> streams,removeStreams;
    std::set<std::string_view> names;
    const auto first=std::next(inode);
    auto after=first;
    for(;after!=catalog.end()&&Le64(after->key.data())==((uint64_t(4)<<60)|id);++after){
        const auto& old=after->value;
        Check(after->key.size()>=12&&!after->key.back()&&old.size()>=4&&Le16(old.data()+2)==old.size()-4,
              "Invalid APFS resource fork attribute");
        const std::string_view name(reinterpret_cast<const char*>(after->key.data()+10),after->key.size()-11);
        const auto storage=Le16(old.data());
        if(name==SymlinkAttribute){
            Check(symlink&&names.insert(name).second&&storage==6&&old.size()>5&&!old.back(),
                  "Invalid APFS symbolic-link target record");
            attributes.push_back(*after);
            continue;
        }
        Check(names.insert(name).second&&(!resource||name!=CompressionAttribute)&&
              (storage==2||(storage==1&&old.size()==52)),
              "Unsupported APFS attribute record");
        const bool replace=(resource&&name==ResourceAttribute)||(ordinary&&OrdinaryAttribute(name));
        if(storage==1){
            const auto stream=Le64(old.data()+4);
            Check(stream>id&&streams.insert(stream).second,"Invalid APFS attribute stream ID");
            if(replace)removeStreams.insert(stream);
        }
        if(!replace)attributes.push_back(*after);
    }
    Check(!symlink||names.count(SymlinkAttribute),"Missing APFS symbolic-link target");
    CollectExternalRetiredExtents(catalog,id,removeStreams,extentTree,retired);
    if(ordinary)SetSecurityFlag(value,*ordinary);
    if(resource){
        SetResourceFlag(value,resource->size!=0);
        Set(value,24,CatalogTimestamp(modified),8);
    }
    Set(value,32,time,8);
    SortAttributes(attributes);
    const auto insert=catalog.erase(first,after);
    catalog.insert(insert,std::make_move_iterator(attributes.begin()),std::make_move_iterator(attributes.end()));
    if(!removeStreams.empty()){
        catalog.erase(std::remove_if(catalog.begin(),catalog.end(),[&](const auto& record){
            const auto key=Le64(record.key.data());
            return key>>60==8&&removeStreams.count(key&IdMask);
        }),catalog.end());
    }
}

uint64_t ExclusiveExternalDataStream(const std::vector<OwnedRecord>& catalog,uint64_t inode,
                                     const Bytes& value,const CatalogRecords& extents)
{
    const auto stream=Le64(value.data()+8);
    if(!extents.verifiedPhysicalReferences){
        Check(stream==inode,"APFS edit requires a private data stream");
        return stream;
    }
    Check(stream>=16&&stream<=IdMask,"Invalid APFS file data stream ID");
    for(const auto& record:catalog){
        const auto key=Le64(record.key.data());
        if(key>>60==3&&(key&IdMask)!=inode){
            Check(record.value.size()>=16&&Le64(record.value.data()+8)!=stream&&
                  Le64(record.value.data()+8)!=inode,"APFS edit requires an unshared logical data stream");
        }
        if(key>>60==6&&(key&IdMask)==stream)
            Check(record.value.size()==4&&Le32(record.value.data())==1,"APFS edit requires a private data stream reference");
        if(stream!=inode&&(key&IdMask)==inode)
            Check(key>>60!=6&&key>>60!=8,"APFS replacement data stream ID is already in use");
    }
    return stream;
}

std::string ExternalInodeName(const Bytes& inode);

size_t ReplaceExternalFile(Source& source,std::vector<OwnedRecord>& catalog,const CatalogReplaceData& edit,
                           uint64_t size,uint64_t time,const ResourceWrite* resource,
                           const AttributeStreams& streams,const CatalogRecords& extentTree,
                           std::set<uint64_t>& retired)
{
    Check(resource,"External APFS overwrite requires both forks");
    const auto inodeKey=(uint64_t(3)<<60)|edit.inode;
    const auto inode=std::find_if(catalog.begin(),catalog.end(),[&](const auto& record){
        return record.key.size()==8&&Le64(record.key.data())==inodeKey;
    });
    Check(inode!=catalog.end()&&inode->value.size()>=100,"Invalid APFS overwrite inode");
    const auto& old=inode->value;
    const auto flags=Le32(old.data()+68),count=Le32(old.data()+56);
    const auto mode=Le16(old.data()+80);
    const auto primaryParent=Le64(old.data());
    Check(count&&bool(edit.alias)==(count>1)&&
          (mode&0170000)==0100000&&!(flags&WriteProtectionMask),
          "External APFS overwrite cannot replace this file");
    const auto privateStream=ExclusiveExternalDataStream(catalog,edit.inode,old,extentTree);
    const auto name=ExternalInodeName(old);
    std::map<Bytes,Bytes> siblings;
    std::map<Bytes,const Bytes*> onDisk;
    bool selected=false,foundPrimary=false;
    size_t names=0;
    for(const auto& record:catalog){
        Check(record.key.size()>=8,"Truncated APFS catalog key");
        const auto key=Le64(record.key.data()),type=key>>60,parent=key&IdMask;
        if(type==12||(type==5&&parent==edit.inode))
            Check(onDisk.emplace(record.key,&record.value).second,"Repeated APFS sibling record");
        if(type!=9||record.value.size()<18||Le64(record.value.data())!=edit.inode)continue;
        ++names;
        Check(record.key.size()>=14&&record.key.back()==0&&Le16(record.value.data()+16)==8&&
              record.value.size()==(edit.alias?34:18),"Invalid APFS overwrite directory entry");
        const std::string filename(reinterpret_cast<const char*>(record.key.data()+12),record.key.size()-13);
        foundPrimary|=parent==primaryParent&&filename==name;
        if(!edit.alias){
            selected|=parent==edit.parent;
            continue;
        }
        const auto* value=record.value.data();
        const auto alias=Le64(value+26);
        Check(alias>=16&&alias!=edit.inode&&Le16(value+18)==1&&Le16(value+20)==8&&
              value[22]==1&&Le16(value+24)==8,"Invalid APFS overwrite sibling ID");
        const CatalogSibling sibling{edit.inode,parent,filename};
        auto link=sibling.LinkRecord(alias),map=sibling.MapRecord(alias);
        Check(siblings.emplace(std::move(link.key),std::move(link.value)).second&&
              siblings.emplace(std::move(map.key),std::move(map.value)).second,"Repeated APFS overwrite sibling ID");
        selected|=alias==edit.alias&&parent==edit.parent;
    }
    Check(selected&&foundPrimary&&names==count,"The APFS overwrite target no longer exists");
    for(const auto& [key,value]:siblings){
        const auto record=onDisk.find(key);
        Check(record!=onDisk.end()&&*record->second==value,"APFS sibling records differ from their directory names");
    }
    const auto fields=Le16(old.data()+92);
    size_t offset=96+size_t(fields)*4;
    Check(offset<=old.size(),"Truncated APFS overwrite inode fields");
    uint64_t oldSize=0;
    std::optional<size_t> oldStream;
    std::optional<FinderBytes> oldFinder;
    for(size_t i=0;i<fields;++i){
        const auto descriptor=96+4*i;
        const auto length=Le16(old.data()+descriptor+2);
        const auto padded=DivideUp(length,8)*8;
        Check(Range(offset,padded,old.size()),"Truncated APFS overwrite inode field");
        if(old[descriptor]==8&&length==40){
            oldStream=offset;
            oldSize=Le64(old.data()+offset);
        }
        if(old[descriptor]==7&&length==FinderBytes{}.size()){
            oldFinder.emplace();
            std::copy_n(old.data()+offset,length,oldFinder->begin());
        }
        offset+=size_t(padded);
    }
    const bool wasCompressed=(flags&BsdCompressed)!=0;
    Check(!wasCompressed||!oldSize,"APFS compressed overwrite target unexpectedly has a data stream");
    auto oldLayout=WriterDisk::Inode(name,edit.inode,0,primaryParent,mode,count,oldSize,
                                           oldFinder?&*oldFinder:nullptr);
    if(oldStream&&old.size()==oldLayout.value.size()){
        const auto canonicalStream=96+size_t(Le16(oldLayout.value.data()+92))*4+DivideUp(name.size()+1,8)*8;
        const auto first=extentTree.cipher?16u:24u;
        std::copy_n(old.data()+*oldStream+first,40-first,oldLayout.value.data()+canonicalStream+first);
    }
    const auto fieldsByType=[](const Bytes& inode){
        std::map<uint8_t,Bytes> fields;
        const auto count=Le16(inode.data()+92);
        size_t payload=96+size_t(count)*4;
        for(size_t i=0;i<count;++i){
            const auto descriptor=96+i*4;
            const auto padded=size_t(DivideUp(Le16(inode.data()+descriptor+2),8)*8);
            Check(Range(payload,padded,inode.size()),"Invalid APFS overwrite inode field");
            Bytes field(inode.begin()+descriptor,inode.begin()+descriptor+4);
            field.insert(field.end(),inode.begin()+payload,inode.begin()+payload+padded);
            Check(fields.emplace(inode[descriptor],std::move(field)).second,"Repeated APFS overwrite inode field");
            payload+=padded;
        }
        Check(payload==inode.size(),"Invalid APFS overwrite inode field length");
        return fields;
    };
    Check(old.size()==oldLayout.value.size()&&Le32(old.data()+92)==Le32(oldLayout.value.data()+92)&&
          fieldsByType(old)==fieldsByType(oldLayout.value),
          "External APFS overwrite requires a supported inode field layout");
    auto updated=WriterDisk::Inode(name,edit.inode,0,primaryParent,mode,count,size,
                                   edit.finder?edit.finder:oldFinder?&*oldFinder:nullptr);
    if(oldStream&&size){
        const auto newStream=96+size_t(Le16(updated.value.data()+92))*4+DivideUp(name.size()+1,8)*8;
        const auto first=extentTree.cipher?16u:24u;
        std::copy_n(old.data()+*oldStream+first,40-first,updated.value.data()+newStream+first);
    }
    const auto finderFlag=Le64(updated.value.data()+48)&WriterDisk::HasFinderInfo;
    std::copy_n(old.begin(),92,updated.value.begin());
    Set(updated.value,8,edit.inode,8);
    Set(updated.value,48,(Le64(old.data()+48)&~WriterDisk::HasFinderInfo)|finderFlag,8);
    SetResourceFlag(updated.value,resource->size!=0);
    if(edit.attributes)SetSecurityFlag(updated.value,*edit.attributes);
    if(edit.locked)Set(updated.value,68,flags|CopiedProtectionFlags(edit.posix),4);
    SetPosixMetadata(updated.value,edit.posix);
    Set(updated.value,32,time,8);
    if(!edit.preserveDates){
        Set(updated.value,24,edit.dates?(edit.dates->exact?(*edit.dates->exact)[1]:CatalogTimestamp(edit.dates->modified)):time,8);
        if(edit.dates){
            SetFileDates(updated.value,*edit.dates);
        }
    }

    auto attributes=EncodeAttributes(edit.inode,edit.attributes,resource,streams);
    WriteCompressionAttribute(attributes,updated.value,edit.inode,size,edit.compression,streams);
    bool foundCompression=false;
    std::set<uint64_t> removeStreams{privateStream};
    std::vector<OwnedRecord> links;
    auto after=std::next(inode);
    for(;after!=catalog.end()&&(Le64(after->key.data())&IdMask)==edit.inode;++after){
        const auto kind=Le64(after->key.data())>>60;
        if(kind==6||kind==8){
            Check(!wasCompressed,"APFS compressed overwrite target has unexpected data records");
            continue;
        }
        if(kind==5){
            Check(siblings.count(after->key),"Unexpected APFS overwrite sibling link");
            links.push_back(*after);
            continue;
        }
        Check(kind==4&&after->key.size()>=12&&after->key.back()==0&&after->value.size()>=4,
              "Unsupported APFS overwrite record");
        const std::string attr(reinterpret_cast<const char*>(after->key.data()+10),after->key.size()-11);
        const auto storage=Le16(after->value.data());
        if(attr==CompressionAttribute){
            Check(wasCompressed&&!foundCompression,"Invalid APFS compressed overwrite target");
            ReadExternalCompressionMode(source,catalog,extentTree,after->value,
                                        edit.inode,Le64(old.data()+84),removeStreams);
            foundCompression=true;
            continue;
        }
        Check((OrdinaryAttribute(attr)||attr==ResourceAttribute)&&(storage==2||(storage==1&&after->value.size()==52)),
              "Unsupported APFS overwrite attribute");
        if(attr!=ResourceAttribute&&!edit.attributes)attributes.push_back(*after);
        else if(storage==1){
            const auto stream=Le64(after->value.data()+4);
            Check(stream>edit.inode&&removeStreams.insert(stream).second,"Invalid APFS overwrite stream ID");
        }
    }
    Check(wasCompressed==foundCompression,"Missing APFS compression attribute");
    CollectExternalRetiredExtents(catalog,edit.inode,removeStreams,extentTree,retired);
    const auto index=size_t(inode-catalog.begin());
    inode->value=std::move(updated.value);
    catalog.erase(catalog.begin()+index+1,after);
    catalog.erase(std::remove_if(catalog.begin(),catalog.end(),[&](const auto& record){
        const auto key=Le64(record.key.data());
        return (key>>60==8&&removeStreams.count(key&IdMask))||
               (key>>60==6&&(key&IdMask)==privateStream);
    }),catalog.end());
    const auto insert=std::find_if(catalog.begin(),catalog.end(),[&](const auto& record){
        return Le64(record.key.data())==inodeKey;
    });
    Check(insert!=catalog.end(),"Missing APFS replacement inode");
    const auto streamAt=size_t(insert-catalog.begin())+1;
    SortAttributes(attributes);
    attributes.insert(attributes.end(),std::make_move_iterator(links.begin()),std::make_move_iterator(links.end()));
    catalog.insert(catalog.begin()+streamAt,std::make_move_iterator(attributes.begin()),
                   std::make_move_iterator(attributes.end()));
    return streamAt+attributes.size();
}

std::string ExternalInodeName(const Bytes& inode)
{
    Check(inode.size()>=96,"Missing APFS inode name fields");
    const auto fields=Le16(inode.data()+92);
    size_t offset=96+size_t(fields)*4;
    Check(offset<=inode.size(),"Truncated APFS hard-link inode fields");
    std::optional<std::string> primaryName;
    for(size_t i=0;i<fields;++i){
        const auto desc=96+4*i;
        const auto length=Le16(inode.data()+desc+2);
        const auto padded=size_t(DivideUp(length,8)*8);
        Check(Range(offset,padded,inode.size()),"Truncated APFS hard-link inode field");
        if(inode[desc]==4){
            Check(!primaryName&&length>1&&inode[offset+length-1]==0,"Invalid APFS hard-link inode name");
            primaryName.emplace(reinterpret_cast<const char*>(inode.data()+offset),length-1);
        }
        offset+=padded;
    }
    Check(primaryName.has_value(),"Missing APFS hard-link inode name");
    return std::move(*primaryName);
}

bool UnlinkExternalAlias(std::vector<OwnedRecord>& catalog,const CatalogDelete& edit,uint64_t time)
{
    if(!edit.alias)return false;
    struct Alias {
        uint64_t id;
        size_t directory;
        CatalogSibling sibling;
    };
    std::vector<Alias> group;
    std::map<Bytes,size_t> records;
    std::optional<size_t> inodeIndex;
    for(size_t i=0;i<catalog.size();++i){
        const auto& record=catalog[i];
        Check(record.key.size()>=8,"Truncated APFS catalog key");
        const auto raw=Le64(record.key.data()),type=raw>>60,oid=raw&IdMask;
        if(oid==edit.inode&&type==3){
            Check(!inodeIndex&&record.key.size()==8&&record.value.size()>=96,"Invalid APFS hard-link inode");
            inodeIndex=i;
        }
        if(type==12||(type==5&&oid==edit.inode))
            Check(records.emplace(record.key,i).second,"Repeated APFS sibling record");
        if(type!=9||record.value.size()<18||Le64(record.value.data())!=edit.inode)continue;
        if(edit.recursive&&record.value.size()==18&&oid==edit.parent)return false;
        Check(record.value.size()==34&&record.key.size()>=14&&record.key.back()==0&&
              Le16(record.value.data()+16)==8&&Le16(record.value.data()+18)==1&&
              Le16(record.value.data()+20)==8&&record.value[22]==1&&Le16(record.value.data()+24)==8,
              "Invalid APFS hard-link directory entry");
        const auto id=Le64(record.value.data()+26);
        Check(id>=16&&id!=edit.inode,"Invalid APFS sibling ID");
        group.push_back({id,i,{edit.inode,oid,std::string(reinterpret_cast<const char*>(record.key.data()+12),
                                                         record.key.size()-13)}});
    }
    Check(inodeIndex.has_value()&&group.size()>=2,"The APFS hard-link group no longer exists");
    auto& inode=catalog[*inodeIndex].value;
    Check(Le32(inode.data()+56)==group.size()&&(Le16(inode.data()+80)&0170000)==0100000&&
          !(Le32(inode.data()+68)&WriteProtectionMask),
          "Invalid or locked APFS hard-link inode");
    std::sort(group.begin(),group.end(),[](const Alias& a,const Alias& b){return a.id<b.id;});
    std::vector<size_t> discard;
    std::optional<size_t> selected;
    for(size_t i=0;i<group.size();++i){
        const auto& alias=group[i];
        const auto link=alias.sibling.LinkRecord(alias.id),map=alias.sibling.MapRecord(alias.id);
        const auto storedLink=records.find(link.key),storedMap=records.find(map.key);
        Check((!i||group[i-1].id<alias.id)&&storedLink!=records.end()&&storedMap!=records.end()&&
              catalog[storedLink->second].value==link.value&&catalog[storedMap->second].value==map.value,
              "APFS sibling records disagree with the directory entry");
        if(alias.id==edit.alias){
            Check(alias.sibling.parent==edit.parent,"APFS hard link moved to another folder");
            selected=i;
        }
        if(alias.id==edit.alias||group.size()==2){
            discard.push_back(storedLink->second);
            discard.push_back(storedMap->second);
        }
    }
    Check(selected.has_value(),"The APFS hard-link name no longer exists");
    const auto& removed=group[*selected];
    const auto& survivor=group[*selected==0?1:0];
    const auto primaryName=ExternalInodeName(inode);
    const auto primaryParent=Le64(inode.data());
    if(group.size()==2||(primaryParent==removed.sibling.parent&&primaryName==removed.sibling.name)){
        RenameExternalInode(inode,primaryName,survivor.sibling.name);
        Set(inode,0,survivor.sibling.parent,8);
    }
    Set(inode,56,group.size()-1,4);
    Set(inode,32,time,8);
    if(group.size()==2)catalog[survivor.directory].value.resize(18);
    TouchExternalParent(catalog,edit.parent,time,-1);
    discard.push_back(removed.directory);
    std::sort(discard.rbegin(),discard.rend());
    for(const auto index:discard)catalog.erase(catalog.begin()+index);
    return true;
}

CatalogKind DeleteExternalEntry(Source& source,std::vector<OwnedRecord>& catalog,const CatalogDelete& edit,
                                uint64_t time,const CatalogRecords& extentTree,std::set<uint64_t>& retired)
{
    Check((!edit.alias||edit.recursive)&&edit.inode>=16&&edit.inode!=edit.parent,"APFS cannot delete this entry");
    const auto entryKey=(uint64_t(9)<<60)|edit.parent;
    std::optional<size_t> directoryRecord;
    CatalogKind kind=CatalogKind::RegularFile;
    for(size_t i=0;i<catalog.size();++i){
        const auto& record=catalog[i];
        Check(record.key.size()>=8,"Truncated APFS catalog key");
        if(Le64(record.key.data())!=entryKey||record.value.size()<18||Le64(record.value.data())!=edit.inode)continue;
        Check(!directoryRecord&&record.value.size()==18,"APFS deletion requires an unlinked entry");
        kind=CatalogKind(Le16(record.value.data()+16));
        Check(kind==CatalogKind::Directory||kind==CatalogKind::RegularFile||kind==CatalogKind::SymbolicLink||IsSpecialKind(kind),
              "APFS deletion target type is unsupported");
        directoryRecord=i;
    }
    Check(directoryRecord.has_value(),"The APFS deletion target no longer exists");

    const auto inodeKey=(uint64_t(3)<<60)|edit.inode;
    const auto inode=std::find_if(catalog.begin(),catalog.end(),[&](const auto& record){
        return Le64(record.key.data())==inodeKey;
    });
    Check(inode!=catalog.end()&&inode->key.size()==8&&inode->value.size()>=92,"Invalid APFS deletion inode");
    const auto& value=inode->value;
    const bool directory=kind==CatalogKind::Directory;
    Check(Le64(value.data())==edit.parent&&
          (Le16(value.data()+80)&0170000)==(uint16_t(kind)<<12)&&
          !(Le32(value.data()+68)&WriteProtectionMask),
          "APFS deletion requires a supported unlocked inode");
    const bool compressed=(Le32(value.data()+68)&BsdCompressed)!=0;
    Check(!compressed||kind==CatalogKind::RegularFile,"Invalid APFS compressed inode type");
    Check(Le32(value.data()+56)==(directory?0:1),"The APFS folder is populated or file has multiple names");

    std::set<uint64_t> streams;
    const auto privateStream=kind==CatalogKind::RegularFile&&!compressed?
        ExclusiveExternalDataStream(catalog,edit.inode,value,extentTree):edit.inode;
    Check(kind==CatalogKind::RegularFile&&!compressed||Le64(value.data()+8)==edit.inode,
          "Invalid APFS deletion stream ID");
    if(kind==CatalogKind::RegularFile&&!compressed)streams.insert(privateStream);
    bool hadTarget=false,hadCompression=false,hadResource=false,needsResource=false;
    auto after=std::next(inode);
    for(;after!=catalog.end()&&(Le64(after->key.data())&IdMask)==edit.inode;++after){
        const auto type=Le64(after->key.data())>>60;
        if(kind==CatalogKind::RegularFile&&!compressed&&(type==6||type==8))continue;
        Check(type==4&&after->key.size()>=12&&after->key.back()==0&&after->value.size()>=4,
              "APFS deletion found an unsupported entry record");
        const std::string attr(reinterpret_cast<const char*>(after->key.data()+10),after->key.size()-11);
        const auto storage=Le16(after->value.data());
        if(attr==SymlinkAttribute){
            Check(kind==CatalogKind::SymbolicLink&&!hadTarget&&storage==6&&after->value.size()>5&&
                  !after->value.back()&&Le16(after->value.data()+2)==after->value.size()-4,
                  "Invalid APFS symbolic-link target record");
            hadTarget=true;
            continue;
        }
        if(attr==CompressionAttribute){
            Check(compressed&&!hadCompression,
                  "Invalid APFS compression attribute for deletion");
            const auto mode=ReadExternalCompressionMode(source,catalog,extentTree,after->value,
                                                        edit.inode,Le64(value.data()+84),streams);
            needsResource=mode==4||mode==8||mode==10||mode==12||mode==14;
            Check(needsResource||mode==1||mode==3||mode==7||mode==9||mode==11||mode==13,
                  "Unsupported APFS compression mode for deletion");
            hadCompression=true;
            continue;
        }
        Check((OrdinaryAttribute(attr)||(kind==CatalogKind::RegularFile&&attr==ResourceAttribute))&&
              (storage==2||(storage==1&&after->value.size()==52)),
              "APFS deletion found an unsupported attribute");
        if(attr==ResourceAttribute){
            Check(!hadResource,"Repeated APFS resource fork");
            hadResource=true;
        }
        if(storage==1){
            const auto stream=Le64(after->value.data()+4);
            Check(stream>edit.inode&&streams.insert(stream).second,"Invalid APFS deletion stream ID");
        }
    }
    Check(hadTarget==(kind==CatalogKind::SymbolicLink),"Missing APFS symbolic-link target");
    Check(hadCompression==compressed&&(!needsResource||hadResource),
          "APFS compression records disagree with the inode");
    CollectExternalRetiredExtents(catalog,edit.inode,streams,extentTree,retired);
    const auto first=size_t(inode-catalog.begin()),last=size_t(after-catalog.begin());
    const auto entry=*directoryRecord;
    TouchExternalParent(catalog,edit.parent,time,-1);
    if(entry>first){
        catalog.erase(catalog.begin()+entry);
        catalog.erase(catalog.begin()+first,catalog.begin()+last);
    }else{
        catalog.erase(catalog.begin()+first,catalog.begin()+last);
        catalog.erase(catalog.begin()+entry);
    }
    catalog.erase(std::remove_if(catalog.begin(),catalog.end(),[&](const auto& record){
        const auto key=Le64(record.key.data());
        return (key>>60==8&&streams.count(key&IdMask))||
               (key>>60==6&&!streams.empty()&&(key&IdMask)==privateStream);
    }),catalog.end());
    return kind;
}

std::tuple<uint64_t,uint64_t,uint64_t,uint64_t> DeleteExternalTree(Source& source,std::vector<OwnedRecord>& catalog,const CatalogDelete& root,
                                               uint64_t time,const CatalogRecords& extentTree,
                                               std::set<uint64_t>& retired)
{
    std::map<uint64_t,std::vector<CatalogDelete>> children;
    for(const auto& record:catalog){
        Check(record.key.size()>=8,"Truncated APFS catalog key");
        const auto key=Le64(record.key.data()),parent=key&IdMask;
        if(key>>60!=9||parent==1)continue;
        Check(record.value.size()>=18,"Invalid APFS directory entry");
        const auto alias=record.value.size()==34?Le64(record.value.data()+26):0;
        children[parent].push_back({parent,Le64(record.value.data()),alias,true,true});
    }
    std::vector<CatalogDelete> order{root};
    std::set<uint64_t> seen{root.alias?root.alias:root.inode};
    for(size_t i=0;i<order.size();++i){
        if(order[i].alias)continue;
        const auto parent=order[i].inode;
        const auto found=children.find(parent);
        if(found==children.end())continue;
        for(const auto& child:found->second){
            Check(child.inode>=16&&seen.insert(child.alias?child.alias:child.inode).second,
                  "APFS folder tree is cyclic or repeats an entry");
            order.push_back(child);
        }
    }
    uint64_t files=0,folders=0,symlinks=0,special=0;
    for(auto at=order.rbegin();at!=order.rend();++at){
        if(UnlinkExternalAlias(catalog,*at,time))continue;
        const auto kind=DeleteExternalEntry(source,catalog,*at,time,extentTree,retired);
        if(kind==CatalogKind::Directory)++folders;
        else if(kind==CatalogKind::SymbolicLink)++symlinks;
        else if(IsSpecialKind(kind))++special;
        else ++files;
    }
    return {files,folders,symlinks,special};
}

void ReplaceExternalSymlink(std::vector<OwnedRecord>& catalog,const CatalogReplaceSymlink& edit,
                            uint64_t time,const AttributeStreams& streams,const CatalogRecords& extentTree,
                            std::set<uint64_t>& retired)
{
    const auto parentKey=(uint64_t(9)<<60)|edit.parent;
    const auto inodeKey=(uint64_t(3)<<60)|edit.inode;
    bool named=false;
    for(const auto& record:catalog){
        Check(record.key.size()>=8,"Truncated APFS catalog key");
        if(Le64(record.key.data())!=parentKey||record.value.size()<18||Le64(record.value.data())!=edit.inode)continue;
        Check(!named&&record.value.size()==18&&Le16(record.value.data()+16)==10,
              "APFS symbolic-link overwrite requires an unlinked symbolic link");
        named=true;
    }
    Check(named,"The APFS symbolic-link destination no longer exists");
    auto inode=std::find_if(catalog.begin(),catalog.end(),[&](const auto& record){
        return Le64(record.key.data())==inodeKey;
    });
    Check(inode!=catalog.end()&&inode->key.size()==8&&inode->value.size()>=92,
          "Invalid APFS symbolic-link inode");
    auto& value=inode->value;
    const auto flags=Le32(value.data()+68);
    Check(Le64(value.data())==edit.parent&&Le32(value.data()+56)==1&&
          (Le16(value.data()+80)&0170000)==0120000&&!(flags&(WriteProtectionMask|BsdCompressed)),
          "APFS symbolic-link destination is locked or unsupported");
    std::set<uint64_t> oldStreams;
    bool hadTarget=false;
    auto after=std::next(inode);
    for(;after!=catalog.end()&&(Le64(after->key.data())&IdMask)==edit.inode;++after){
        Check(Le64(after->key.data())>>60==4&&after->key.size()>=12&&after->key.back()==0&&after->value.size()>=4,
              "Unsupported APFS symbolic-link record");
        const std::string attr(reinterpret_cast<const char*>(after->key.data()+10),after->key.size()-11);
        const auto kind=Le16(after->value.data());
        if(attr==SymlinkAttribute){
            Check(!hadTarget&&kind==6&&after->value.size()>5&&!after->value.back(),"Invalid APFS symbolic-link target");
            hadTarget=true;
        }else{
            Check(OrdinaryAttribute(attr)&&(kind==2||(kind==1&&after->value.size()==52)),
                  "Unsupported APFS symbolic-link attribute");
            if(kind==1){
                const auto stream=Le64(after->value.data()+4);
                Check(stream>edit.inode&&oldStreams.insert(stream).second,"Invalid APFS symbolic-link stream");
            }
        }
    }
    Check(hadTarget,"Missing APFS symbolic-link target");
    CollectExternalRetiredExtents(catalog,edit.inode,oldStreams,extentTree,retired);
    SetExternalFinderInfo(value,edit.finder);
    SetSecurityFlag(value,edit.attributes);
    SetPosixMetadata(value,edit.posix);
    if(edit.locked)Set(value,68,Le32(value.data()+68)|CopiedProtectionFlags(edit.posix),4);
    Set(value,24,edit.dates?(edit.dates->exact?(*edit.dates->exact)[1]:CatalogTimestamp(edit.dates->modified)):time,8);
    Set(value,32,time,8);
    if(edit.dates){
        SetFileDates(value,*edit.dates);
    }
    auto attributes=EncodeAttributes(edit.inode,&edit.attributes,nullptr,streams);
    attributes.push_back(LinkTargetRecord(edit.inode,edit.target));
    SortAttributes(attributes);
    const auto first=size_t(inode-catalog.begin())+1;
    catalog.erase(catalog.begin()+first,after);
    catalog.erase(std::remove_if(catalog.begin(),catalog.end(),[&](const auto& record){
        const auto key=Le64(record.key.data());
        return key>>60==8&&oldStreams.count(key&IdMask);
    }),catalog.end());
    catalog.insert(catalog.begin()+first,std::make_move_iterator(attributes.begin()),
                   std::make_move_iterator(attributes.end()));
}

void CreateExternalHardLink(std::vector<OwnedRecord>& catalog,const CatalogHardLink& request,
                             const std::string& name,uint64_t nextId,uint64_t time,bool fold)
{
    Check(request.parent>=2&&request.parent!=3&&request.inode>=16,
          "Invalid external APFS hard-link request");
    const auto inodeKey=(uint64_t(3)<<60)|request.inode;
    const auto linkKey=(uint64_t(5)<<60)|request.inode;
    std::optional<size_t> inodeRecord;
    std::vector<size_t> names;
    std::map<Bytes,const Bytes*> siblingRecords;
    size_t afterLinks=0;
    for(size_t i=0;i<catalog.size();++i){
        const auto& record=catalog[i];
        Check(record.key.size()>=8,"Truncated APFS catalog key");
        const auto raw=Le64(record.key.data()),oid=raw&IdMask,type=raw>>60;
        Check(oid<nextId,"APFS catalog contains an object at or after its next ID");
        if(raw==inodeKey){
            Check(!inodeRecord&&record.key.size()==8&&record.value.size()>=92,"Invalid APFS hard-link source inode");
            inodeRecord=i;
        }
        if(oid<request.inode||(oid==request.inode&&type<=5))afterLinks=i+1;
        if(type==12||raw==linkKey)
            Check(siblingRecords.emplace(record.key,&record.value).second,"Repeated APFS sibling record");
        if(type!=9||record.value.size()<18||Le64(record.value.data())!=request.inode)continue;
        Check((record.value.size()==18||record.value.size()==34)&&Le16(record.value.data()+16)==8&&
              record.key.size()>=14&&(Le32(record.key.data()+8)&1023)==record.key.size()-12&&!record.key.back(),
              "APFS hard-link source must be a regular file");
        names.push_back(i);
    }
    Check(inodeRecord.has_value()&&!names.empty(),"The APFS hard-link source no longer exists");
    auto& inode=catalog[*inodeRecord].value;
    const auto count=Le32(inode.data()+56);
    const auto forbidden=request.copiedLockedInode?0u:WriteProtectionMask;
    Check((Le16(inode.data()+80)&0170000)==0100000&&!(Le32(inode.data()+68)&forbidden)&&
          names.size()==count&&count<INT32_MAX,"Invalid or locked APFS hard-link source");
    std::set<uint64_t> identifiers;
    for(const auto index:names){
        const auto& record=catalog[index];
        if(count==1){
            Check(record.value.size()==18&&Le64(inode.data())==(Le64(record.key.data())&IdMask),
                  "Invalid APFS original file name");
            continue;
        }
        Check(record.value.size()==34,"Missing APFS sibling ID");
        const auto* value=record.value.data();
        const auto id=Le64(value+26);
        Check(id>=16&&id<nextId&&id!=request.inode&&identifiers.insert(id).second&&
              Le16(value+18)==1&&Le16(value+20)==8&&value[22]==1&&Le16(value+24)==8,
              "Invalid APFS sibling ID");
        const CatalogSibling sibling{request.inode,Le64(record.key.data())&IdMask,
            std::string(reinterpret_cast<const char*>(record.key.data()+12),record.key.size()-13)};
        const auto link=sibling.LinkRecord(id),map=sibling.MapRecord(id);
        const auto storedLink=siblingRecords.find(link.key),storedMap=siblingRecords.find(map.key);
        Check(storedLink!=siblingRecords.end()&&storedMap!=siblingRecords.end()&&
              *storedLink->second==link.value&&*storedMap->second==map.value,
              "APFS sibling records do not match the selected inode");
    }
    Set(inode,56,count+1,4);
    if(!request.copiedLockedInode)Set(inode,32,time,8);
    TouchExternalParent(catalog,request.parent,time,1);
    std::vector<OwnedRecord> links,maps;
    const auto append=[&](const CatalogSibling& sibling,uint64_t id){
        links.push_back(sibling.LinkRecord(id));
        maps.push_back(sibling.MapRecord(id));
    };
    if(count==1){
        auto& original=catalog[names.front()];
        const auto originalParent=Le64(original.key.data())&IdMask;
        const std::string originalName(reinterpret_cast<const char*>(original.key.data()+12),original.key.size()-13);
        SetSibling(original.value,nextId);
        append({request.inode,originalParent,originalName},nextId);
    }
    const auto aliasId=nextId+1;
    auto added=WriterDisk::DirectoryEntry(name,request.inode,time,request.parent,fold,
                                          uint16_t(CatalogKind::RegularFile));
    SetSibling(added.value,aliasId);
    append({request.inode,request.parent,name},aliasId);
    catalog.insert(catalog.begin()+afterLinks,std::make_move_iterator(links.begin()),
                   std::make_move_iterator(links.end()));
    InsertExternalDirectoryRecord(catalog,std::move(added),request.parent,name,fold);
    catalog.insert(catalog.end(),std::make_move_iterator(maps.begin()),std::make_move_iterator(maps.end()));
}

constexpr uint64_t VolumeGroupInodeSpace=0x10,UnifiedInodeMark=0x0800000000000000;
constexpr uint16_t SystemVolumeRole=1,DataVolumeRole=64;
constexpr size_t VolumeRoleOffset=964,VolumeGroupOffset=1008;

bool InVolumeGroup(const Bytes& volume)
{
    return (Le64(volume.data()+40)&VolumeGroupInodeSpace)!=0;
}

void CheckExternalVolume(const Bytes& volume,bool allowVolumeGroup,bool allowSnapshot=false,bool encrypted=false)
{
    const auto features=Le64(volume.data()+40);
    const auto group=allowVolumeGroup&&features==(2|VolumeGroupInodeSpace);
    Check(!memcmp(volume.data()+32,"APSB",4)&&(features==2||group)&&
          !Le64(volume.data()+48)&&(encrypted?(Le64(volume.data()+56)==1||Le64(volume.data()+56)==5||Le64(volume.data()+56)==12)&&Le64(volume.data()+264)==8&&
                     !Le64(volume.data()+976)&&!Le64(volume.data()+216):
                     (Le64(volume.data()+56)==1||Le64(volume.data()+56)==8)&&Le64(volume.data()+264)==1)&&
          Le32(volume.data()+116)==2&&Le64(volume.data()+216)<=(allowSnapshot?1u:0u),
          "Unsupported APFS volume features for external edits");
    if(group){
        const auto role=Le16(volume.data()+VolumeRoleOffset);
        const auto nextId=Le64(volume.data()+176);
        Check((role==SystemVolumeRole&&nextId>=UnifiedInodeMark+16)||
              (role==DataVolumeRole&&nextId>=16&&nextId<UnifiedInodeMark),
              "Unsupported APFS volume group inode namespace");
        Check(std::any_of(volume.begin()+VolumeGroupOffset,volume.begin()+VolumeGroupOffset+16,
                          [](uint8_t byte){return byte!=0;}),"Missing APFS volume group identifier");
    }
}

struct SnapshotAllocations {
    std::vector<Mapping> mappings;
    std::vector<OwnedRecord> historicalData;
    uint64_t historicalSuper=0;
    std::set<uint64_t> sharedData;
    std::set<uint64_t> catalogBlocks;
    std::vector<DataRun> protectedRuns;
};

WritableMap MapAt(const WritableMap& source,uint64_t xid)
{
    WritableMap result=source;
    result.mappings.clear();
    for(const auto& entry:source.mappings){
        if(entry.xid>xid)continue;
        if(!result.mappings.empty()&&result.mappings.back().oid==entry.oid)
            result.mappings.back()=entry;
        else result.mappings.push_back(entry);
    }
    auto& records=result.mappings;
    records.erase(std::remove_if(records.begin(),records.end(),[](const auto& item){return !item.block;}),records.end());
    return result;
}

void ValidateSharedFileExtents(const CatalogRecords& catalog,const std::vector<OwnedRecord>& extents,bool encrypted=false)
{
    std::map<uint64_t,int64_t> references;
    const auto add=[&](uint64_t block,uint64_t count,int64_t change){
        Check(count&&block<=UINT64_MAX-count,"Invalid APFS snapshot extent range");
        references[block]+=change;
        references[block+count]-=change;
    };
    uint64_t previousEnd=0;
    for(const auto& extent:extents){
        const auto block=Le64(extent.key.data())&IdMask;
        const auto count=Le64(extent.value.data())&IdMask;
        Check(block>=previousEnd,"Overlapping APFS snapshot physical extents");
        add(block,count,Le32(extent.value.data()+16));
        previousEnd=block+count;
    }
    for(const auto& entry:catalog.records){
        if(Le64(entry.key.data())>>60!=8)continue;
        Check(entry.key.size()==16&&entry.value.size()==24,"Invalid APFS snapshot file extent");
        const auto block=Le64(entry.value.data()+8),encoded=Le64(entry.value.data());
        const auto flags=encoded>>56,length=encoded&((uint64_t(1)<<56)-1);
        Check(length&&!(length%Block)&&(!flags||(encrypted&&flags==1)),
              "Unsupported APFS file extent length or flags");
        if(block)add(block,length/Block,-1);
    }
    Check(std::all_of(references.begin(),references.end(),[](const auto& boundary){
        return boundary.second==0;
    }),"APFS snapshot file extents disagree with their reference counts");
}


std::map<uint64_t,uint32_t> CatalogReferenceCounts(const std::vector<OwnedRecord>& catalog,
                                                  const std::vector<HfsPlus::SharedBlockRange>& reused,bool encrypted)
{
    std::map<uint64_t,int64_t> boundaries;
    for(const auto& entry:catalog){
        if(Le64(entry.key.data())>>60!=8)continue;
        Check(entry.key.size()==16&&entry.value.size()==24,"Invalid APFS file extent");
        const auto start=Le64(entry.value.data()+8),encoded=Le64(entry.value.data());
        const auto flags=encoded>>56,bytes=encoded&((uint64_t(1)<<56)-1);
        Check(bytes&&!(bytes%Block)&&(!flags||(encrypted&&flags==1)),"Invalid APFS file extent length or flags");
        if(!start)continue;
        const auto blocks=bytes/Block;
        Check(start<=UINT64_MAX-blocks,"Invalid APFS file extent range");
        ++boundaries[start];
        --boundaries[start+blocks];
    }
    for(const auto& run:reused){
        ++boundaries[run.physical];
        --boundaries[run.physical+run.blocks];
    }
    std::map<uint64_t,uint32_t> counts;
    int64_t active=0;
    for(const auto& [address,delta]:boundaries){
        if(!delta)continue;
        active+=delta;
        Check(active>=0&&active<=INT32_MAX,"Invalid APFS file extent reference count");
        counts.emplace(address,uint32_t(active));
    }
    return counts;
}

std::vector<OwnedRecord> CloneDataExtents(std::vector<OwnedRecord>& catalog,
                                         const std::vector<OwnedRecord>& physicalExtents,
                                         uint64_t source,uint64_t destination,uint64_t size)
{
    const auto inodeKey=(uint64_t(3)<<60)|source;
    const auto inode=std::find_if(catalog.begin(),catalog.end(),[&](const auto& record){
        return record.key.size()==8&&Le64(record.key.data())==inodeKey;
    });
    Check(inode!=catalog.end()&&inode->value.size()>=92,"Missing APFS clone source inode");
    auto& value=inode->value;
    Check((Le16(value.data()+80)&0170000)==0100000,"The APFS clone source is not a regular file");
    const auto stream=Le64(value.data()+8);
    std::vector<OwnedRecord> result{StreamReference(destination)};
    uint64_t logical=0;
    for(const auto& record:catalog){
        if(Le64(record.key.data())!=((uint64_t(8)<<60)|stream))continue;
        Check(record.key.size()==16&&record.value.size()==24,"Invalid APFS clone source extent");
        const auto start=Le64(record.value.data()+8),bytes=Le64(record.value.data());
        if(!start||!bytes||(bytes%Block)||(bytes>>56)||Le64(record.value.data()+16))
            throw CloneUnavailable("APFS clone source has unsupported extents");
        Check(Le64(record.key.data()+8)==logical&&bytes<=UINT64_MAX-logical,"Invalid APFS clone source stream");
        const auto end=start+bytes/Block;
        auto base=std::lower_bound(physicalExtents.begin(),physicalExtents.end(),(uint64_t(2)<<60)|start,
            [](const auto& extent,uint64_t key){return Le64(extent.key.data())<key;});
        auto covered=start;
        for(;base!=physicalExtents.end()&&(Le64(base->key.data())&IdMask)==covered&&covered<end;++base)
            covered+=Le64(base->value.data())&IdMask;
        if(covered!=end)throw CloneUnavailable("APFS clone source does not cover whole physical extents");
        result.push_back(record);
        Set(result.back().key,0,(uint64_t(8)<<60)|destination,8);
        logical+=bytes;
    }
    if(!size||size>UINT64_MAX-(Block-1)||logical!=DivideUp(size,Block)*Block)
        throw CloneUnavailable("APFS clone source has an unsupported stream size");
    constexpr uint64_t wasEverCloned=0x400;
    Set(value,48,Le64(value.data()+48)|wasEverCloned,8);
    return result;
}

uint32_t CatalogReferences(const std::map<uint64_t,uint32_t>& counts,uint64_t start,uint64_t blocks)
{
    const auto next=counts.upper_bound(start);
    Check(next==counts.end()||next->first>=start+blocks,"Unsupported partial APFS physical extent change");
    return next==counts.begin()?0:std::prev(next)->second;
}

SnapshotAllocations ReadSnapshotAllocations(Source& source,const Container& container,const Bytes& volume,
                                             const WritableMap& map,const CatalogRecords& catalog,CatalogRecords& extentTree)
{
    const auto snapshots=Le64(volume.data()+152);
    const auto count=Le64(volume.data()+216);
    const Node tree(ReadObject(source,container,snapshots,snapshots,container.transaction,2,16,true),true,{});
    SnapshotAllocations result;
    if(!count){
        Check(!tree.Level()&&!tree.Count(),"APFS volume snapshot count disagrees with the tree");
        return result;
    }
    Check(count==1&&container.volumes.size()==1&&!tree.Level()&&tree.Count()==2,
          "APFS writes require one supported data-bearing snapshot");
    const auto record=tree.At(0);
    Check(record.key.size==8&&record.value.size>=51&&Le64(record.key.data)>>60==1&&
          !Le32(record.value.data+44)&&Le16(record.value.data+48)==record.value.size-50,
          "Unsupported APFS snapshot metadata");
    const auto xid=Le64(record.key.data)&IdMask;
    const auto* metadata=record.value.data;
    const auto extents=Le64(metadata),super=Le64(metadata+8);
    const auto name=tree.At(1);
    Check(xid&&xid<=container.transaction&&name.key.size>=11&&Le64(name.key.data)>>60==11&&
          name.value.size==8&&Le64(name.value.data)==xid,"Invalid APFS snapshot name index");
    const auto historical=ReadObject(source,container,super,super,xid,13,0,true);
    Check(!memcmp(historical.data()+32,"APSB",4)&&Le64(historical.data()+16)==xid&&
          !memcmp(historical.data()+240,volume.data()+240,16),"Invalid APFS historical volume");
    const auto snapMap=Le64(map.header.data()+56);
    const Node snapIndex(ReadObject(source,container,snapMap,snapMap,container.transaction,2,19,true),true,{});
    Check(!snapIndex.Level()&&snapIndex.Count()==1&&snapIndex.At(0).key.size==8&&
          Le64(snapIndex.At(0).key.data)==xid&&snapIndex.At(0).value.size==16&&
          !Le32(snapIndex.At(0).value.data),"Unsupported APFS object-map snapshot index");

    const auto oldMap=MapAt(map,xid);
    auto old=container;
    old.transaction=xid;
    const auto prior=ReadWritableCatalog(source,old,oldMap,Le64(historical.data()+136),true);
    result.catalogBlocks.insert(prior.blocks.begin(),prior.blocks.end());
    const std::set<uint64_t> liveCatalog(catalog.blocks.begin(),catalog.blocks.end());
    for(const auto& entry:oldMap.mappings)
        if(result.catalogBlocks.count(entry.block))result.mappings.push_back(entry);
    Check(result.mappings.size()==prior.blocks.size(),"Incomplete APFS snapshot catalog map");
    auto addProtected=[&](uint64_t block,uint64_t size=1){result.protectedRuns.push_back({block,size});};
    const auto extendedMetadata=Le64(volume.data()+1000);
    Check(!Le64(historical.data()+1000),"Unsupported historical APFS snapshot extension");
    const auto liveMap=MapAt(map,container.transaction);
    uint64_t extensionBlock=0;
    if(extendedMetadata){
        const auto entry=std::find_if(liveMap.mappings.begin(),liveMap.mappings.end(),
            [&](const auto& mapping){return mapping.oid==extendedMetadata;});
        Check(entry!=liveMap.mappings.end(),"Missing APFS snapshot extension mapping");
        const auto extension=ReadObject(source,container,entry->block,entry->oid,container.transaction,29);
        Check(Le32(extension.data()+32)==1&&!Le32(extension.data()+36)&&Le64(extension.data()+40)==xid,
              "Unsupported APFS snapshot extension");
        extensionBlock=entry->block;
        result.mappings.push_back(*entry);
        addProtected(extensionBlock);
    }
    for(const auto& entry:map.mappings)
        Check(!entry.block||entry.block==extensionBlock||liveCatalog.count(entry.block)||result.catalogBlocks.count(entry.block),
              "Unsupported APFS historical object map entry");

    addProtected(super);
    result.historicalSuper=super;
    addProtected(snapMap);
    for(const auto address:prior.blocks)addProtected(address);
    const auto priorExtents=ReadExtentTree(source,old,extents);
    for(const auto address:priorExtents.blocks)addProtected(address);
    std::map<uint64_t,OwnedRecord> priorData;
    for(const auto& entry:priorExtents.records)
        priorData.emplace(Le64(entry.key.data())&IdMask,entry);
    ValidateSharedFileExtents(prior,priorExtents.records);
    std::map<uint64_t,int64_t> adjustments;
    std::vector<OwnedRecord> liveData;
    for(auto& entry:extentTree.records){
        if(Le64(entry.value.data())>>60==1){
            liveData.push_back(std::move(entry));
            continue;
        }
        const auto address=Le64(entry.key.data())&IdMask;
        const auto prior=priorData.find(address);
        const auto encoded=Le32(entry.value.data()+16);
        const int64_t adjustment=encoded<=INT32_MAX?int64_t(encoded):int64_t(encoded)-0x100000000LL;
        Check(prior!=priorData.end()&&
              (Le64(entry.value.data())&IdMask)==(Le64(prior->second.value.data())&IdMask),
              "Unsupported partial APFS snapshot extent update");
        adjustments.emplace(address,adjustment);
    }
    for(auto& [address,entry]:priorData){
        result.historicalData.push_back(entry);
        const auto remaining=int64_t(Le32(entry.value.data()+16))+adjustments[address];
        Check(remaining>=0&&remaining<=INT32_MAX,"Invalid APFS snapshot extent reference count");
        if(!remaining)addProtected(address,Le64(entry.value.data())&IdMask);
        else{
            result.sharedData.insert(address);
            Set(entry.value,16,remaining,4);
            liveData.push_back(std::move(entry));
        }
    }
    std::sort(liveData.begin(),liveData.end(),[](const auto& a,const auto& b){
        return Le64(a.key.data())<Le64(b.key.data());
    });
    ValidateSharedFileExtents(catalog,liveData);
    extentTree.records=std::move(liveData);
    extentTree.verifiedPhysicalReferences=true;
    std::sort(result.protectedRuns.begin(),result.protectedRuns.end(),[](const auto& a,const auto& b){
        return a.block<b.block;
    });
    uint64_t end=0;
    for(const auto& run:result.protectedRuns){
        Check(run.block>=end&&Range(run.block,run.count,container.blockCount),"Overlapping APFS snapshot blocks");
        end=run.block+run.count;
    }
    return result;
}

struct VolumeSpace {
    uint64_t reserve,quota,used;
};

VolumeSpace ReadVolumeSpace(const Bytes& volume,uint64_t blockCount)
{
    const VolumeSpace limits{Le64(volume.data()+72),Le64(volume.data()+80),Le64(volume.data()+88)};
    Check(limits.reserve<=blockCount&&limits.quota<=blockCount&&limits.used<=blockCount&&
          (!limits.quota||limits.reserve<=limits.quota),"Invalid APFS volume space limits");
    return limits;
}

struct SiblingAllocation {
    std::vector<DataRun> runs;
    uint64_t totalReserve=0,allocatedReserve=0;
};

SiblingAllocation ReadSiblingAllocation(Source& source,const Container& container,const WritableMap& containerMap,
                                        uint64_t selected,const Bytes& selectedVolume)
{
    SiblingAllocation siblings;
    auto& runs=siblings.runs;
    const auto block=[&](uint64_t address){runs.push_back({address,1});};
    for(const auto& mapping:containerMap.mappings){
        if(mapping.oid==selected)continue;
        block(mapping.block);
        const auto volume=ReadObject(source,container,mapping.block,mapping.oid,container.transaction,13);
        auto sibling=container;
        sibling.writeCipher.reset();
        const auto cipher=container.siblingCiphers.find(mapping.oid);
        if(cipher!=container.siblingCiphers.end())sibling.writeCipher=cipher->second;
        CheckExternalVolume(volume,container.volumes.size()==2,false,bool(sibling.writeCipher));
        if(InVolumeGroup(volume)||InVolumeGroup(selectedVolume)){
            Check(InVolumeGroup(volume)&&InVolumeGroup(selectedVolume)&&
                  Le16(volume.data()+VolumeRoleOffset)!=Le16(selectedVolume.data()+VolumeRoleOffset)&&
                  std::equal(volume.begin()+VolumeGroupOffset,volume.begin()+VolumeGroupOffset+16,
                             selectedVolume.begin()+VolumeGroupOffset),
                  "Unsupported APFS volume group membership");
        }
        const auto limits=ReadVolumeSpace(volume,container.blockCount);
        Check(limits.reserve<=container.blockCount-siblings.totalReserve,"Invalid APFS shared volume reserves");
        siblings.totalReserve+=limits.reserve;
        siblings.allocatedReserve+=(std::min)(limits.reserve,limits.used);
        const auto map=ReadWritableMap(source,sibling,Le64(volume.data()+128));
        block(map.headerBlock);
        block(map.rootBlock);
        for(auto entry:map.leaves)block(entry);
        for(const auto& entry:map.mappings)block(entry.block);
        const auto catalog=ReadWritableCatalog(source,sibling,map,Le64(volume.data()+136));
        Check(catalog.blocks.size()==map.mappings.size(),"Unsupported APFS sibling catalog mappings");
        const auto snapshots=Le64(volume.data()+152);
        const Node snapshotTree(ReadObject(source,container,snapshots,snapshots,container.transaction,2,16,true),true,{});
        Check(!snapshotTree.Level()&&!snapshotTree.Count(),"APFS sibling volume has snapshots");
        block(snapshots);
        const auto extents=ReadExtentTree(source,container,Le64(volume.data()+144));
        for(auto entry:extents.blocks)block(entry);
        std::vector<DataRun> data;
        for(const auto& extent:extents.records)
            data.push_back({Le64(extent.key.data())&IdMask,Le64(extent.value.data())&IdMask});
        for(const auto& record:catalog.records){
            if(Le64(record.key.data())>>60!=8)continue;
            Check(record.key.size()==16&&record.value.size()==24,
                  "Invalid APFS sibling file extent");
            const auto encoded=Le64(record.value.data());
            Check(!(encoded>>56)||(sibling.writeCipher&&encoded>>56==1),"Unsupported APFS sibling file extent flags");
            const auto size=encoded&((uint64_t(1)<<56)-1),start=Le64(record.value.data()+8);
            Check(size&&!(size%Block),"Unsupported APFS sibling file extent");
            if(!start)continue; // Sparse runs own no physical blocks.
            const auto next=std::upper_bound(data.begin(),data.end(),start,
                [](uint64_t address,const DataRun& run){return address<run.block;});
            Check(next!=data.begin()&&Range(start-std::prev(next)->block,size/Block,std::prev(next)->count),
                  "APFS sibling file extent is missing from the extent references");
        }
        runs.insert(runs.end(),data.begin(),data.end());
    }
    std::sort(runs.begin(),runs.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    uint64_t end=0;
    for(const auto& run:runs){
        Check(run.count&&run.block>=end&&Range(run.block,run.count,container.blockCount),
              "Invalid or overlapping APFS sibling volume allocation");
        end=run.block+run.count;
    }
    return siblings;
}

void CheckNativeVolume(const Bytes& volume)
{
    const auto flags=Le64(volume.data()+56);
    Check(!memcmp(volume.data()+32,"APSB",4)&&!memcmp(volume.data()+272,"HFVExplorer",12)&&
          (flags==1||flags==8)&&Le64(volume.data()+264)==1&&Le32(volume.data()+116)==2&&
          !Le64(volume.data()+216),
          "APFS catalog writes currently require an unencrypted volume formatted by HFVExplorer");
}
}

std::vector<ReferenceSpan> SplitExtentReferences(const std::map<uint64_t,uint32_t>& counts,
                                                  uint64_t start,uint64_t blocks)
{
    Check(blocks&&blocks<=UINT64_MAX-start,"Invalid APFS physical extent range");
    const auto end=start+blocks;
    auto next=counts.upper_bound(start);
    auto references=next==counts.begin()?0:std::prev(next)->second;
    std::vector<ReferenceSpan> spans;
    for(auto at=start;at<end;){
        const auto stop=next==counts.end()?end:(std::min)(end,next->first);
        if(!spans.empty()&&spans.back().references==references)spans.back().blocks+=stop-at;
        else spans.push_back({at,stop-at,references});
        at=stop;
        if(at<end)references=(next++)->second;
    }
    return spans;
}

uint64_t DataStreamWrite::AllocatedBlocks() const
{
    const auto blocks=DivideUp(size,Block);
    uint64_t allocated=blocks,end=0;
    for(const auto& extent:existingExtents){
        Check(extent.count&&extent.block>=end&&extent.block<=blocks&&extent.count<=blocks-extent.block&&
              extent.count<=UINT64_MAX-extent.physical,"Invalid APFS existing stream extent");
        end=extent.block+extent.count;
        allocated-=extent.count;
    }
    return allocated;
}

void AssignDataRuns(std::vector<DataStreamWrite>& streams,const std::vector<DataRun>& runs)
{
    size_t index=0;
    uint64_t consumed=0;
    for(auto& stream:streams){
        stream.runs.clear();
        const auto allocation=stream.AllocatedBlocks();
        const auto blocks=DivideUp(stream.size,Block);
        uint64_t logical=0,assigned=0;
        size_t extentIndex=0;
        while(logical<blocks){
            if(extentIndex<stream.existingExtents.size()&&stream.existingExtents[extentIndex].block==logical){
                const auto& extent=stream.existingExtents[extentIndex++];
                stream.runs.push_back({extent.physical,extent.count,extent.physical!=0});
                logical+=extent.count;
                continue;
            }
            const auto stop=extentIndex<stream.existingExtents.size()?stream.existingExtents[extentIndex].block:blocks;
            Check(index<runs.size()&&runs[index].block&&!runs[index].reused&&consumed<runs[index].count,"APFS data allocation is incomplete");
            const auto& run=runs[index];
            const auto count=(std::min)(stop-logical,run.count-consumed);
            Check(consumed<=UINT64_MAX-run.block,"APFS data allocation overflows");
            stream.runs.push_back({run.block+consumed,count});
            logical+=count;
            assigned+=count;
            consumed+=count;
            if(consumed==run.count){
                ++index;
                consumed=0;
            }
        }
        Check(assigned==allocation,"APFS sparse stream allocation has inconsistent size");
    }
    Check(index==runs.size(),"APFS data allocation has unused blocks");
}

void ValidateWritableAttributes(const HfsPlus::Attributes& attributes)
{
    for(const auto& [wide,value]:attributes){
        const auto name=EncodeAttributeName(wide);
        Check(OrdinaryAttribute(name),"Special APFS attributes require dedicated write support");
        if(wide==SecurityAttribute)HfsPlus::ValidateAttributes({{wide,value}});
    }
}

uint64_t CatalogTimestamp(uint64_t secondsSince1904)
{
    constexpr int64_t UnixEpoch=2082844800,Second=1000000000;
    Check(secondsSince1904<=UnixEpoch+INT64_MAX/Second,"The timestamp is outside the APFS writable range");
    return uint64_t((int64_t(secondsSince1904)-UnixEpoch)*Second);
}

CatalogLayoutCounts InspectCatalogLayout(Source& source,const Container& container)
{
    const auto layout=ReadCatalogVolume(source,container);
    const auto& volume=layout.volume;
    const auto volumeMap=ReadWritableMap(source,container,Le64(volume.data()+128));
    const auto catalog=ReadWritableCatalog(source,container,volumeMap,Le64(volume.data()+136));
    const auto extents=ReadExtentTree(source,container,Le64(volume.data()+144));
    return {catalog.records.size(),extents.records.size()};
}

namespace {
struct WriteAllocationState {
    SiblingAllocation siblings;
    WritableMap volumeMap;
    CatalogRecords catalog,extents;
    SnapshotAllocations snapshot;
    std::vector<DataRun> protectedRuns;
};

WriteAllocationState ReadWriteAllocationState(Source& source,const Container& container,
                                               const WritableMap& containerMap,uint64_t oid,const Bytes& volume)
{
    const bool historical=Le64(volume.data()+216)!=0;
    auto siblings=ReadSiblingAllocation(source,container,containerMap,oid,volume);
    auto volumeMap=ReadWritableMap(source,container,Le64(volume.data()+128),historical);
    auto catalog=ReadWritableCatalog(source,container,
        historical?MapAt(volumeMap,container.transaction):volumeMap,Le64(volume.data()+136),historical);
    auto extents=ReadExtentTree(source,container,Le64(volume.data()+144),historical);
    extents.cipher=container.writeCipher;
    auto snapshot=ReadSnapshotAllocations(source,container,volume,volumeMap,catalog,extents);
    if(!historical)ValidateSharedFileExtents(catalog,extents.records,bool(extents.cipher));
    auto protectedRuns=historical?snapshot.protectedRuns:siblings.runs;
    ProtectKeybags(container,protectedRuns);
    return {std::move(siblings),std::move(volumeMap),std::move(catalog),std::move(extents),
            std::move(snapshot),std::move(protectedRuns)};
}
}

uint64_t VerifyFreeQueueAllocation(Source& source,const Container& container,uint64_t transaction,
                                   Bytes& superblock,const CheckpointObjectLookup& ephemeral,uint64_t selected)
{
    const auto [containerMap,address,volume]=ReadCatalogVolume(source,container,selected);
    CheckExternalVolume(volume,container.volumes.size()==2,container.volumes.size()==1,bool(container.writeCipher));
    const auto [siblings,volumeMap,catalog,extents,snapshot,protectedRuns]=ReadWriteAllocationState(
        source,container,containerMap,selected?selected:container.volumes.front(),volume);
    const auto snapshots=Le64(volume.data()+152);
    std::vector<uint64_t> live{containerMap.headerBlock,containerMap.rootBlock,address,
                               volumeMap.headerBlock,volumeMap.rootBlock};
    live.insert(live.end(),containerMap.leaves.begin(),containerMap.leaves.end());
    live.insert(live.end(),volumeMap.leaves.begin(),volumeMap.leaves.end());
    for(auto address:catalog.blocks)
        if(!snapshot.catalogBlocks.count(address))live.push_back(address);
    std::vector<FileRun> files;
    for(const auto& extent:extents.records)
        files.push_back({Le64(extent.value.data()+8),Le64(extent.key.data())&IdMask,
                         Le64(extent.value.data())&IdMask});
    auto& space=ephemeral(Le64(superblock.data()+152));
    Writes discarded;
    std::vector<DataRun> data;
    const auto allocated=AllocateWriteBlocks(discarded,source,container,space,superblock,ephemeral,
                                  live,extents.blocks,snapshots,files,{},1,data,transaction,[]{return 1;},protectedRuns);
    Check(allocated.size()==1&&data.size()==1&&data.front().count==1&&allocated.front()!=data.front().block,
          "APFS allocation preflight did not reserve distinct data and metadata blocks");
    return Le64(space.data()+72)*container.blockSize;
}

std::vector<HfsPlus::SharedBlockRange> SnapshotCloneReferenceRanges(Source& source,const Container& container,
                                                                const std::vector<HfsPlus::SharedBlockRange>& ranges)
{
    const auto [map,address,volume]=ReadCatalogVolume(source,container,container.writeVolume);
    const auto [siblings,volumeMap,catalog,extents,snapshot,protectedRuns]=ReadWriteAllocationState(
        source,container,map,container.writeVolume?container.writeVolume:container.volumes.front(),volume);
    std::vector<HfsPlus::SharedBlockRange> whole;
    for(const auto& range:ranges){
        const auto end=range.physical+range.blocks;
        auto next=std::upper_bound(extents.records.begin(),extents.records.end(),range.physical,
            [](uint64_t start,const auto& record){return start<(Le64(record.key.data())&IdMask);});
        if(next!=extents.records.begin())--next;
        for(;next!=extents.records.end();++next){
            const auto start=Le64(next->key.data())&IdMask,blocks=Le64(next->value.data())&IdMask;
            if(start>=end)break;
            const auto first=(std::max)(start,range.physical),last=(std::min)(start+blocks,end);
            if(first>=last)continue;
            // Snapshot reference adjustments describe the historical record as a whole.
            if(snapshot.sharedData.count(start)&&(first!=start||last!=start+blocks))continue;
            whole.push_back({range.offset+(first-range.physical)*Block,first,last-first,range.partner});
        }
    }
    return whole;
}

void ValidateWritableLayout(Source& source,const Container& container)
{
    const auto layout=ReadCatalogVolume(source,container);
    const auto& volume=layout.volume;
    CheckNativeVolume(volume);
    const auto extents=Le64(volume.data()+144),snapshots=Le64(volume.data()+152);
    ReadExtentTree(source,container,extents);
    const Node snapshotTree(ReadObject(source,container,snapshots,snapshots,container.transaction,2,16,true),true,{});
    Check(!snapshotTree.Level()&&!snapshotTree.Count(),"Unsupported APFS extent or snapshot tree");
    const auto volumeMap=ReadWritableMap(source,container,Le64(volume.data()+128));
    const auto catalog=ReadWritableCatalog(source,container,volumeMap,Le64(volume.data()+136));
    constexpr uint64_t cloneFlags=0x10|0x400;
    for(const auto& record:catalog.records){
        constexpr std::string_view compressionKey=CompressionAttribute;
        if(record.key.size()==11+compressionKey.size()&&(Le64(record.key.data())>>60)==4&&
           !memcmp(record.key.data()+10,compressionKey.data(),compressionKey.size())&&
           record.value.size()>=4&&Le16(record.value.data())==1)
            throw HfsPlus::Error("APFS streamed compression requires the compatibility writer");
        if(record.key.size()<8||(Le64(record.key.data())>>60)!=3)continue;
        Check(record.value.size()>=56,"Truncated APFS inode");
        Check(!(Le64(record.value.data()+48)&cloneFlags),
              "APFS clones require the shared-extent writer");
    }
}

namespace {
CatalogWritePlan PrepareVolumeRename(Source& source,const Container& current,uint64_t transaction,
    Bytes& superblock,const CheckpointObjectLookup& ephemeral,const CatalogVolumeRename& request,uint64_t selected)
{
    auto [containerMap,volumeAddress,volume]=ReadCatalogVolume(source,current,selected);
    CheckExternalVolume(volume,current.volumes.size()==2,current.volumes.size()==1,bool(current.writeCipher));
    const auto oid=selected?selected:current.volumes.front();
    const auto name=EncodeCatalogName(request.name);
    auto [siblings,volumeMap,catalog,extents,snapshot,protectedRuns]=ReadWriteAllocationState(
        source,current,containerMap,oid,volume);
    std::vector<FileRun> files;
    for(const auto& extent:extents.records)
        files.push_back({Le64(extent.value.data()+8),Le64(extent.key.data())&IdMask,Le64(extent.value.data())&IdMask});
    for(auto block:catalog.blocks)
        if(!snapshot.catalogBlocks.count(block))protectedRuns.push_back({block,1});
    for(auto block:volumeMap.leaves)protectedRuns.push_back({block,1});
    protectedRuns.push_back({volumeMap.headerBlock,1});
    protectedRuns.push_back({volumeMap.rootBlock,1});
    std::sort(protectedRuns.begin(),protectedRuns.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    CatalogWritePlan plan;
    std::vector<DataRun> unused;
    auto& space=ephemeral(Le64(superblock.data()+152));
    const auto blocks=AllocateWriteBlocks(plan.metadata,source,current,space,superblock,ephemeral,
        {volumeAddress,containerMap.headerBlock,containerMap.rootBlock},extents.blocks,Le64(volume.data()+152),
        files,{},0,unused,transaction,[]{return 3;},protectedRuns);
    const auto limits=ReadVolumeSpace(volume,current.blockCount);
    Check(limits.reserve<=current.blockCount-siblings.totalReserve,"Invalid APFS reserve total");
    const auto total=siblings.totalReserve+limits.reserve;
    const auto allocated=siblings.allocatedReserve+(std::min)(limits.reserve,limits.used);
    Check(Le64(space.data()+SpaceReserveCount)==total&&Le64(space.data()+SpaceReserveAllocated)==allocated&&
          Le64(space.data()+72)>=total-allocated,"APFS rename would consume reserved space");
    std::fill_n(volume.begin()+704,256,0);
    std::copy(name.begin(),name.end(),volume.begin()+704);
    StageObject(plan.metadata,blocks[0],std::move(volume),oid,transaction,13);
    for(auto& mapping:containerMap.mappings)
        if(mapping.oid==oid)mapping={oid,blocks[0]};
    StageMap(plan.metadata,containerMap,blocks[1],blocks[2],containerMap.mappings,transaction,blocks,3);
    Set(superblock,160,blocks[1],8);
    return plan;
}

CatalogWritePlan PrepareSnapshotMutation(Source& source,const Container& current,uint64_t transaction,
    Bytes& superblock,const CheckpointObjectLookup& ephemeral,const CatalogSnapshot& request)
{
    Check(current.volumes.size()==1,"APFS snapshot changes require a single-volume image");
    auto [containerMap,volumeAddress,volume]=ReadCatalogVolume(source,current);
    CheckExternalVolume(volume,false,true);
    const bool deleting=!request.create;
    Check(Le64(volume.data()+216)==uint64_t(deleting),
          deleting?"The APFS image has no snapshot to delete":"This APFS image already has a snapshot");
    const auto name=EncodeCatalogName(request.name);
    const auto volumeId=current.volumes.front();
    const auto snapshotTree=Le64(volume.data()+152),extentRoot=Le64(volume.data()+144);
    auto [siblings,map,catalog,extents,snapshot,snapshotOnlyRuns]=ReadWriteAllocationState(
        source,current,containerMap,volumeId,volume);
    if(deleting){
        const Node tree(ReadObject(source,current,snapshotTree,snapshotTree,current.transaction,2,16,true),true,{});
        const auto entry=tree.At(0).value;
        Check(entry.size==51+name.size()&&!memcmp(entry.data+50,name.c_str(),name.size()+1),
              "The selected APFS snapshot changed");
    }
    // Collapse the snapshot deltas into the live physical reference tree.
    std::vector<FileRun> live;
    for(const auto& extent:extents.records){
        Check(deleting||Le32(extent.value.data()+16)==1,"Creating a snapshot requires exclusive live file extents");
        live.push_back({Le64(extent.value.data()+8),Le64(extent.key.data())&IdMask,Le64(extent.value.data())&IdMask});
    }
    Check(deleting||!Le64(volume.data()+1000),"APFS snapshot creation requires no existing snapshot extension");
    const std::set<uint64_t> liveCatalog(catalog.blocks.begin(),catalog.blocks.end());

    snapshotOnlyRuns.erase(std::remove_if(snapshotOnlyRuns.begin(),snapshotOnlyRuns.end(),[&](const auto& run){
        return run.count==1&&snapshot.catalogBlocks.count(run.block)&&liveCatalog.count(run.block);
    }),snapshotOnlyRuns.end());
    auto protectedRuns=snapshotOnlyRuns;
    for(auto block:catalog.blocks)protectedRuns.push_back({block,1});
    std::sort(protectedRuns.begin(),protectedRuns.end(),[](const auto& a,const auto& b){return a.block<b.block;});
    std::vector<uint64_t> retired{containerMap.headerBlock,containerMap.rootBlock,volumeAddress,
                                  map.headerBlock,map.rootBlock,snapshotTree};
    retired.insert(retired.end(),containerMap.leaves.begin(),containerMap.leaves.end());
    retired.insert(retired.end(),map.leaves.begin(),map.leaves.end());
    if(deleting)retired.insert(retired.end(),extents.blocks.begin(),extents.blocks.end());

    auto extentLeaves=PartitionCatalog(deleting?std::move(extents.records):std::vector<OwnedRecord>{});
    const auto extentChildren=extentLeaves.size()>1?extentLeaves.size():0;
    auto mappings=deleting?MapAt(map,current.transaction).mappings:map.mappings;
    mappings.erase(std::remove_if(mappings.begin(),mappings.end(),[&](const auto& mapping){
        return !liveCatalog.count(mapping.block);
    }),mappings.end());
    Check(mappings.size()==catalog.blocks.size(),"Incomplete APFS live catalog map");
    for(auto& mapping:mappings)mapping.flags=0;
    const auto mapChildren=MapLeaves(mappings.size());
    const size_t firstChild=deleting?7:9;
    auto& space=ephemeral(Le64(superblock.data()+152));
    CatalogWritePlan plan;
    std::vector<DataRun> unused;
    const auto blocks=AllocateWriteBlocks(plan.metadata,source,current,space,superblock,ephemeral,retired,extents.blocks,
        snapshotTree,live,snapshotOnlyRuns,0,unused,transaction,
        [&]{return firstChild+extentChildren+mapChildren;},protectedRuns);
    StageExtentTree(plan.metadata,extentLeaves,blocks,firstChild,transaction);
    FILETIME clock{};
    GetSystemTimeAsFileTime(&clock);
    const auto time=(((uint64_t(clock.dwHighDateTime)<<32)|clock.dwLowDateTime)-116444736000000000ULL)*100;
    std::vector<OwnedRecord> snapshotRecords;
    if(!deleting){
        const auto next=Le64(volume.data()+176);
        Check(next>=16&&next<IdMask&&transaction<=IdMask,"APFS snapshot object IDs exhausted");
        OwnedRecord meta{Bytes(8),Bytes(51+name.size())},label{Bytes(11+name.size()),Bytes(8)};
        Set(meta.key,0,(uint64_t(1)<<60)|transaction,8);
        Set(meta.value,0,extentRoot,8);
        Set(meta.value,8,blocks[7],8);
        Set(meta.value,16,time,8);
        Set(meta.value,24,time,8);
        Set(meta.value,32,next,8);
        Set(meta.value,40,Physical|2,4);
        Set(meta.value,48,name.size()+1,2);
        std::copy(name.begin(),name.end(),meta.value.begin()+50);
        Set(label.key,0,(uint64_t(11)<<60)|IdMask,8);
        Set(label.key,8,name.size()+1,2);
        std::copy(name.begin(),name.end(),label.key.begin()+10);
        Set(label.value,0,transaction,8);
        snapshotRecords={std::move(meta),std::move(label)};
        auto historical=volume;
        for(auto field:{128,144,152})Set(historical,field,0,8);
        StageObject(plan.metadata,blocks[7],std::move(historical),blocks[7],transaction,Physical|13);
        OwnedRecord index{Bytes(8),Bytes(16)};
        Set(index.key,0,transaction,8);
        StageObject(plan.metadata,blocks[8],EncodeLeaf({index},0x10,8,16),blocks[8],transaction,Physical|2,19);
        Set(volume,176,next+1,8);
    }
    StageObject(plan.metadata,blocks[0],EncodeLeaf(snapshotRecords,0x50),blocks[0],transaction,Physical|2,16);
    Set(map.header,36,!deleting,4);
    Set(map.header,56,deleting?0:blocks[8],8);
    Set(map.header,64,deleting?0:transaction,8);
    StageMap(plan.metadata,map,blocks[1],blocks[2],mappings,transaction,blocks,firstChild+extentChildren);
    const auto limits=ReadVolumeSpace(volume,current.blockCount);
    uint64_t dropped=map.leaves.size(),added=mapChildren;
    if(deleting){
        dropped+=extents.blocks.size()-1;
        for(const auto& run:snapshotOnlyRuns)dropped+=run.count;
        Check(snapshot.historicalSuper&&dropped,"Invalid APFS snapshot allocation history");
        --dropped; // The physical snapshot superblock is excluded from the volume allocation count.
        added+=extentChildren;
    }else added+=2; // New live extent tree and object-map snapshot index.
    Check(dropped<=limits.used&&added<=current.blockCount-(limits.used-dropped),"Invalid APFS snapshot allocation count");
    const auto used=limits.used-dropped+added;
    Check(!limits.quota||used<=limits.quota,"The APFS volume quota would be exceeded");
    Check(Le64(space.data()+SpaceReserveCount)==limits.reserve&&
          Le64(space.data()+SpaceReserveAllocated)==(std::min)(limits.reserve,limits.used),"Invalid APFS snapshot reserve");
    const auto reserveAllocated=(std::min)(limits.reserve,used);
    Check(Le64(space.data()+72)>=limits.reserve-reserveAllocated,"Insufficient space for the APFS reserve");
    Set(space,SpaceReserveAllocated,reserveAllocated,8);
    Set(volume,88,used,8);
    Set(volume,128,blocks[1],8);
    Set(volume,144,blocks[6],8);
    Set(volume,152,blocks[0],8);
    Set(volume,216,!deleting,8);
    if(deleting)Set(volume,1000,0,8);
    Set(volume,256,time,8);
    Check(Le64(volume.data()+224)<=UINT64_MAX-added-3&&Le64(volume.data()+232)<=UINT64_MAX-dropped-3,
          "APFS snapshot allocation history overflow");
    Set(volume,224,Le64(volume.data()+224)+added+3,8);
    Set(volume,232,Le64(volume.data()+232)+dropped+3,8);
    StageObject(plan.metadata,blocks[3],std::move(volume),volumeId,transaction,13);
    for(auto& mapping:containerMap.mappings)
        if(mapping.oid==volumeId)mapping={volumeId,blocks[3]};
    StageMap(plan.metadata,containerMap,blocks[4],blocks[5],containerMap.mappings,transaction,blocks,0);
    Set(superblock,160,blocks[4],8);
    return plan;
}
}
CatalogWritePlan PrepareCatalogMutation(
    Source& source,const Container& current,uint64_t transaction,Bytes& superblock,
    const CheckpointObjectLookup& ephemeral,const CatalogMutation& request,uint64_t selected)
{
    Check(current.blockSize==Block,"APFS catalog writes currently require 4096-byte blocks");
    if(const auto* snapshot=std::get_if<CatalogSnapshot>(&request)){
        Check(!selected||selected==current.volumes.front(),"The APFS snapshot volume changed");
        return PrepareSnapshotMutation(source,current,transaction,superblock,ephemeral,*snapshot);
    }
    if(const auto* rename=std::get_if<CatalogVolumeRename>(&request))
        return PrepareVolumeRename(source,current,transaction,superblock,ephemeral,*rename,selected);
    const auto* create=std::get_if<CatalogCreate>(&request);
    const auto* rename=std::get_if<CatalogRename>(&request);
    const auto* hardLink=std::get_if<CatalogHardLink>(&request);
    const auto* properties=std::get_if<ExternalCatalogEdit>(&request);
    const auto name=create?EncodeCatalogName(create->name):rename?EncodeCatalogName(rename->name):
                    hardLink?EncodeCatalogName(hardLink->name):properties?EncodeCatalogName(properties->name):std::string{};
    const auto* replace=std::get_if<CatalogReplaceData>(&request);
    const auto* linkReplacement=std::get_if<CatalogReplaceSymlink>(&request);
    const auto dates=replace?replace->dates:create?create->dates:linkReplacement?linkReplacement->dates:std::optional<FileDates>{};
    CatalogWritePlan plan;
    auto* input=replace?&replace->data:create?create->data:nullptr;
    const bool clone=create&&create->cloneSource;
    const auto dataSize=input?input->Size():clone?create->cloneSize:0;
    const auto addStream=[&](Source& source,uint64_t size,const std::vector<HfsPlus::SparseRange>& holes){
        auto& stream=plan.streams.emplace_back(DataStreamWrite{source,size});
        for(const auto& range:holes){
            Check(!(range.offset%Block)&&range.length&&Range(range.offset,range.length,size)&&
                  (!(range.length%Block)||range.offset+range.length==size),
                  "Invalid APFS sparse source range");
            stream.existingExtents.push_back({range.offset/Block,DivideUp(range.length,Block)});
        }
        return stream.AllocatedBlocks();
    };
    const auto dataHoles=input?input->KnownZeroRanges():std::vector<HfsPlus::SparseRange>{};
    const auto shared=input?input->VerifiedSharedBlocks():std::vector<HfsPlus::SharedBlockRange>{};
    const auto fileBlocks=[&]{
        if(!input||!dataSize)return uint64_t(0);
        const auto blocks=addStream(*input,dataSize,dataHoles);
        if(shared.empty())return blocks;
        auto& stream=plan.streams.front();
        for(const auto& range:shared){
            Check(!(range.offset%Block)&&range.blocks&&range.offset<=dataSize&&
                  range.blocks<=(dataSize-range.offset)/Block&&range.physical&&range.partner>=16&&
                  Range(range.physical,range.blocks,current.blockCount),"Invalid APFS reuse range");
            stream.existingExtents.push_back({range.offset/Block,range.blocks,range.physical});
        }
        std::sort(stream.existingExtents.begin(),stream.existingExtents.end(),
                  [](const auto& a,const auto& b){return a.block<b.block;});
        return stream.AllocatedBlocks();
    }();
    const auto* resourceEdit=std::get_if<CatalogResourceFork>(&request);
    auto* resourceInput=resourceEdit?&resourceEdit->resource:replace?replace->resource:create?create->resource:nullptr;
    std::optional<ResourceWrite> resource;
    if(resourceInput)resource=ResourceWrite{resourceInput,resourceInput->Size(),0};
    // Compressed files with a resource fork use a streamed attribute for that
    // fork, whether it holds compressed units or independent user data.
    const bool streamCompressedFileResource=(replace?replace->compression:create?create->compression:nullptr)&&
                                            resource&&resource->size;
    const auto resourceHoles=resourceInput?resourceInput->KnownZeroRanges():std::vector<HfsPlus::SparseRange>{};
    const bool hasSparseInput=!dataHoles.empty()||!resourceHoles.empty();
    const bool restoresSharing=!shared.empty();
    const bool streamedResource=resource&&(resource->size>=EmbeddedAttributeLimit||streamCompressedFileResource||!resourceHoles.empty());
    const auto resourceBlocks=streamedResource?addStream(*resourceInput,resource->size,resourceHoles):0;
    if(streamedResource)resource->allocatedBytes=resourceBlocks*Block;
    Check(fileBlocks<=current.blockCount&&resourceBlocks<=current.blockCount-fileBlocks,
          "APFS forks exceed the container size");
    auto dataBlocks=fileBlocks+resourceBlocks;
    auto [containerMap,volumeAddress,volume]=ReadCatalogVolume(source,current,selected);
    const auto oid=selected?selected:current.volumes.front();
    const auto flags=Le64(volume.data()+56);
    const bool fold=(flags&1)!=0;
    const auto fileCount=Le64(volume.data()+184);
    const auto directoryCount=Le64(volume.data()+192);
    const auto symlinkCount=Le64(volume.data()+200);
    const auto specialCount=Le64(volume.data()+208);
    const auto catalogOid=Le64(volume.data()+136);
    const auto snapshots=Le64(volume.data()+152);
    const auto* compression=create?create->compression:replace?replace->compression:nullptr;
    const bool streamCompression=compression&&compression->size()>WriterDisk::InlineCompressionCapacity();
    const bool externalCreate=create&&(create->external||streamCompression);
    const bool externalHardLink=hardLink&&hardLink->external;
    const bool externalReplace=replace&&(replace->external||streamCompression);
    const bool externalResource=resourceEdit&&resourceEdit->external;
    const auto* attributeEdit=std::get_if<CatalogAttributes>(&request);
    const bool externalAttributes=attributeEdit&&attributeEdit->external;
    const bool mergedFolderAttributes=properties&&properties->folderAttributes;
    const bool externalLinkReplace=linkReplacement&&linkReplacement->external;
    const auto* remove=std::get_if<CatalogDelete>(&request);
    const bool externalDelete=remove&&remove->external;
    const bool historical=Le64(volume.data()+216)!=0;
    Check(!restoresSharing||externalCreate,"APFS shared range restoration requires the compatibility writer");
    if(properties||externalCreate||externalReplace||externalResource||externalAttributes||externalDelete||externalLinkReplace||externalHardLink)
        CheckExternalVolume(volume,current.volumes.size()==2,current.volumes.size()==1,bool(current.writeCipher));
    else CheckNativeVolume(volume);
    auto [siblings,volumeMap,existing,extentTree,snapshot,protectedRuns]=ReadWriteAllocationState(
        source,current,containerMap,oid,volume);
    for(const auto& run:shared){
        uint64_t at=run.physical;
        const auto end=at+run.blocks;
        auto next=std::upper_bound(extentTree.records.begin(),extentTree.records.end(),at,
            [](uint64_t block,const auto& record){return block<(Le64(record.key.data())&IdMask);});
        Check(next!=extentTree.records.begin(),"APFS reuse range does not belong to this volume");
        for(--next;at<end&&next!=extentTree.records.end();++next){
            const auto start=Le64(next->key.data())&IdMask,length=Le64(next->value.data())&IdMask;
            if(start>at||start+length<=at)break;
            Check(!snapshot.sharedData.count(start)||(start==at&&length<=end-at),
                  "APFS snapshot reuse requires whole allocation records");
            at=(std::min)(end,start+length);
        }
        Check(at==end,"APFS reuse range does not belong to this volume");
    }
    const bool liveClones=!historical&&(clone||restoresSharing||std::any_of(existing.records.begin(),existing.records.end(),
        [](const auto& record){
            constexpr uint64_t cloneFlags=0x10|0x400;
            return record.key.size()==8&&Le64(record.key.data())>>60==3&&record.value.size()>=56&&
                   (Le64(record.value.data()+48)&cloneFlags);
        }));

    if(liveClones)extentTree.verifiedPhysicalReferences=true;
    const bool reconcileReferences=extentTree.verifiedPhysicalReferences;
    const auto nextId=Le64(volume.data()+176);
    Check(nextId>=16&&nextId<IdMask,"Invalid APFS next inode ID");
    auto nextAvailable=nextId+bool(create)+streamedResource+(hardLink?2:0);
    const auto idLimit=InVolumeGroup(volume)&&Le16(volume.data()+VolumeRoleOffset)==DataVolumeRole?UnifiedInodeMark:IdMask;
    Check(nextAvailable<idLimit,"APFS file-system object IDs are exhausted");
    if(streamedResource){
        resource->id=nextId+bool(create);
        plan.streams.back().id=resource->id;
    }
    const auto* attributes=mergedFolderAttributes?properties->folderAttributes:
                           attributeEdit?&attributeEdit->attributes:replace?replace->attributes:
                           create?create->attributes:linkReplacement?&linkReplacement->attributes:nullptr;
    AttributeStreams attributeIds;
    const auto addAttributeStream=[&](const std::u16string& name,const HfsPlus::AttributeData& value){
        const auto blocks=DivideUp(value.size(),Block);
        Check(blocks<=current.blockCount-dataBlocks,"APFS streams exceed the container size");
        Check(nextAvailable<IdMask-1,"APFS file-system object IDs are exhausted");
        attributeIds.emplace(name,nextAvailable);
        auto input=std::make_unique<AttributeInput>(value);
        plan.streams.push_back({*input,value.size(),{},nextAvailable++});
        plan.attributeInputs.push_back(std::move(input));
        dataBlocks+=blocks;
    };
    if(streamCompression)
        addAttributeStream(u"com.apple.decmpfs",*compression);
    if(attributes){
        ValidateWritableAttributes(*attributes);
        for(const auto& [name,value]:*attributes){
            if(value.size()<EmbeddedAttributeLimit)continue;
            addAttributeStream(name,value);
        }
    }
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    const auto time=(((uint64_t(fileTime.dwHighDateTime)<<32)|fileTime.dwLowDateTime)-116444736000000000ULL)*100;
    std::vector<FileRun> files;
    std::optional<CatalogKind> removedKind;
    size_t streamAt=0;
    const auto entryId=std::visit([nextId](const auto& edit){
        if constexpr(std::is_same_v<std::decay_t<decltype(edit)>,CatalogCreate>)return nextId;
        else if constexpr(std::is_same_v<std::decay_t<decltype(edit)>,ExternalCatalogEdit>||(std::is_same_v<std::decay_t<decltype(edit)>,CatalogSnapshot>||std::is_same_v<std::decay_t<decltype(edit)>,CatalogVolumeRename>))return uint64_t(0);
        else return edit.inode;
    },request);
    std::set<uint64_t> retiredStreams;
    std::set<uint64_t> externalRetired;
    uint64_t deletedFiles=0,deletedFolders=0,deletedSymlinks=0,deletedSpecial=0;
    std::vector<OwnedRecord> catalog;
    if(externalHardLink){
        catalog=std::move(existing.records);
        if(hardLink->replaced){
            Check(hardLink->replaced!=hardLink->inode,"APFS hard-link source and replacement are the same inode");
            const auto destinationKey=(uint64_t(9)<<60)|hardLink->parent;
            const auto destinationName=NormalizedName(name,fold);
            const auto matches=[&](const OwnedRecord& record){
                if(record.key.size()<14||Le64(record.key.data())!=destinationKey||
                   (record.value.size()!=18&&record.value.size()!=34)||Le64(record.value.data())!=hardLink->replaced||
                   record.key.back())return false;
                const auto kind=CatalogKind(Le16(record.value.data()+16));
                if(kind!=CatalogKind::RegularFile&&
                   ((kind!=CatalogKind::SymbolicLink&&!IsSpecialKind(kind))||record.value.size()!=18))return false;
                const std::string_view candidate(reinterpret_cast<const char*>(record.key.data()+12),record.key.size()-13);
                return NormalizedName(candidate,fold)==destinationName;
            };
            const auto victim=std::find_if(catalog.begin(),catalog.end(),matches);
            Check(victim!=catalog.end(),"APFS hard-link overwrite requires a supported file entry at the chosen name");
            if(victim->value.size()==34){
                const auto alias=Le64(victim->value.data()+26);
                Check(UnlinkExternalAlias(catalog,{hardLink->parent,hardLink->replaced,alias},time),
                      "Invalid APFS hard-link replacement alias");
            }else{
                removedKind=DeleteExternalEntry(source,catalog,{hardLink->parent,hardLink->replaced},time,extentTree,externalRetired);
            }
        }
        CreateExternalHardLink(catalog,*hardLink,name,nextId,time,fold);
    }else if(externalDelete){
        catalog=std::move(existing.records);
        if(remove->recursive)
            std::tie(deletedFiles,deletedFolders,deletedSymlinks,deletedSpecial)=DeleteExternalTree(source,catalog,*remove,time,extentTree,externalRetired);
        else if(!UnlinkExternalAlias(catalog,*remove,time))
            removedKind=DeleteExternalEntry(source,catalog,*remove,time,extentTree,externalRetired);
    }else if(externalLinkReplace){
        catalog=std::move(existing.records);
        ReplaceExternalSymlink(catalog,*linkReplacement,time,attributeIds,extentTree,externalRetired);
    }else if(externalResource||externalAttributes){
        catalog=std::move(existing.records);
        ReplaceExternalAttributes(catalog,externalResource?resourceEdit->parent:attributeEdit->parent,entryId,
                                  externalAttributes?attributes:nullptr,externalResource?&*resource:nullptr,
                                  attributeIds,externalResource?resourceEdit->modified:0,time,extentTree,externalRetired);
    }else if(externalReplace){
        catalog=std::move(existing.records);
        TouchExternalParent(catalog,replace->parent,time,0);
        streamAt=ReplaceExternalFile(source,catalog,*replace,dataSize,time,resource?&*resource:nullptr,
                                     attributeIds,extentTree,externalRetired);
    }else if(externalCreate){
        const bool directory=create->kind==CatalogKind::Directory;
        const bool symlink=create->kind==CatalogKind::SymbolicLink;
        const bool special=IsSpecialKind(create->kind);
        Check((directory||symlink||special?!input&&!clone:create->kind==CatalogKind::RegularFile&&(input||clone))&&
              create->parent>=2&&create->parent!=3&&create->parent<nextId&&
              (!(directory||symlink||special)||(!resource&&!create->compression))&&(!directory||!create->locked)&&
              (!symlink||create->linkTarget),
              "Unsupported APFS creation request for this image");
        catalog=std::move(existing.records);
        for(const auto& record:catalog){
            Check(record.key.size()>=8&&(Le64(record.key.data())&IdMask)<entryId,
                  "APFS catalog contains an object at or after its next ID");
        }
        if(create->replacedEntry){
            const bool replacesFile=symlink||create->replacedAlias;
            Check(!directory,"Invalid APFS entry replacement");
            const auto victimKind=replacesFile?CatalogKind::RegularFile:CatalogKind::SymbolicLink;
            const auto parentKey=(uint64_t(9)<<60)|create->parent;
            const auto wanted=NormalizedName(name,fold);
            const auto victim=std::find_if(catalog.begin(),catalog.end(),[&](const auto& record){
                const bool linked=record.value.size()==34;
                if(record.key.size()<14||Le64(record.key.data())!=parentKey||record.key.back()||
                   (record.value.size()!=18&&(!replacesFile||!linked))||
                   Le64(record.value.data())!=create->replacedEntry)return false;
                const auto kind=CatalogKind(Le16(record.value.data()+16));
                if(special){
                    if(create->replacedAlias||linked){if(kind!=CatalogKind::RegularFile)return false;}
                    else if(kind!=CatalogKind::RegularFile&&kind!=CatalogKind::SymbolicLink&&!IsSpecialKind(kind))return false;
                }else if(kind!=victimKind&&(linked||create->replacedAlias||
                         (!IsSpecialKind(kind)&&!((hasSparseInput||restoresSharing)&&kind==CatalogKind::RegularFile))))return false;
                if(create->replacedAlias&&(!linked||Le64(record.value.data()+26)!=create->replacedAlias))
                    return false;
                const std::string_view candidate(reinterpret_cast<const char*>(record.key.data()+12),record.key.size()-13);
                return NormalizedName(candidate,fold)==wanted;
            });
            Check(victim!=catalog.end(),"The APFS replacement entry no longer exists");
            if(victim->value.size()==34){
                const auto alias=Le64(victim->value.data()+26);
                Check(UnlinkExternalAlias(catalog,{create->parent,create->replacedEntry,alias},time),
                      "Invalid APFS replacement alias");
            }else{
                removedKind=DeleteExternalEntry(source,catalog,{create->parent,create->replacedEntry},
                                                time,extentTree,externalRetired);
            }
        }
        std::vector<OwnedRecord> cloneExtents;
        if(clone){
            const auto& physicalExtents=historical?snapshot.historicalData:extentTree.records;
            cloneExtents=CloneDataExtents(catalog,physicalExtents,create->cloneSource,entryId,dataSize);
        }
        TouchExternalParent(catalog,create->parent,time,1);
        auto entry=WriterDisk::DirectoryEntry(name,entryId,time,create->parent,fold,uint16_t(create->kind));
        InsertExternalDirectoryRecord(catalog,std::move(entry),create->parent,name,fold);
        auto inode=WriterDisk::Inode(name,entryId,time,create->parent,
                                     EntryMode(create->kind),directory?0:1,dataSize,create->finder,DeviceNumber(create->posix));
        if(current.writeCipher&&dataSize){
            const auto stream=96+size_t(Le16(inode.value.data()+92))*4+DivideUp(name.size()+1,8)*8;
            Set(inode.value,stream+16,4,8);
        }
        if(dates){
            SetFileDates(inode.value,*dates);
        }
        if(!dataHoles.empty()){
            auto& value=inode.value;
            const auto count=Le16(value.data()+92);
            const auto payload=96+size_t(count)*4;
            const auto stream=payload+size_t(DivideUp(name.size()+1,8)*8);
            Check(count>=2&&value[100]==8&&stream+40<=value.size(),"Missing APFS sparse data stream");
            uint64_t sparseBytes=0;
            for(const auto& hole:dataHoles)sparseBytes+=DivideUp(hole.length,Block)*Block;
            Set(value,stream+8,DivideUp(dataSize,Block)*Block-sparseBytes,8);
            Set(value,48,Le64(value.data()+48)|0x200,8); // INODE_IS_SPARSE
            Set(value,92,count+1,2);
            Set(value,94,Le16(value.data()+94)+8,2);
            value.insert(value.begin()+payload,{13,0x20,8,0}); // INO_EXT_TYPE_SPARSE_BYTES
            const auto end=value.size();
            value.resize(end+8);
            Set(value,end,sparseBytes,8);
        }
        SetPosixMetadata(inode.value,create->posix);
        if(create->locked)Set(inode.value,68,Le32(inode.value.data()+68)|CopiedProtectionFlags(create->posix),4);
        if(clone||restoresSharing){
            constexpr uint64_t wasCloned=0x10,wasEverCloned=0x400;
            Set(inode.value,48,Le64(inode.value.data()+48)|wasCloned|wasEverCloned,8);
        }
        if(attributes)SetSecurityFlag(inode.value,*attributes);
        if(resource)SetResourceFlag(inode.value,resource->size!=0);
        auto records=EncodeAttributes(entryId,attributes,resource?&*resource:nullptr,attributeIds);
        if(symlink)records.push_back(LinkTargetRecord(entryId,*create->linkTarget));
        if(create->compression)WriteCompressionAttribute(records,inode.value,entryId,dataSize,create->compression,attributeIds);
        SortAttributes(records);
        catalog.push_back(std::move(inode));
        catalog.insert(catalog.end(),std::make_move_iterator(records.begin()),std::make_move_iterator(records.end()));
        streamAt=catalog.size();
        catalog.insert(catalog.end(),std::make_move_iterator(cloneExtents.begin()),std::make_move_iterator(cloneExtents.end()));
    }else if(properties){
        catalog=std::move(existing.records);
        uint64_t entryOid=0;
        CatalogKind kind=CatalogKind::RegularFile;
        std::optional<size_t> directoryRecord;
        uint64_t siblingId=0;
        const auto parentKey=(uint64_t(9)<<60)|properties->parent;
        const auto destination=properties->destinationParent?properties->destinationParent:properties->parent;
        const bool moving=destination!=properties->parent;
        for(size_t i=0;i<catalog.size();++i){
            const auto& record=catalog[i];
            if(record.key.size()!=13+name.size()||
               Le64(record.key.data())!=parentKey||
               memcmp(record.key.data()+12,name.c_str(),name.size()+1))continue;
            Check(!directoryRecord&&(record.value.size()==18||record.value.size()==34),
                  "Invalid APFS target directory entry");
            kind=CatalogKind(Le16(record.value.data()+16));
            Check(kind==CatalogKind::RegularFile||kind==CatalogKind::Directory||
                  kind==CatalogKind::SymbolicLink,
                  "APFS edit requires a regular file, a folder, or a symbolic link");
            entryOid=Le64(record.value.data());
            if(record.value.size()==34){
                const auto* value=record.value.data();
                siblingId=Le64(value+26);
                Check(kind==CatalogKind::RegularFile&&siblingId>=16&&
                      siblingId!=entryOid&&siblingId<nextId&&Le16(value+18)==1&&
                      Le16(value+20)==8&&value[22]==1&&Le16(value+24)==8,
                      "Unsupported APFS hard-linked edit");
            }
            directoryRecord=i;
        }
        Check(entryOid>=16&&entryOid<nextId,"APFS catalog edit target was not found");
        if(moving&&kind==CatalogKind::Directory){
            std::map<uint64_t,uint64_t> parents;
            for(const auto& record:catalog){
                if(record.key.size()!=8||Le64(record.key.data())>>60!=3||record.value.size()<92||
                   (Le16(record.value.data()+80)&0170000)!=0040000)continue;
                Check(parents.emplace(Le64(record.key.data())&IdMask,Le64(record.value.data())).second,
                      "Duplicate APFS directory inode");
            }
            std::set<uint64_t> visited;
            for(auto ancestor=destination;ancestor!=2;){
                Check(ancestor!=entryOid,"Cannot move an APFS folder into itself or its descendants");
                const auto link=parents.find(ancestor);
                Check(link!=parents.end()&&visited.insert(ancestor).second,"Invalid APFS destination ancestry");
                ancestor=link->second;
            }
        }
        std::optional<size_t> siblingLink;
        if(siblingId){
            const CatalogSibling sibling{entryOid,properties->parent,name};
            const auto link=sibling.LinkRecord(siblingId);
            const auto map=sibling.MapRecord(siblingId);
            bool mapped=false;
            for(size_t index=0;index<catalog.size();++index){
                const auto& record=catalog[index];
                if(record.key==link.key){
                    Check(!siblingLink&&record.value==link.value,"APFS sibling link differs from the selected name");
                    siblingLink=index;
                }
                if(record.key==map.key){
                    Check(!mapped&&record.value==map.value,"APFS sibling map differs from the selected inode");
                    mapped=true;
                }
            }
            Check(siblingLink&&mapped,"Missing APFS sibling link or map");
        }
        std::string replacement;
        if(properties->newName){
            replacement=EncodeCatalogName(*properties->newName);
        }
        if(properties->folderPosix||mergedFolderAttributes){
            Check(kind==CatalogKind::Directory&&!properties->newName,"APFS merge metadata requires an existing folder");
            if(mergedFolderAttributes)
                ReplaceExternalAttributes(catalog,properties->parent,entryOid,properties->folderAttributes,nullptr,
                                          attributeIds,0,time,extentTree,externalRetired);
        }
        bool edited=false;
        for(auto& record:catalog){
            if(record.key.size()!=8||Le64(record.key.data())!=((uint64_t(3)<<60)|entryOid))continue;
            auto& value=record.value;
            Check(!edited&&value.size()>=92&&
                  (siblingId||Le64(value.data())==properties->parent)&&
                  (kind==CatalogKind::Directory||(siblingId?Le32(value.data()+56)>1:Le32(value.data()+56)==1))&&
                  (Le16(value.data()+80)&0170000)==(uint16_t(kind)<<12),
                  "APFS catalog edit target inode is unsupported");
            const auto flags=Le32(value.data()+68);
            const bool unlocking=properties->locked==false;
            Check(!(flags&(SystemImmutable|AppendOnlyMask))&&(!(flags&UserImmutable)||unlocking),
                  "Unlock the APFS file before editing its properties");
            if(properties->locked)
                Set(value,68,(flags&~UserImmutable)|(*properties->locked?properties->lockFlags:0),4);
            if(properties->newName&&(!siblingId||
               (Le64(value.data())==properties->parent&&ExternalInodeName(value)==name))){
                RenameExternalInode(value,name,replacement);
                if(moving)Set(value,0,destination,8);
            }
            if(properties->finder)SetExternalFinderInfo(value,*properties->finder);
            if(properties->folderPosix)SetPosixMetadata(value,*properties->folderPosix);
            Set(value,32,time,8);
            ApplyTimestampEdits(value,properties->dates);
            edited=true;
        }
        Check(edited,"APFS Properties target inode was not found");
        if(properties->newName){
            if(siblingLink){
                const CatalogSibling renamed{entryOid,destination,replacement};
                catalog[*siblingLink]=renamed.LinkRecord(siblingId);
            }
            auto record=std::move(catalog[*directoryRecord]);
            catalog.erase(catalog.begin()+*directoryRecord);
            const auto hash=WriterDisk::DirectoryHash(replacement,fold);
            record.key.resize(13+replacement.size());
            record.key.back()=0;
            Set(record.key,8,hash|(replacement.size()+1),4);
            std::copy(replacement.begin(),replacement.end(),record.key.begin()+12);
            Set(record.key,0,(uint64_t(9)<<60)|destination,8);
            InsertExternalDirectoryRecord(catalog,std::move(record),destination,replacement,fold);
            TouchExternalParent(catalog,properties->parent,time,moving?-1:0);
            if(moving)TouchExternalParent(catalog,destination,time,1);
        }
    }else{
        catalog=EditCatalog(std::move(existing.records),request,name,nextId,entryId,time,flags==1,
                            directoryCount,fileCount,symlinkCount,specialCount,dataSize,current.blockCount,files,removedKind,streamAt,dates,resource?&*resource:nullptr,attributeIds,retiredStreams);
    }
    if(properties||externalCreate||externalReplace||externalResource||externalAttributes||externalDelete||externalLinkReplace||externalHardLink){
        for(const auto& extent:extentTree.records)
            files.push_back({Le64(extent.value.data()+8),Le64(extent.key.data())&IdMask,
                             Le64(extent.value.data())&IdMask});
    }
    if(restoresSharing){
        std::map<uint64_t,uint64_t> partnerStreams;
        for(const auto& run:shared)partnerStreams.emplace(run.partner,0);
        for(auto& record:catalog){
            if(record.key.size()!=8||Le64(record.key.data())>>60!=3)continue;
            const auto found=partnerStreams.find(Le64(record.key.data())&IdMask);
            if(found==partnerStreams.end())continue;
            Check(record.value.size()>=92&&(Le16(record.value.data()+80)&0170000)==0100000,
                  "Invalid APFS reuse partner inode");
            found->second=Le64(record.value.data()+8);
            Set(record.value,48,Le64(record.value.data()+48)|0x400,8); // INODE_WAS_EVER_CLONED
        }
        for(const auto& run:shared){
            const auto stream=partnerStreams.at(run.partner);
            Check(stream&&std::any_of(catalog.begin(),catalog.end(),[&](const auto& record){
                if(record.key.size()!=16||record.value.size()!=24||
                   Le64(record.key.data())!=((uint64_t(8)<<60)|stream))return false;
                const auto start=Le64(record.value.data()+8),encoded=Le64(record.value.data());
                const auto blocks=(encoded&((uint64_t(1)<<56)-1))/Block;
                if(current.writeCipher&&((encoded>>56)!=1||Le64(record.value.data()+16)!=start))return false;
                return start&&run.physical>=start&&Range(run.physical-start,run.blocks,blocks);
            }),"APFS reuse range is no longer covered by its partner");
        }
    }
    if(!reconcileReferences)ValidateExtentReferences(extentTree,files);
    if(!retiredStreams.empty()){
        catalog.erase(std::remove_if(catalog.begin(),catalog.end(),[&](const auto& record){
            const auto key=Le64(record.key.data());
            return key>>60==8&&retiredStreams.count(key&IdMask);
        }),catalog.end());
    }
    std::set<uint64_t> reconciledRetired;
    std::map<uint64_t,std::vector<ReferenceSpan>> updatedReferences;
    std::vector<OwnedRecord> snapshotUpdates;
    bool extentReferencesChanged=false;
    if(reconcileReferences){
        const auto counts=CatalogReferenceCounts(catalog,shared,bool(current.writeCipher));
        for(const auto& prior:snapshot.historicalData){
            const auto start=Le64(prior.key.data())&IdMask,blocks=Le64(prior.value.data())&IdMask;
            const auto before=Le32(prior.value.data()+16),after=CatalogReferences(counts,start,blocks);
            Check(!after||snapshot.sharedData.count(start),"APFS reused an extent held only by a snapshot");
            if(after==before)continue;
            auto update=prior;
            Set(update.value,0,(uint64_t(2)<<60)|blocks,8);
            Set(update.value,8,UINT64_MAX,8);
            Set(update.value,16,uint32_t(after-before),4);
            snapshotUpdates.push_back(std::move(update));
        }
        for(const auto& current:extentTree.records){
            const auto start=Le64(current.key.data())&IdMask,blocks=Le64(current.value.data())&IdMask;
            const auto before=Le32(current.value.data()+16);
            if(!snapshot.sharedData.count(start)){
                auto spans=SplitExtentReferences(counts,start,blocks);
                for(const auto& span:spans)
                    Check(span.references<=before||clone||restoresSharing,"APFS extent references increased");
                if(spans.size()==1&&spans.front().references==before)continue;
                extentReferencesChanged=true;
                if(spans.size()==1&&!spans.front().references)reconciledRetired.insert(start);
                else updatedReferences.emplace(start,std::move(spans));
                continue;
            }
            const auto after=CatalogReferences(counts,start,blocks);
            Check(after<=before||restoresSharing||clone,"APFS snapshot extent references increased");
            extentReferencesChanged|=after!=before;
        }
    }
    const bool retireFile=replace||removedKind.has_value();
    const auto retiredInode=hardLink&&hardLink->replaced?hardLink->replaced:entryId;
    const auto retiredRun=[&](const FileRun& run){
        if(reconcileReferences)return reconciledRetired.count(run.block)!=0;
        if(externalCreate||externalReplace||externalResource||externalAttributes||externalDelete||externalLinkReplace||externalHardLink||mergedFolderAttributes)
            return externalRetired.count(run.block)!=0;
        return run.stream?retiredStreams.count(run.stream)!=0:retireFile&&run.owner==retiredInode;
    };
    std::vector<DataRun> retiredData;
    uint64_t removedBlocks=0;
    bool removeExtents=extentReferencesChanged;
    for(const auto& run:files){
        if(!retiredRun(run))continue;
        removeExtents=true;
        if(snapshot.sharedData.count(run.block))continue;
        retiredData.push_back({run.block,run.count});
        removedBlocks+=run.count;
    }

    for(const auto& [start,spans]:updatedReferences){
        for(const auto& span:spans){
            if(span.references)continue;
            retiredData.push_back({span.start,span.blocks});
            removedBlocks+=span.blocks;
        }
    }
    const bool changeExtents=dataBlocks||removeExtents;
    auto& space=ephemeral(Le64(superblock.data()+152));
    auto& writes=plan.metadata;
    std::vector<uint64_t> retired{containerMap.headerBlock,containerMap.rootBlock,volumeAddress,
                                  volumeMap.headerBlock,volumeMap.rootBlock};
    uint64_t retainedCatalogBlocks=0;
    for(auto block:existing.blocks){
        if(snapshot.catalogBlocks.count(block))++retainedCatalogBlocks;
        else retired.push_back(block);
    }
    retired.insert(retired.end(),volumeMap.leaves.begin(),volumeMap.leaves.end());
    retired.insert(retired.end(),containerMap.leaves.begin(),containerMap.leaves.end());
    if(changeExtents)retired.insert(retired.end(),extentTree.blocks.begin(),extentTree.blocks.end());

    const size_t firstChild=changeExtents?7:6;
    CatalogLeaves leaves;
    size_t children=0,extentChildren=0,mapChildren=0;
    CatalogLeaves extentLeaves;
    std::vector<DataRun> allocatedData;
    const auto allocated=AllocateWriteBlocks(writes,source,current,space,superblock,ephemeral,
        retired,extentTree.blocks,snapshots,files,std::move(retiredData),dataBlocks,allocatedData,transaction,[&]{
            AssignDataRuns(plan.streams,allocatedData);
            if(input&&dataSize){
                std::vector<OwnedRecord> stream;
                stream.push_back(StreamReference(entryId));
                uint64_t logical=0;
                for(const auto& data:plan.streams.front().runs){
                    const FileRun run{entryId,data.block,data.count};
                    stream.push_back(FileExtent(run,logical,data.block&&bool(current.writeCipher)));
                    logical+=data.count;
                }
                catalog.insert(catalog.begin()+streamAt,std::make_move_iterator(stream.begin()),
                               std::make_move_iterator(stream.end()));
            }
            for(const auto& stream:plan.streams){
                if(!stream.id)continue;
                uint64_t logical=0;
                for(const auto& data:stream.runs){
                    catalog.push_back(FileExtent({entryId,data.block,data.count,stream.id},logical,data.block&&bool(current.writeCipher)));
                    logical+=data.count;
                }
            }
            leaves=PartitionCatalog(std::move(catalog));
            children=leaves.size()>1?leaves.size():0;
            if(children){
                const auto indexes=CatalogIndex(leaves,0).size();
                if(indexes>1)children+=indexes;
            }
            if(changeExtents){
                std::vector<FileRun> updated;
                for(const auto& run:files)if(!retiredRun(run))updated.push_back(run);
                for(const auto& stream:plan.streams)
                    for(const auto& run:stream.runs)if(run.block&&!run.reused)updated.push_back({entryId,run.block,run.count,stream.id});
                std::sort(updated.begin(),updated.end(),[](const auto& a,const auto& b){return a.block<b.block;});
                auto references=snapshotUpdates;
                for(const auto& run:updated){
                    if(snapshot.sharedData.count(run.block))continue;
                    const auto key=(uint64_t(2)<<60)|run.block;
                    const auto old=std::lower_bound(extentTree.records.begin(),extentTree.records.end(),key,
                        [](const auto& record,uint64_t wanted){return Le64(record.key.data())<wanted;});
                    if(reconcileReferences&&old!=extentTree.records.end()&&Le64(old->key.data())==key){
                        const auto changed=updatedReferences.find(run.block);
                        if(changed==updatedReferences.end())references.push_back(*old);
                        else for(const auto& span:changed->second){
                            if(!span.references)continue;
                            auto split=*old;
                            Set(split.key,0,(uint64_t(2)<<60)|span.start,8);
                            Set(split.value,0,(Le64(old->value.data())&~IdMask)|span.blocks,8);
                            Set(split.value,16,span.references,4);
                            references.push_back(std::move(split));
                        }
                    }else references.push_back(ExtentReference(run));
                }

                std::sort(references.begin(),references.end(),[](const auto& a,const auto& b){
                    return Le64(a.key.data())<Le64(b.key.data());
                });
                extentLeaves=PartitionCatalog(std::move(references));
                extentChildren=extentLeaves.size()>1?extentLeaves.size():0;
            }
            mapChildren=MapLeaves(children+1+snapshot.mappings.size());
            return firstChild+children+extentChildren+mapChildren;
        },protectedRuns);
    if(changeExtents){
        StageExtentTree(writes,extentLeaves,allocated,firstChild+children,transaction);
        Set(volume,144,allocated[6],8);
    }
    const auto firstOid=Le64(superblock.data()+88);
    if(children){
        Check(firstOid>volumeMap.mappings.back().oid&&firstOid>oid&&children<=UINT64_MAX-firstOid,
              "APFS virtual object ID space is exhausted or invalid");
        Set(superblock,88,firstOid+children,8);
    }
    auto mappings=StageCatalog(writes,leaves,allocated,firstChild,catalogOid,firstOid,transaction);
    for(auto& entry:mappings)entry.xid=transaction;
    if(current.writeCipher){
        for(auto& entry:mappings){
            entry.flags=4;
            const auto write=std::find_if(writes.begin(),writes.end(),[&](const auto& item){return item.block==entry.block;});
            Check(write!=writes.end(),"Missing staged encrypted APFS catalog block");
            Set(write->bytes,24,Le32(write->bytes.data()+24)|0x10000000U,4);
            Set(write->bytes,0,ObjectChecksum(write->bytes),8);
            current.writeCipher->Encrypt(entry.block*(Block/512),write->bytes.data(),write->bytes.size());
        }
    }
    mappings.insert(mappings.end(),snapshot.mappings.begin(),snapshot.mappings.end());
    std::sort(mappings.begin(),mappings.end(),[](const auto& a,const auto& b){
        return std::tie(a.oid,a.xid)<std::tie(b.oid,b.xid);
    });
    StageMap(writes,volumeMap,allocated[1],allocated[2],mappings,transaction,allocated,firstChild+children+extentChildren);
    const auto oldChildren=existing.blocks.size()-1+volumeMap.leaves.size()+(changeExtents?extentTree.blocks.size()-1:0);
    const auto newChildren=children+extentChildren+mapChildren;
    const auto spaceLimits=ReadVolumeSpace(volume,current.blockCount);
    const auto volumeBlocks=spaceLimits.used;
    const auto removedVolumeBlocks=oldChildren+removedBlocks;
    Check(volumeBlocks>=removedVolumeBlocks&&volumeBlocks<=current.blockCount&&
          dataBlocks+newChildren+retainedCatalogBlocks<=current.blockCount-(volumeBlocks-removedVolumeBlocks),
          "Invalid APFS volume allocation count");
    const auto updatedVolumeBlocks=volumeBlocks-removedVolumeBlocks+newChildren+dataBlocks+retainedCatalogBlocks;
    Check(!spaceLimits.quota||updatedVolumeBlocks<=spaceLimits.quota,
          "The APFS volume quota would be exceeded");
    Check(spaceLimits.reserve<=current.blockCount-siblings.totalReserve,"Invalid APFS shared volume reserves");
    const auto totalReserve=siblings.totalReserve+spaceLimits.reserve;
    const auto oldAllocatedReserve=siblings.allocatedReserve+(std::min)(spaceLimits.reserve,volumeBlocks);
    Check(Le64(space.data()+SpaceReserveCount)==totalReserve&&
          Le64(space.data()+SpaceReserveAllocated)==oldAllocatedReserve,"APFS reserve counters disagree with the volumes");
    const auto allocatedReserve=siblings.allocatedReserve+(std::min)(spaceLimits.reserve,updatedVolumeBlocks);
    // Blocks retired by this transaction are still allocated in its published bitmap.
    Check(Le64(space.data()+72)>=totalReserve-allocatedReserve,
          "The APFS write would consume space reserved for another volume");
    Set(space,SpaceReserveAllocated,allocatedReserve,8);
    Set(volume,88,updatedVolumeBlocks,8);
    Set(volume,128,allocated[1],8);
    Set(volume,176,nextAvailable,8);
    if(create){
        const auto updateCount=[&](size_t offset,uint64_t count,bool added,bool removed){
            Check(!removed||count,"Invalid APFS replacement entry count");
            count-=removed;
            Check(!added||count<UINT64_MAX,"APFS entry count overflow");
            Set(volume,offset,count+added,8);
        };
        updateCount(184,fileCount,create->kind==CatalogKind::RegularFile,removedKind==CatalogKind::RegularFile);
        updateCount(192,directoryCount,create->kind==CatalogKind::Directory,removedKind==CatalogKind::Directory);
        updateCount(200,symlinkCount,create->kind==CatalogKind::SymbolicLink,removedKind==CatalogKind::SymbolicLink);
        updateCount(208,specialCount,IsSpecialKind(create->kind),removedKind&&IsSpecialKind(*removedKind));
    }else if(externalDelete&&remove->recursive){
        Check(deletedFiles<=fileCount&&deletedFolders<=directoryCount&&deletedSymlinks<=symlinkCount&&deletedSpecial<=specialCount,
              "Invalid APFS volume entry counts");
        Set(volume,184,fileCount-deletedFiles,8);
        Set(volume,192,directoryCount-deletedFolders,8);
        Set(volume,200,symlinkCount-deletedSymlinks,8);
        Set(volume,208,specialCount-deletedSpecial,8);
    }else if(removedKind){
        if(*removedKind==CatalogKind::Directory)Set(volume,192,directoryCount-1,8);
        else if(*removedKind==CatalogKind::SymbolicLink)Set(volume,200,symlinkCount-1,8);
        else if(IsSpecialKind(*removedKind)){
            Check(specialCount,"Invalid APFS special file count");
            Set(volume,208,specialCount-1,8);
        }
        else Set(volume,184,fileCount-1,8);
    }
    Set(volume,256,time,8);
    const uint64_t replaced=changeExtents?4:3;
    const auto newVolumeBlocks=replaced+newChildren+dataBlocks,oldVolumeBlocks=replaced+removedVolumeBlocks-retainedCatalogBlocks;
    Check(Le64(volume.data()+224)<=UINT64_MAX-newVolumeBlocks&&Le64(volume.data()+232)<=UINT64_MAX-oldVolumeBlocks,
          "APFS volume allocation history overflow");
    Set(volume,224,Le64(volume.data()+224)+newVolumeBlocks,8);
    Set(volume,232,Le64(volume.data()+232)+oldVolumeBlocks,8);
    StageObject(writes,allocated[3],std::move(volume),oid,transaction,13);
    auto volumeMappings=containerMap.mappings;
    for(auto& mapping:volumeMappings)
        if(mapping.oid==oid)mapping={oid,allocated[3]};
    StageMap(writes,containerMap,allocated[4],allocated[5],volumeMappings,transaction,allocated,0);
    Set(superblock,160,allocated[4],8);
    return plan;
}
}
