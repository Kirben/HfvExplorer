// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "OpticalReader.h"

// ECMA-167, parts 3/4, and OSTA UDF 2.60 (including the earlier partition layouts).
namespace Optical {
namespace {
struct Address {uint32_t block;uint16_t map;};
struct Partition {uint32_t start,blocks,sequence,access;};

Address LongAd(const uint8_t* p)
{
    Check(Le32(p)>>30==0&&(Le32(p)&0x3fffffff)!=0,"UDF descriptor is not recorded");
    return {Le32(p+4),Le16(p+8)};
}

bool Entity(const uint8_t* p,const char* id)
{
    const auto n=strlen(id);
    return n<=23&&!memcmp(p+1,id,n)&&(n==23||p[1+n]==0);
}

std::u16string Name(const uint8_t* p,size_t length)
{
    if(!length)return {};
    Check(p[0]==8||(p[0]==16&&length%2==1),"Unsupported UDF Unicode encoding");
    std::u16string result;
    for(size_t i=1;i<length;i+=p[0]==16?2:1)
        result.push_back(char16_t(p[0]==16?HfsPlus::Disk::U16(p+i):p[i]));
    return result;
}

std::optional<uint64_t> Timestamp(const uint8_t* p)
{
    const auto flags=Le16(p);
    const auto zone=flags&0xfff;
    const auto offset=zone>=0x800?int(zone)-4096:int(zone);
    const auto kind=flags>>12;
    if(kind>1||(kind==1&&(offset<-1440||offset>1440)&&offset!=-2047))return {};
    SYSTEMTIME time{};
    time.wYear=Le16(p+2);time.wMonth=p[4];time.wDay=p[5];
    time.wHour=p[6];time.wMinute=p[7];time.wSecond=p[8];
    return CatalogDate(time,kind==1&&offset!=-2047?offset:0);
}

unsigned Tag(const Bytes& data,uint32_t block)
{
    Check(data.size()>=16,"Truncated UDF descriptor");
    unsigned sum=0;
    for(size_t i=0;i<16;++i)if(i!=4)sum+=data[i];
    const auto version=Le16(data.data()+2);
    const auto length=Le16(data.data()+10);
    Check((sum&255)==data[4]&&(version==2||version==3)&&Le32(data.data()+12)==block&&
          length<=data.size()-16,"Invalid UDF descriptor tag");
    uint16_t crc=0;
    for(size_t i=16;i<16+size_t(length);++i){
        crc^=uint16_t(data[i])<<8;
        for(unsigned bit=0;bit<8;++bit)
            crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0));
    }
    Check(crc==Le16(data.data()+8),"UDF descriptor CRC mismatch");
    return Le16(data.data());
}

class Udf final:public Reader {
    enum class MapKind {Physical,Sparable,Virtual,Metadata};
    struct Map {
        MapKind kind=MapKind::Physical;
        Partition physical{};
        unsigned number=0,backing=0;
        unsigned packet=0;
        std::map<uint32_t,uint32_t> remapped;
        uint64_t vatStart=0,vatCount=0;
        Node table;
        bool initialized=false;
    };
    struct Descriptors {
        Bytes logical;
        std::map<unsigned,Partition> partitions;
    };

    const uint64_t blocks_;
    std::vector<Map> maps_;
    Node root_;

    Bytes Absolute(uint64_t block,size_t bytes=BlockSize) {
        Check(block<blocks_&&Range(block*BlockSize,bytes,source_.Size()),"UDF descriptor is outside the image");
        return HfsPlus::Disk::Read(source_,block*BlockSize,bytes);
    }

