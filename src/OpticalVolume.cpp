// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "OpticalReader.h"
#include "VolumeCoordinator.h"
#include "WorkerEndpoint.h"
#include "StorageHandoff.h"
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"
#include <array>
#include <mutex>
#include <string_view>

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

void Reader::FileContents(const Node& node,uint64_t offset,void* output,size_t bytes)
{
    if(!node.zisofs){Contents(node,offset,output,bytes);return;}
    const auto& format=*node.zisofs;
    Check(Range(offset,bytes,format.size)&&(!bytes||output),"Zisofs read exceeds the file length");

    const uint32_t blockBytes=1u<<format.blockShift;
    const uint64_t table=uint64_t(format.headerWords)*4;
    const uint64_t blockCount=(uint64_t(format.size)+blockBytes-1)/blockBytes;
    const uint64_t data=table+4*(blockCount+1);
    Check(data<=node.size,"Truncated zisofs block table");

    constexpr uint8_t magic[]={0x37,0xe4,0x53,0x96,0xc9,0xdb,0xd6,0x07};
    uint8_t header[16];
    Contents(node,0,header,sizeof(header));
    Check(!memcmp(header,magic,sizeof(magic))&&Le32(header+8)==format.size&&
          header[12]==format.headerWords&&header[13]==format.blockShift&&!header[14]&&!header[15],
          "Zisofs header does not match the directory record");
    uint8_t edge[4];
    Contents(node,table,edge,sizeof(edge));
    Check(Le32(edge)==data,"Invalid first zisofs block pointer");
    Contents(node,table+4*blockCount,edge,sizeof(edge));
    const auto end=Le32(edge);
    Check(end>=data&&end<=node.size,"Invalid last zisofs block pointer");
    if(!bytes)return;

    std::array<uint8_t,16384> input;
    Bytes decoded(size_t(blockBytes)+1);
    auto dest=static_cast<uint8_t*>(output);
    while(bytes){
        const auto blockStart=(offset/blockBytes)*blockBytes;
        const auto expected=uint32_t((std::min)(uint64_t(blockBytes),format.size-blockStart));
        const auto within=size_t(offset-blockStart);
        const auto chunk=(std::min)(bytes,expected-within);
        uint8_t pointers[8];
        Contents(node,table+4*(offset/blockBytes),pointers,sizeof(pointers));
        auto current=Le32(pointers);
        const auto next=Le32(pointers+4);
        Check(current>=data&&current<=next&&next<=end,"Invalid zisofs block pointers");
        if(current==next){
            memset(dest,0,chunk);
        }else{
            z_stream stream{};
            stream.zalloc=[](void*,uInt n,uInt size)->void*{return calloc(n,size);};
            stream.zfree=[](void*,void* p){free(p);};
            Check(inflateInit(&stream)==Z_OK,"Cannot initialize zisofs decompression");
            const std::unique_ptr<z_stream,decltype(&inflateEnd)> cleanup(&stream,inflateEnd);
            stream.next_out=decoded.data();
            stream.avail_out=expected+1;
            int result;
            do{
                if(!stream.avail_in){
                    const auto packed=(std::min)(next-current,uint32_t(input.size()));
                    Check(packed!=0,"Truncated zisofs compressed block");
                    Contents(node,current,input.data(),packed);
                    current+=packed;
                    stream.next_in=input.data();
                    stream.avail_in=packed;
                }
                result=inflate(&stream,Z_NO_FLUSH);
            }while(result==Z_OK&&stream.avail_out);
            Check(result==Z_STREAM_END&&stream.total_out==expected&&!stream.avail_in&&current==next,
                  "Invalid zisofs compressed block");
            memcpy(dest,decoded.data()+within,chunk);
        }
        bytes-=chunk;
        offset+=chunk;
        dest+=chunk;
    }
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

bool Signature(const uint8_t* field,const char* id)
{
    return field[0]==id[0]&&field[1]==id[1];
}

std::pair<const uint8_t*,size_t> SystemUse(const uint8_t* record,size_t size)
{
    Check(size>=34&&record[32]&&record[32]<=size-33,"Truncated ISO 9660 directory record");
    const auto start=size_t(33+record[32]+(record[32]%2==0));
    return start<size?std::make_pair(record+start,size-start):std::make_pair(record+size,size_t(0));
}

std::u16string RockName(const std::string& bytes)
{
    if(bytes.empty())return {};
    const auto length=int(bytes.size());
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),length,nullptr,0);
    if(!count){
        // POSIX names can use an unspecified byte encoding.
        std::u16string name;
        for(unsigned char byte:bytes)name.push_back(char16_t(byte));
        return name;
    }
    std::wstring wide(size_t(count),0);
    Check(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),length,wide.data(),count)==count,
          "Invalid Rock Ridge filename");
    return {wide.begin(),wide.end()};
}