    Descriptors Sequence(uint32_t start,uint32_t bytes) {
        Check(bytes>=BlockSize&&bytes%BlockSize==0&&Range(start,bytes/BlockSize,blocks_),
              "Invalid UDF descriptor sequence extent");
        Descriptors descriptors;
        for(uint64_t i=start;i<uint64_t(start)+bytes/BlockSize;++i){
            auto desc=Absolute(i);
            const auto tag=Tag(desc,uint32_t(i));
            if(tag==8)break;
            if(tag==5){
                const auto number=Le16(desc.data()+22);
                const auto sequence=Le32(desc.data()+16);
                const auto access=Le32(desc.data()+184);
                const auto first=Le32(desc.data()+188),count=Le32(desc.data()+192);
                Check(count&&first<blocks_&&Range(first,count,uint64_t(UINT32_MAX)+1)&&
                      (access==2||Range(first,count,blocks_)),"UDF physical partition exceeds the image");
                auto old=descriptors.partitions.find(number);
                if(old==descriptors.partitions.end()||sequence>=old->second.sequence)
                    descriptors.partitions[number]={first,count,sequence,access};
            }else if(tag==6&&(descriptors.logical.empty()||Le32(desc.data()+16)>=Le32(descriptors.logical.data()+16)))
                descriptors.logical=std::move(desc);
        }
        Check(!descriptors.logical.empty()&&!descriptors.partitions.empty(),"UDF volume descriptors are incomplete");
        return descriptors;
    }

    void OpenFromAnchors() {
        std::exception_ptr failure;
        for(const auto anchor:{uint64_t(256),blocks_-1,blocks_-257}){
            if(anchor>=blocks_)continue;
            try{
                const auto bytes=Absolute(anchor);
                Check(Tag(bytes,uint32_t(anchor))==2,"Missing UDF anchor");
                for(unsigned field:{16u,24u}){
                    try{
                        Mount(Sequence(Le32(bytes.data()+field+4),Le32(bytes.data()+field)));
                        return;
                    }catch(const HfsPlus::Error&){if(!failure)failure=std::current_exception();}
                }
            }catch(const HfsPlus::Error&){if(!failure)failure=std::current_exception();}
        }
        if(failure)std::rethrow_exception(failure);
        throw HfsPlus::Error("No UDF anchor was found");
    }

    uint64_t Translate(unsigned index,uint64_t lba) {
        Check(index<maps_.size()&&maps_[index].initialized,"Unresolved UDF partition map");
        const auto& map=maps_[index];
        if(map.kind==MapKind::Virtual){
            Check(lba<map.vatCount,"UDF virtual block is outside the allocation table");
            uint8_t word[4]{};
            Contents(map.table,map.vatStart+lba*4,word,4);
            const auto block=Le32(word);
            Check(block!=UINT32_MAX,"UDF virtual block is unallocated");
            return Translate(map.backing,block);
        }
        if(map.kind==MapKind::Metadata){
            Check(lba<map.table.size/BlockSize,"UDF metadata block is out of range");
            auto relative=lba;
            for(const auto& extent:map.table.extents){
                const auto blocks=extent.bytes/BlockSize;
                if(relative<blocks)return Translate(extent.partition,extent.block+relative);
                relative-=blocks;
            }
            throw HfsPlus::Error("UDF metadata block is out of range");
        }
        Check(lba<map.physical.blocks,"UDF block is outside the physical partition");
        if(map.kind==MapKind::Sparable){
            const auto packet=uint32_t(lba/map.packet*map.packet);
            const auto remapped=map.remapped.find(packet);
            if(remapped!=map.remapped.end())return remapped->second+(lba-packet);
        }
        const auto physical=map.physical.start+lba;
        Check(physical<blocks_,"UDF block is past the recorded optical data");
        return physical;
    }

    void ReadExtent(const Extent& extent,uint64_t offset,void* output,size_t size)override {
        auto dest=static_cast<uint8_t*>(output);
        while(size){
            const auto block=extent.block+offset/BlockSize;
            const auto skip=offset%BlockSize;
            const auto physical=Translate(extent.partition,block);
            auto take=size_t((std::min)(uint64_t(size),BlockSize-skip));
            // Physical maps have no translation discontinuities.
            const auto& map=maps_[extent.partition];
            if(map.kind==MapKind::Physical){
                const auto available=(std::min)(map.physical.blocks-block,blocks_-physical)*BlockSize-skip;
                Check(size<=available,"UDF extent exceeds the recorded partition");
                take=size;
            }
            Read(physical*BlockSize+skip,dest,take);
            size-=take;dest+=take;offset+=take;
        }
    }

    Bytes Block(Address where) {
        return Absolute(Translate(where.map,where.block));
    }

    std::string Label(Address where)const {
        return std::to_string(where.map)+":"+std::to_string(where.block);
    }

    struct File {Node node;unsigned type;};
    File Icb(Address where) {
        std::set<std::pair<unsigned,uint32_t>> visited;
        auto raw=Block(where);
        auto tag=Tag(raw,where.block);
        while(tag==259){
            Check(visited.emplace(where.map,where.block).second&&visited.size()<=32,
                  "Cyclic UDF indirect file entries");
            where=LongAd(raw.data()+36);
            raw=Block(where);
            tag=Tag(raw,where.block);
        }
        Check(tag==261||tag==266,"Unsupported UDF file entry");
        auto p=raw.data();
        const auto type=p[27];
        const auto allocation=Le16(p+34)&7;
        const size_t fixed=tag==261?176:216;
        const auto ea=Le32(p+fixed-8),ad=Le32(p+fixed-4);
        Check(Range(fixed,uint64_t(ea)+ad,raw.size())&&fixed+uint64_t(ea)+ad<=16+Le16(p+10),
              "Invalid UDF file entry allocation descriptors");
        Node node;
        node.size=Le64(p+56);
        Check(node.size<=INT64_MAX,"UDF file is too large");
        node.address=Label(where);
        node.descriptorBlock=where.block;
        node.entry.directory=type==4;
        if(const auto modified=Timestamp(p+(tag==261?84:92))){
            node.entry.datesValid=true;
            node.entry.modified=*modified;
            node.entry.created=tag==266?Timestamp(p+104).value_or(*modified):*modified;
        }
        auto descriptors=p+fixed+ea;
        if(type==12){
            node.entry.readError="UDF symbolic links cannot be exported";
            return {std::move(node),type};
        }
        if(allocation==3){
            Check(node.size<=ad,"Truncated UDF embedded file");
            node.embedded.assign(descriptors,descriptors+size_t(node.size));
            return {std::move(node),type};
        }
        Check(allocation<=2,"Unsupported UDF file allocation type");
        Allocation(node,where.map,allocation,descriptors,ad);
        return {std::move(node),type};
    }

    void Allocation(Node& node,unsigned defaultMap,unsigned form,const uint8_t* descriptors,size_t bytes) {
        const size_t descriptorSize=form==0?8:form==1?16:20;
        std::set<std::pair<unsigned,uint32_t>> continued;
        uint64_t allocated=0;
        Bytes extension;
        for(;;){
            Check(bytes%descriptorSize==0,"Misaligned UDF allocation descriptors");
            std::optional<Address> next;
            uint32_t nextBytes=0;
            for(size_t i=0;i<bytes;i+=descriptorSize){
                const auto p=descriptors+i;
                const auto type=Le32(p)>>30,length=Le32(p)&0x3fffffff;
                if(!length)continue;
                Address where{Le32(p+(form==2?12:4)),uint16_t(form==0?defaultMap:Le16(p+(form==2?16:8)))};
                if(type==3){
                    Check(i+descriptorSize==bytes,"Unexpected UDF allocation continuation");
                    next=where;nextBytes=length;break;
                }
                uint32_t recorded=type==0?length:0,logical=length;
                if(form==2){
                    recorded=Le32(p+4);logical=Le32(p+8);
                    Check(recorded<=logical&&logical<=length&&(type==0||!recorded),
                          "Invalid UDF extended allocation lengths");
                }
                if(recorded)node.extents.push_back({where.block,recorded,where.map});
                if(logical>recorded)node.extents.push_back({0,logical-recorded,0,true});
                Check(logical<=UINT64_MAX-allocated,"UDF extent length overflow");
                allocated+=logical;
            }
            if(!next)break;
            Check(continued.emplace(next->map,next->block).second&&continued.size()<=4096,
                  "Cyclic UDF allocation continuation");
            const auto header=Block(*next);
            const auto length=Le32(header.data()+20);
            Check(nextBytes>=24&&Range(24,length,nextBytes)&&length<=UINT16_MAX-8,
                  "Invalid UDF allocation continuation size");
            Bytes data(24+length);
            ReadExtent({next->block,data.size(),next->map},0,data.data(),data.size());
            Check(Tag(data,next->block)==258,"Missing UDF allocation continuation");
            extension=std::move(data);
            descriptors=extension.data()+24;
            bytes=length;
        }
        Check(allocated>=node.size,"Incomplete UDF file allocation");
    }