std::optional<uint64_t> RockDate(const uint8_t* p,bool longForm)
{
    SYSTEMTIME time{};
    const auto digits=[](const uint8_t* s,unsigned n){
        unsigned value=0;
        for(unsigned i=0;i<n;++i){
            if(s[i]<'0'||s[i]>'9')return 0u;
            value=value*10+s[i]-'0';
        }
        return value;
    };
    if(longForm){
        time.wYear=WORD(digits(p,4)); time.wMonth=WORD(digits(p+4,2)); time.wDay=WORD(digits(p+6,2));
        time.wHour=WORD(digits(p+8,2)); time.wMinute=WORD(digits(p+10,2)); time.wSecond=WORD(digits(p+12,2));
    }else{
        time.wYear=WORD(1900+p[0]); time.wMonth=p[1]; time.wDay=p[2];
        time.wHour=p[3]; time.wMinute=p[4]; time.wSecond=p[5];
    }
    const auto zone=int(int8_t(p[longForm?16:6]));
    return zone>=-48&&zone<=52?CatalogDate(time,zone*15):std::nullopt;
}

struct RockRecord {
    std::string name,target;
    std::optional<uint32_t> child,parent,mode;
    uint32_t links=0,uid=0,gid=0;
    std::optional<uint64_t> created,modified;
    std::optional<Zisofs> zisofs;
    bool relocated=false,link=false,special=false,sparse=false,unsupportedZisofs=false;
    bool nameMore=false,linkMore=false,componentMore=false;

    void ReadLink(const uint8_t* field,size_t size) {
        Check(size>=5&&(field[4]&~1)==0&&(!link||linkMore),"Invalid Rock Ridge symbolic link");
        link=true;
        linkMore=(field[4]&1)!=0;
        for(size_t at=5;at<size;){
            Check(size-at>=2,"Truncated Rock Ridge link component");
            const auto flags=field[at],length=field[at+1];
            at+=2;
            Check(length<=size-at&&(flags==0||flags==1||flags==2||flags==4||flags==8||flags==16||flags==32),
                  "Invalid Rock Ridge link component");
            if(!componentMore&&!target.empty()&&target.back()!='/')target+='/';
            if(flags<=1){
                Check(length&&std::none_of(field+at,field+at+length,[](uint8_t c){return c==0||c=='/';}),
                      "Invalid Rock Ridge link component name");
                target.append(reinterpret_cast<const char*>(field+at),length);
            }else{
                Check(!length&&!componentMore,"Invalid special Rock Ridge link component");
                if(flags==8){Check(target.empty(),"Misplaced Rock Ridge link root");target="/";}
                else if(flags==2)target+='.';
                else if(flags==4)target+="..";
                else target+=flags==16?"<volume root>":"<host>";
            }
            componentMore=flags==1;
            at+=length;
        }
    }

    void Parse(const uint8_t* field,size_t size) {
        if(Signature(field,"ZF")||Signature(field,"Z2")){
            Check(!zisofs&&!unsupportedZisofs,"Duplicate Rock Ridge compression record");
            if(!Signature(field,"ZF")||field[3]!=1||size<6||memcmp(field+4,"pz",2)){
                unsupportedZisofs=true;
                return;
            }
            Check(size==16&&field[6]>=4&&field[7]>=15&&field[7]<=17,
                  "Invalid Rock Ridge zisofs parameters");
            zisofs=Zisofs{Both32(field+8),field[6],field[7]};
            return;
        }
        if(field[3]!=1)return;
        if(Signature(field,"NM")){
            Check(size>5&&(field[4]&~1)==0&&(name.empty()||nameMore),"Invalid Rock Ridge alternate name");
            name.append(reinterpret_cast<const char*>(field+5),size-5);
            nameMore=(field[4]&1)!=0;
        }else if(Signature(field,"PX")){
            Check((size==36||size==44)&&!mode,"Invalid Rock Ridge POSIX attributes");
            mode=Both32(field+4);links=Both32(field+12);uid=Both32(field+20);gid=Both32(field+28);
        }else if(Signature(field,"SL")){
            ReadLink(field,size);
        }else if(Signature(field,"CL")||Signature(field,"PL")){
            auto& linkBlock=Signature(field,"CL")?child:parent;
            Check(size==12&&!linkBlock,"Invalid Rock Ridge relocation link");
            linkBlock=Both32(field+4);
        }else if(Signature(field,"RE")){
            Check(size==4,"Invalid Rock Ridge relocated-directory record");
            relocated=true;
        }else if(Signature(field,"TF")){
            Check(size>=5,"Truncated Rock Ridge timestamps");
            const auto flags=field[4];
            const auto width=(flags&128)?17u:7u;
            size_t at=5;
            for(unsigned bit=1;bit<128;bit<<=1)if(flags&bit){
                Check(width<=size-at,"Truncated Rock Ridge timestamp");
                if(bit==1)created=RockDate(field+at,width==17);
                if(bit==2)modified=RockDate(field+at,width==17);
                at+=width;
            }
            Check(at==size,"Invalid Rock Ridge timestamp length");
        }else if(Signature(field,"PN")){
            special=true;
        }else if(Signature(field,"SF")){
            sparse=true;
        }
    }
};