    void Sparing(Map& map,const uint8_t* p) {
        map.packet=Le16(p+40);
        const auto copies=p[42];
        const auto bytes=Le32(p+44);
        Check(map.packet&&!(map.packet&(map.packet-1))&&copies&&copies<=4&&bytes>=56&&bytes<=UINT16_MAX,
              "Invalid UDF sparing map");
        std::optional<uint32_t> sequence;
        for(unsigned i=0;i<copies;++i){
            uint32_t generation=0;
            std::map<uint32_t,uint32_t> remapped;
            try{
                const auto at=Le32(p+48+i*4);
                auto table=Absolute(at,bytes);
                Check(Tag(table,at)==0&&Entity(table.data()+16,"*UDF Sparing Table"),"Invalid UDF sparing table");
                const auto count=Le16(table.data()+48);
                generation=Le32(table.data()+52);
                Check(Range(56,uint64_t(count)*8,table.size())&&56u+uint64_t(count)*8<=16u+Le16(table.data()+10),
                      "Truncated UDF sparing table");
                for(unsigned j=0;j<count;++j){
                    const auto from=Le32(table.data()+56+j*8),to=Le32(table.data()+60+j*8);
                    if(from>=0xfffffff0)continue;
                    Check(from%map.packet==0&&Range(from,map.packet,map.physical.blocks)&&
                          Range(to,map.packet,blocks_)&&remapped.emplace(from,to).second,
                          "Invalid UDF spared packet");
                }
            }catch(const HfsPlus::Error&){continue;}
            if(!sequence||generation>*sequence){sequence=generation;map.remapped=std::move(remapped);}
            else if(generation==*sequence)Check(map.remapped==remapped,"UDF sparing table copies disagree");
        }
        Check(sequence.has_value(),"No valid UDF sparing table");
        map.initialized=true;
    }

    void Virtual(Map& map,bool modern) {
        const auto& backing=maps_[map.backing].physical;
        const auto end=(std::min)(blocks_,uint64_t(backing.start)+backing.blocks);
        // VAT file entries live at the last recorded block. Also check the
        // physical partition end for images padded after mastering.
        for(const auto boundary:{blocks_,end}){
            for(unsigned skip=1;skip<=4&&skip<=boundary;++skip){
                const auto at=boundary-skip;
                if(at<backing.start||at>=end)continue;
                try{
                    auto file=Icb({uint32_t(at-backing.start),uint16_t(map.backing)});
                    Check(file.type==(modern?248:0),"Unexpected UDF virtual allocation file type");
                    uint64_t begin=0,finish=file.node.size;
                    if(modern){
                        uint8_t header[4]{};
                        Contents(file.node,0,header,sizeof(header));
                        begin=Le16(header);
                        Check(begin==152u+Le16(header+2),"Invalid UDF 2.x virtual allocation header");
                    }else{
                        Check(finish>=36,"Truncated UDF 1.50 virtual allocation table");
                        finish-=36;
                        uint8_t footer[36]{};
                        Contents(file.node,finish,footer,sizeof(footer));
                        Check(Entity(footer,"*UDF Virtual Alloc Tbl"),"Invalid UDF 1.50 virtual allocation footer");
                    }
                    Check(begin<finish&&(finish-begin)%4==0,"Invalid UDF virtual allocation table length");
                    map.vatStart=begin;map.vatCount=(finish-begin)/4;
                    map.table=std::move(file.node);map.initialized=true;
                    return;
                }catch(const HfsPlus::Error&){continue;}
            }
        }
        throw HfsPlus::Error("Cannot locate the UDF virtual allocation table");
    }

    void Metadata(Map& map,const uint8_t* p) {
        for(unsigned i=0;i<2;++i){
            const auto block=Le32(p+40+i*4);
            if(block==UINT32_MAX)continue;
            try{
                auto file=Icb({block,uint16_t(map.backing)});
                Check(file.type==(i==0?250:251)&&file.node.size&&file.node.size%BlockSize==0&&
                      !file.node.extents.empty(),"Invalid UDF metadata file");
                for(const auto& extent:file.node.extents)
                    Check(!extent.sparse&&extent.partition==map.backing&&extent.bytes%BlockSize==0&&
                          Range(extent.block,extent.bytes/BlockSize,map.physical.blocks),
                          "Invalid UDF metadata file extent");
                map.table=std::move(file.node);map.initialized=true;
                return;
            }catch(const HfsPlus::Error&){continue;}
        }
        throw HfsPlus::Error("Cannot open the UDF metadata file or its mirror");
    }

    void ReadMaps(const Descriptors& descriptors) {
        const auto data=descriptors.logical.data();
        const auto bytes=Le32(data+264),count=Le32(data+268);
        Check(count&&count<=UINT16_MAX&&Range(440,bytes,BlockSize)&&
              440+uint64_t(bytes)<=16u+Le16(data+10),"Invalid UDF partition map table");
        std::vector<const uint8_t*> details;
        std::set<unsigned> virtualPartitions;
        size_t at=0;
        for(unsigned i=0;i<count;++i){
            Check(Range(at,2,bytes),"Truncated UDF partition map table");
            const auto p=data+440+at;
            const auto length=p[1];
            Check(((p[0]==1&&length==6)||(p[0]==2&&length==64))&&Range(at,length,bytes),
                  "Unsupported UDF partition map");
            Check(Le16(p+(p[0]==1?2:36))==1,"Multi-volume UDF partition maps are unsupported");
            at+=length;
            Map map;
            map.number=Le16(p+(p[0]==1?4:38));
            const auto partition=descriptors.partitions.find(map.number);
            Check(partition!=descriptors.partitions.end(),"Missing UDF physical partition descriptor");
            map.physical=partition->second;
            if(p[0]==1)map.initialized=true;
            else if(Entity(p+4,"*UDF Sparable Partition")){map.kind=MapKind::Sparable;Sparing(map,p);}
            else if(Entity(p+4,"*UDF Metadata Partition"))map.kind=MapKind::Metadata;
            else if(Entity(p+4,"*UDF Virtual Partition")){
                map.kind=MapKind::Virtual;
                virtualPartitions.insert(map.number);
            }
            else throw HfsPlus::Error("Unsupported UDF partition map type");
            maps_.push_back(std::move(map));details.push_back(p);
        }
        Check(at==bytes,"Unexpected bytes in UDF partition map table");
        // A sequential image can end at the VAT ICB while the write-once
        // partition still declares the unrecorded capacity of the medium.
        for(const auto& map:maps_){
            if(Range(map.physical.start,map.physical.blocks,blocks_))continue;
            Check(map.physical.access==2&&virtualPartitions.count(map.number)&&
                  (map.kind==MapKind::Physical||map.kind==MapKind::Virtual),
                  "Only a write-once UDF partition with a VAT may extend past the image");
        }
        for(size_t i=0;i<maps_.size();++i){
            auto& map=maps_[i];
            if(map.initialized)continue;
            const auto backing=std::find_if(maps_.begin(),maps_.end(),[&](const Map& candidate){
                return candidate.number==map.number&&
                       (candidate.kind==MapKind::Physical||candidate.kind==MapKind::Sparable);
            });
            Check(backing!=maps_.end(),"Missing backing map for UDF virtual or metadata partition");
            map.backing=unsigned(backing-maps_.begin());
            if(map.kind==MapKind::Metadata)Metadata(map,details[i]);
            else Virtual(map,Le16(details[i]+28)>=0x0200);
        }
    }