class Iso final:public Reader {
    uint64_t blocks_=0;
    bool joliet_;
    Node root_;
    bool rockRidge_=false;
    size_t suspSkip_=0;

    template<class Visit>
    void WalkSusp(const uint8_t* bytes,size_t size,Visit visit)
    {
        constexpr size_t continuationBudget=64*1024;
        size_t remaining=continuationBudget;
        std::set<uint64_t> visited;
        Bytes continuation;
        for(;;){
            uint64_t next=0,length=0;
            while(size&&*bytes){
                Check(size>=4&&bytes[2]>=4&&bytes[2]<=size,"Invalid ISO System Use entry length");
                const auto width=bytes[2];
                if(Signature(bytes,"ST")&&bytes[3]==1){
                    Check(width==4,"Invalid ISO System Use terminator");
                    break;
                }
                if(Signature(bytes,"CE")&&bytes[3]==1){
                    Check(width==28&&!length,"Invalid ISO continuation record");
                    const auto block=Both32(bytes+4),offset=Both32(bytes+12);
                    length=Both32(bytes+20);
                    next=uint64_t(block)*BlockSize+offset;
                    Check(offset<BlockSize&&length&&length<=remaining&&visited.size()<32&&
                          Range(next,length,blocks_*BlockSize)&&visited.insert(next).second,
                          "Invalid or cyclic ISO continuation area");
                }else visit(bytes,width);
                bytes+=width;
                size-=width;
            }
            if(!length)return;
            remaining-=size_t(length);
            continuation.resize(size_t(length));
            Read(next,continuation.data(),continuation.size());
            bytes=continuation.data();
            size=continuation.size();
        }
    }

    void RecognizeRockRidge()
    {
        if(joliet_)return;
        std::array<uint8_t,BlockSize> block{};
        const auto bytes=size_t((std::min)(BlockSize,root_.size));
        Contents(root_,0,block.data(),bytes);
        const auto length=block[0];
        if(length<34||length>bytes||block[32]!=1||block[33]!=0)return;
        auto [fields,size]=SystemUse(block.data(),length);
        for(size_t prefix:{0u,14u}){ // CD-ROM XA can precede the root's SP field.
            if(size<prefix+7)continue;
            const auto p=fields+prefix;
            if(!Signature(p,"SP")||p[2]!=7||p[3]!=1||p[4]!=0xbe||p[5]!=0xef)continue;
            suspSkip_=p[6];
            WalkSusp(p,size-prefix,[&](const uint8_t* field,size_t width){
                if(!Signature(field,"ER")||field[3]!=1)return;
                Check(width>=8&&size_t(8+field[4]+field[5]+field[6])<=width,
                      "Invalid ISO extension reference");
                const std::string_view id(reinterpret_cast<const char*>(field+8),field[4]);
                rockRidge_|=field[7]==1&&(id=="RRIP_1991A"||id=="IEEE_P1282");
            });
            return;
        }
    }

    RockRecord Rock(const uint8_t* record,size_t length)
    {
        RockRecord rock;
        auto [fields,size]=SystemUse(record,length);
        if(size>suspSkip_)WalkSusp(fields+suspSkip_,size-suspSkip_,[&](const uint8_t* field,size_t width){
            rock.Parse(field,width);
        });
        Check(!rock.nameMore&&!rock.linkMore&&!rock.componentMore&&(!rock.link||!rock.target.empty()),
              "Incomplete Rock Ridge name or link");
        return rock;
    }

    Node FollowRelocation(uint32_t block,const Node& folder)
    {
        Check(Range(uint64_t(block)*BlockSize,BlockSize,blocks_*BlockSize),"Rock Ridge child link is outside the volume");
        std::array<uint8_t,BlockSize> buffer{};
        Read(uint64_t(block)*BlockSize,buffer.data(),buffer.size());
        const auto self=buffer.data();
        Check(self[0]>=34&&self[32]==1&&self[33]==0,"Invalid Rock Ridge relocated self record");
        auto entry=Record(self,self[0]);
        Check(entry.entry.directory&&entry.size&&entry.extents.front().block==block,
              "Rock Ridge child link does not point to a directory");
        const auto parent=self+self[0];
        Check(parent[0]>=34&&parent[32]==1&&parent[33]==1,"Invalid Rock Ridge relocated parent record");
        const auto back=Rock(parent,parent[0]).parent;
        Check(back&&*back==folder.extents.front().block,"Rock Ridge relocated parent does not match");
        return entry;
    }

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
        RecognizeRockRidge();
    }
    const char* Kind()const override{return joliet_?"ISO 9660 / Joliet":rockRidge_?"ISO 9660 / Rock Ridge":"ISO 9660";}
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
                    const auto rock=rockRidge_?Rock(p,length):RockRecord{};
                    if(rock.relocated){
                        Check(!continued,"Relocated Rock Ridge entry uses multiple extents");
                        continue;
                    }
                    if(rock.child){
                        Check(!continued&&!rock.link,"Invalid Rock Ridge child link");
                        item=FollowRelocation(*rock.child,folder);
                    }
                    item.entry.name=rock.name.empty()?raw:RockName(rock.name);
                    if(rock.name.empty()&&!item.entry.directory && item.entry.name.size()>=2 &&
                       item.entry.name.substr(item.entry.name.size()-2)==u";1")
                        item.entry.name.resize(item.entry.name.size()-2);
                    if(rock.name.empty()&&!item.entry.directory&&!item.entry.name.empty()&&item.entry.name.back()==u'.')
                        item.entry.name.pop_back();
                    if(rock.created){item.entry.created=*rock.created;item.entry.datesValid=true;}
                    if(rock.modified){item.entry.modified=*rock.modified;item.entry.datesValid=true;}
                    if(rock.mode){
                        const auto type=*rock.mode&0170000;
                        char permissions[8]{};
                        snprintf(permissions,sizeof(permissions),"%04o",unsigned(*rock.mode&07777));
                        item.details=std::string("POSIX mode ")+permissions+
                            ", uid "+std::to_string(rock.uid)+", gid "+std::to_string(rock.gid)+
                            ", links "+std::to_string(rock.links);
                        if(type){
                            Check(item.entry.directory==(type==0040000),
                                  "Rock Ridge file type does not match its ISO directory record");
                            if(type!=0100000&&type!=0040000&&!rock.link)
                                item.entry.readError="Rock Ridge special files cannot be exported";
                        }
                    }
                    if(rock.special)item.entry.readError="Rock Ridge special files cannot be exported";
                    if(rock.link){
                        Check(!item.entry.directory,"Rock Ridge symbolic link was marked as a directory");
                        item.entry.readError="Rock Ridge symbolic links cannot be exported";
                        item.details="Symbolic link to "+rock.target;
                    }
                    if(rock.sparse)item.entry.readError="Rock Ridge sparse file data is unsupported";
                    if(rock.unsupportedZisofs)item.entry.readError="This Rock Ridge compression format is unsupported";
                    if(rock.zisofs){
                        Check(!item.entry.directory&&!continued&&item.entry.readError.empty(),
                              "Zisofs is valid only for regular single-extent files");
                        item.zisofs=rock.zisofs;
                        item.entry.compressed=true;
                    }
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
                    }else child.entry.forks[0].size=child.zisofs?child.zisofs->size:child.size;
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
    std::string PropertiesDetails(uint32_t id)override{return Access([&]{return At(id).details;});}
    void Read(uint32_t id,unsigned fork,uint64_t offset,void* p,size_t n)override{
        Access([&]{
            const auto& node=At(id);
            Check(!node.entry.directory&&fork<2&&(fork==0||(!offset&&!n)),"Invalid optical file fork");
            if(!node.entry.readError.empty())throw HfsPlus::Error(node.entry.readError);
            if(fork==0)reader_->FileContents(node,offset,p,n);
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