    uint32_t TagBlock(const Node& folder,uint64_t offset) {
        if(!folder.embedded.empty())return folder.descriptorBlock;
        for(const auto& extent:folder.extents){
            if(offset<extent.bytes){
                Check(!extent.sparse&&extent.block+offset/BlockSize<=UINT32_MAX,"Invalid UDF directory block");
                return uint32_t(extent.block+offset/BlockSize);
            }
            offset-=extent.bytes;
        }
        throw HfsPlus::Error("UDF directory block is not allocated");
    }

    void Mount(const Descriptors& descriptors) {
        maps_.clear();
        const auto p=descriptors.logical.data();
        Check(Le32(p+212)==BlockSize&&Entity(p+216,"*OSTA UDF Compliant"),
              "Unsupported UDF logical block size or domain");
        const auto revision=Le16(p+240);
        Check(revision>=0x0100&&revision<=0x0260,"Unsupported UDF revision");
        ReadMaps(descriptors);
        const auto files=LongAd(p+248);
        const auto set=Block(files);
        Check(Tag(set,files.block)==256,"Missing UDF file set descriptor");
        auto root=Icb(LongAd(set.data()+400));
        Check(root.type==4,"UDF root entry is not a directory");
        root_=std::move(root.node);
        const auto length=p[211];
        Check(length<=127,"Invalid UDF volume identifier");
        root_.entry.name=Name(p+84,length);
        if(root_.entry.name.empty())root_.entry.name=u"UDF";
    }
public:
    explicit Udf(HfsPlus::Source& source):Reader(source),blocks_(source.Size()/BlockSize) {
        Check(source.Size()%BlockSize==0&&blocks_>256&&blocks_<=UINT32_MAX,"Invalid UDF image size");
        OpenFromAnchors();
    }
    const char* Kind()const override{return "UDF";}
    Node Root()const override{return root_;}
    std::vector<Node> List(const Node& folder)override{
        std::vector<Node> entries;
        for(uint64_t at=0;at<folder.size;){
            Bytes header(38);
            Contents(folder,at,header.data(),header.size());
            const auto nameBytes=header[19];
            const auto implBytes=Le16(header.data()+36);
            const auto length=(38u+implBytes+nameBytes+3)&~3u;
            Bytes record(length);
            Contents(folder,at,record.data(),record.size());
            Check(Tag(record,TagBlock(folder,at))==257&&
                  size_t(16+Le16(record.data()+10))>=38u+implBytes+nameBytes,
                  "Invalid UDF directory entry");
            at+=length;
            const auto p=record.data();
            if(p[18]&(4|8|16))continue; // deleted, parent or alternate stream
            auto file=Icb(LongAd(p+20));
            if(file.type!=4&&file.type!=5&&file.type!=12)continue;
            file.node.entry.name=Name(p+38+implBytes,nameBytes);
            Check(file.node.entry.directory==bool(p[18]&2),"UDF directory flag disagrees with file entry");
            entries.push_back(std::move(file.node));
        }
        return entries;
    }
};
}

std::unique_ptr<Reader> OpenUdf(HfsPlus::Source& source) { return std::make_unique<Udf>(source); }
}
