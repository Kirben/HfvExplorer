// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DmgImage.h"
#include "HfsPlusDisk.h"
#include "VolumeCoordinator.h"
#include "AppleImagePlist.h"
#include <wincrypt.h>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"
#define BZ_EXPORT
#include "compression/bzip2/bzlib.h"
extern "C" {
#define XZ_USE_CRC64
#include "compression/xz/xz.h"
#include "compression/lzfse/lzfse_internal.h"
}
#pragma comment(lib,"crypt32.lib")

namespace Dmg {
namespace {
using namespace HfsPlus;
using namespace HfsPlus::Disk;
constexpr uint64_t SectorBytes=512;
constexpr size_t MaxBlock=64*1024*1024;
constexpr size_t MaxMetadata=32*1024*1024;
constexpr size_t FooterBytes=512;
enum RunType : uint32_t {
    Zero=0, Raw=1, Ignore=2, Adc=0x80000004, Zlib=0x80000005, Bzip2=0x80000006,
    Lzfse=0x80000007, Xz=0x80000008, Comment=0x7ffffffe, End=0xffffffff
};
struct Run {uint64_t start,bytes,offset,packed;uint32_t type;};
struct Layout {uint64_t bytes;std::vector<Run> runs;};

Bytes Base64(const std::wstring& text)
{
    DWORD size=0;
    Check(!text.empty()&&CryptStringToBinaryW(text.c_str(),DWORD(text.size()),
          CRYPT_STRING_BASE64|CRYPT_STRING_STRICT,nullptr,&size,nullptr,nullptr),
          "Invalid DMG block-map base64");
    Bytes bytes(size);
    Check(CryptStringToBinaryW(text.c_str(),DWORD(text.size()),CRYPT_STRING_BASE64|CRYPT_STRING_STRICT,
          bytes.data(),&size,nullptr,nullptr)&&size==bytes.size(),"Invalid DMG block-map base64");
    return bytes;
}

template<class Add> void ReadPlist(Bytes xml,Add add)
{
    auto reader=AppleImagePlist::Open(xml);

    enum Role {Other,Plist,Root,Resource,BlockArray,Block,Key,Data};
    struct Node {Role role;std::wstring key,text;};
    std::vector<Node> stack;
    XmlNodeType event;
    HRESULT status;
    while((status=reader->Read(&event))==S_OK){
        if(event==XmlNodeType_Element){
            const wchar_t* local=nullptr;
            UINT length=0;
            Check(SUCCEEDED(reader->GetLocalName(&local,&length))&&stack.size()<64,"Invalid DMG plist structure");
            const std::wstring_view name(local,length);
            const auto parent=stack.empty()?Other:stack.back().role;
            std::wstring key;
            if(!stack.empty()&&name!=L"key")key=std::exchange(stack.back().key,{});
            Role role=Other;
            if(stack.empty()&&name==L"plist")role=Plist;
            else if(parent==Plist&&name==L"dict")role=Root;
            else if(parent==Root&&key==L"resource-fork"&&name==L"dict")role=Resource;
            else if(parent==Resource&&key==L"blkx"&&name==L"array")role=BlockArray;
            else if(parent==BlockArray&&name==L"dict")role=Block;
            else if((parent==Root||parent==Resource||parent==Block)&&name==L"key")role=Key;
            else if(parent==Block&&key==L"Data"&&name==L"data")role=Data;
            if(reader->IsEmptyElement()){
                Check(role!=Key&&role!=Data,"Empty DMG plist key or data");
            }else stack.push_back({role});
        }else if(event==XmlNodeType_EndElement){
            Check(!stack.empty(),"DMG plist nesting error");
            auto node=std::move(stack.back());
            stack.pop_back();
            if(node.role==Key)stack.back().key=std::move(node.text);
            if(node.role==Data)add(Base64(node.text));
        }else if(!stack.empty()&&(stack.back().role==Key||stack.back().role==Data)&&
                 (event==XmlNodeType_Text||event==XmlNodeType_CDATA||event==XmlNodeType_Whitespace)){
            const wchar_t* value=nullptr;
            UINT length=0;
            Check(SUCCEEDED(reader->GetValue(&value,&length)),"Cannot read DMG plist value");
            stack.back().text.append(value,length);
        }
    }
    Check(status==S_FALSE&&stack.empty(),"Malformed XML in DMG plist");
}

void ParseMap(const Bytes& map,uint64_t sectors,uint64_t forkBytes,std::vector<Run>& runs)
{
    Check(map.size()>=204&&!memcmp(map.data(),"mish",4)&&U32(map.data()+4)==1,"Invalid DMG block map");
    const auto first=U64(map.data()+8),count=U64(map.data()+16),base=U64(map.data()+24);
    const auto entries=U32(map.data()+200);
    Check(Range(first,count,sectors)&&base<=forkBytes&&entries&&(map.size()-204)/40==entries&&
          (map.size()-204)%40==0,"Invalid DMG block-map bounds");
    for(uint32_t i=0;i<entries;++i){
        const auto p=map.data()+204+size_t(i)*40;
        const auto type=U32(p);
        if(type==Comment)continue;
        if(type==End){
            Check(i+1==entries,"DMG block map has entries after the terminator");
            return;
        }
        const auto start=U64(p+8),length=U64(p+16),offset=U64(p+24),packed=U64(p+32);
        Check(Range(start,length,count),"DMG run is outside its block map");
        if(!length){Check(!packed,"Empty DMG run has stored data");continue;}
        switch(type){
        case Zero:case Ignore:
            Check(!packed,"Unexpected stored bytes in a DMG zero run");
            break;
        case Raw:
            Check(packed==length*SectorBytes,"DMG raw run length mismatch");
            break;
        case Adc:case Zlib:case Bzip2:case Lzfse:case Xz:
            Check(packed&&packed<=MaxBlock&&length*SectorBytes<=MaxBlock,"DMG compressed run exceeds the block limit");
            break;
        default: throw Error("This DMG uses an unsupported compression method");
        }
        if(packed)Check(Range(offset,packed,forkBytes-base),"DMG run exceeds its data fork");
        runs.push_back({(first+start)*SectorBytes,length*SectorBytes,packed?base+offset:0,packed,type});
    }
    throw Error("Missing DMG block-map terminator");
}

// Classic Macintosh resource fork. Only blkx resources feed the UDIF parser.
template<class Add> void ReadResources(const Bytes& fork,Add add)
{
    Check(fork.size()>=16,"Truncated DMG resource fork");
    const uint64_t data=U32(fork.data()),map=U32(fork.data()+4);
    const uint64_t dataBytes=U32(fork.data()+8),mapBytes=U32(fork.data()+12);
    Check(data>=16&&map>=16&&Range(data,dataBytes,fork.size())&&Range(map,mapBytes,fork.size())&&
          (data+dataBytes<=map||map+mapBytes<=data)&&mapBytes>=30&&
          !memcmp(fork.data(),fork.data()+map,16),"Invalid DMG resource fork layout");
    const auto m=fork.data()+map;
    const uint64_t types=U16(m+24),names=U16(m+26);
    Check(types>=28&&names<=mapBytes&&Range(types,2,names),"Invalid DMG resource type list");
    const auto count=uint16_t(U16(m+types)+1);
    const auto references=types+2+uint64_t(count)*8;
    Check(references<=names,"Truncated DMG resource type list");
    for(unsigned i=0;i<count;++i){
        const auto type=m+types+2+i*8;
        if(memcmp(type,"blkx",4))continue;
        const auto entries=uint16_t(U16(type+4)+1);
        const auto first=types+U16(type+6);
        Check(first>=references&&Range(first,uint64_t(entries)*12,names),
              "Truncated DMG resource references");
        for(unsigned j=0;j<entries;++j){
            const auto ref=m+first+j*12;
            const uint64_t block=U32(ref+4)&0xffffff;
            Check(Range(block,4,dataBytes),"Invalid DMG block-map resource offset");
            const auto start=fork.data()+data+block;
            const auto size=U32(start);
            Check(Range(block+4,size,dataBytes),"Truncated DMG block-map resource");
            add(Bytes(start+4,start+4+size));
        }
    }
}

struct Extent {
    uint64_t offset,bytes;
    uint64_t End()const{return offset+bytes;}
};
struct Footer {
    Extent data,xml,resources;
    uint64_t running,sectors;
    uint32_t number,count;
    std::array<uint8_t,16> id;
};

std::optional<Footer> ReadFooter(Source& file)
{
    if(file.Size()<FooterBytes)return {};
    auto trailer=Disk::Read(file,file.Size()-FooterBytes,FooterBytes);
    auto front=false;
    if(memcmp(trailer.data(),"koly",4)){
        trailer=Disk::Read(file,0,FooterBytes);
        if(memcmp(trailer.data(),"koly",4))return {};
        front=true;
    }
    const auto p=trailer.data();
    Check(U32(p+4)==4&&U32(p+8)==FooterBytes,"Unsupported UDIF header version or size");
    Footer out{{U64(p+24),U64(p+32)},{U64(p+216),U64(p+224)},{U64(p+40),U64(p+48)},
               U64(p+16),U64(p+492),U32(p+56),U32(p+60),{}};
    memcpy(out.id.data(),p+64,out.id.size());
    const uint64_t begin=front?FooterBytes:0,end=front?file.Size():file.Size()-FooterBytes;
    for(const auto& region:{out.data,out.xml,out.resources})
        if(region.bytes)Check(region.offset>=begin&&Range(region.offset,region.bytes,end),
                              "A DMG fork extends outside its container");
    const auto disjoint=[](Extent a,Extent b){
        return !a.bytes||!b.bytes||a.End()<=b.offset||b.End()<=a.offset;
    };
    Check(disjoint(out.data,out.xml)&&disjoint(out.data,out.resources)&&disjoint(out.xml,out.resources),
          "Overlapping DMG data and metadata");
    return out;
}

struct Part {
    Footer footer;
    std::unique_ptr<Source> source;
    std::unique_ptr<VolumeCoord::ImageDependency> dependency;
};
std::vector<Part> ReadParts(Source& first,const Footer& footer)
{
    Check(footer.count<=999&&footer.number<=(std::max)(footer.count,1u),
          "Invalid UDIF segment count or number");
    if(footer.count<=1)return {};
    Check(footer.number==1,"Open the first .dmg file of this split image");
    Check(std::any_of(footer.id.begin(),footer.id.end(),[](auto b){return b!=0;}),
          "Split DMG has no segment identifier");
    const auto files=first.WorkerFiles();
    Check(!files.empty(),"The split DMG has no addressable sibling files");
    const auto& path=files.front().first;
    Check(path.size()>4&&!_wcsicmp(path.c_str()+path.size()-4,L".dmg"),
          "Open the first .dmg file of this split image");
    const auto key=VolumeCoord::ImageKey(path);
    std::vector<Part> parts;
    uint64_t running=footer.data.bytes;
    for(uint32_t number=2;number<=footer.count;++number){
        const auto digits=std::to_wstring(number);
        const auto sibling=path.substr(0,path.size()-4)+L"."+std::wstring(3-digits.size(),L'0')+digits+L".dmgpart";
        auto dependency=std::make_unique<VolumeCoord::ImageDependency>(key,sibling);
        auto source=HfsPlus::OpenImage(sibling.c_str(),false);
        const auto candidate=ReadFooter(*source);
        Check(candidate&&candidate->id==footer.id&&candidate->count==footer.count&&candidate->number==number&&
              candidate->running==running&&Range(running,candidate->data.bytes,INT64_MAX),
              "Split DMG segments disagree or are out of order");
        running+=candidate->data.bytes;
        parts.push_back({*candidate,std::move(source),std::move(dependency)});
    }
    return parts;
}

Layout Parse(Source& first,const Footer& footer,const std::vector<Part>& parts)
{
    const auto& last=parts.empty()?footer:parts.back().footer;
    const auto sectors=last.sectors?last.sectors:footer.sectors;
    Check(footer.running==0&&sectors&&sectors<=INT64_MAX/SectorBytes,
          "Invalid UDIF logical disk size or starting offset");
    for(const auto& part:parts)
        Check(!part.footer.sectors||part.footer.sectors==sectors,"Split DMG logical sizes disagree");
    Check(!footer.sectors||footer.sectors==sectors,"Split DMG logical sizes disagree");

    // The block maps refer to offsets in the concatenated data fork. Use
    // the last available metadata in a split image, as written by hdiutil.
    Source* metadata=&first;
    const Footer* directory=&footer;
    for(const auto& part:parts){
        if(part.footer.xml.bytes||part.footer.resources.bytes){
            metadata=part.source.get();
            directory=&part.footer;
        }
    }
    Layout layout{sectors*SectorBytes,{}};
    auto append=[&](const Bytes& map){ParseMap(map,sectors,last.running+last.data.bytes,layout.runs);};
    if(directory->xml.bytes){
        Check(directory->xml.bytes<=MaxMetadata,"DMG XML block maps are too large");
        ReadPlist(Disk::Read(*metadata,directory->xml.offset,size_t(directory->xml.bytes)),append);
    }else if(directory->resources.bytes){
        Check(directory->resources.bytes<=MaxMetadata,"DMG resource fork is too large");
        ReadResources(Disk::Read(*metadata,directory->resources.offset,size_t(directory->resources.bytes)),append);
    }
    Check(!layout.runs.empty(),"No usable block maps in the DMG");
    std::sort(layout.runs.begin(),layout.runs.end(),[](const Run& a,const Run& b){return a.start<b.start;});
    uint64_t end=0;
    for(const auto& run:layout.runs){
        Check(run.start>=end,"Overlapping logical runs in DMG block maps");
        end=run.start+run.bytes;
    }
    return layout;
}

bool DecodeAdc(const Bytes& in,Bytes& out)
{
    size_t src=0,dst=0;
    while(src<in.size()){
        const auto marker=in[src++];
        if(marker&128){
            const size_t length=(marker&127)+1;
            if(!Range(src,length,in.size())||!Range(dst,length,out.size()))return false;
            memcpy(out.data()+dst,in.data()+src,length);
            src+=length;dst+=length;
        }else{
            const bool wide=(marker&64)!=0;
            const size_t length=wide?(marker&63)+4:(marker>>2)+3;
            if(!Range(src,wide?2:1,in.size())||!Range(dst,length,out.size()))return false;
            size_t distance=wide?U16(in.data()+src)+1:(((marker&3)<<8)|in[src])+1;
            src+=wide?2:1;
            if(distance>dst)return false;
            for(size_t i=0;i<length;++i,++dst)out[dst]=out[dst-distance];
        }
    }
    return dst==out.size();
}

bool Decode(uint32_t type,Bytes& in,Bytes& out)
{
    if(type==Adc)return DecodeAdc(in,out);
    if(type==Zlib){
        z_stream stream{};
        stream.zalloc=[](void*,uInt n,uInt size)->void*{return calloc(n,size);};
        stream.zfree=[](void*,void* p){free(p);};
        if(inflateInit(&stream)!=Z_OK)return false;
        stream.next_in=in.data();stream.avail_in=uInt(in.size());
        stream.next_out=out.data();stream.avail_out=uInt(out.size());
        const bool ok=inflate(&stream,Z_FINISH)==Z_STREAM_END&&!stream.avail_in&&!stream.avail_out;
        inflateEnd(&stream);
        return ok;
    }
    if(type==Bzip2){
        bz_stream stream{};
        if(BZ2_bzDecompressInit(&stream,0,0)!=BZ_OK)return false;
        stream.next_in=reinterpret_cast<char*>(in.data());stream.avail_in=unsigned(in.size());
        stream.next_out=reinterpret_cast<char*>(out.data());stream.avail_out=unsigned(out.size());
        const bool ok=BZ2_bzDecompress(&stream)==BZ_STREAM_END&&!stream.avail_in&&!stream.avail_out;
        BZ2_bzDecompressEnd(&stream);
        return ok;
    }
    if(type==Lzfse){
        auto decoder=std::make_unique<lzfse_decoder_state>();
        decoder->src=in.data();decoder->src_begin=in.data();decoder->src_end=in.data()+in.size();
        decoder->dst=out.data();decoder->dst_begin=out.data();decoder->dst_end=out.data()+out.size();
        return lzfse_decode(decoder.get())==LZFSE_STATUS_OK&&decoder->end_of_stream&&
               decoder->src==decoder->src_end&&decoder->dst==decoder->dst_end;
    }
    static const bool initialized=[](){xz_crc32_init();xz_crc64_init();return true;}();
    (void)initialized;
    const std::unique_ptr<xz_dec,decltype(&xz_dec_end)> decoder(xz_dec_init(XZ_SINGLE,0),xz_dec_end);
    if(!decoder)return false;
    xz_buf buffer{in.data(),0,in.size(),out.data(),0,out.size()};
    return xz_dec_run(decoder.get(),&buffer)==XZ_STREAM_END&&buffer.in_pos==in.size()&&buffer.out_pos==out.size();
}

class Image final:public Source {
    std::vector<Part> parts_;
    const Layout layout_;
    std::mutex mutex_;
    size_t cached_=SIZE_MAX;
    Bytes bytes_;

    void ReadPacked(uint64_t at,void* output,size_t count){
        auto p=static_cast<uint8_t*>(output);
        while(count){
            const auto next=std::upper_bound(parts_.begin(),parts_.end(),at,
                [](uint64_t offset,const Part& part){return offset<part.footer.running;});
            Check(next!=parts_.begin(),"DMG data offset lies outside the segments");
            const auto& part=*std::prev(next);
            const auto offset=at-part.footer.running;
            Check(offset<part.footer.data.bytes,"DMG data offset lies outside the segments");
            const auto take=size_t((std::min)(uint64_t(count),part.footer.data.bytes-offset));
            part.source->Read(part.footer.data.offset+offset,p,take);
            at+=take;p+=take;count-=take;
        }
    }
public:
    Image(std::vector<Part> parts,Layout layout):parts_(std::move(parts)),layout_(std::move(layout)){}
    uint64_t Size()const override{return layout_.bytes;}
    bool Writable()const override{return false;}
    bool ProcessLocal()const override{return parts_.front().source->ProcessLocal();}
    ~Image()override{if(ProcessLocal()&&!bytes_.empty())SecureZeroMemory(bytes_.data(),bytes_.size());}
    void Flush()override{}
    void Write(uint64_t,const void*,size_t)override{throw Error("DMG images are read-only");}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{std::vector<std::pair<std::wstring,uintptr_t>> files;
        for(const auto& part:parts_){
            auto owned=part.source->WorkerFiles();
            files.insert(files.end(),owned.begin(),owned.end());
        }
        return files;}
    std::shared_ptr<Source> OpenJournal(const Bytes& info)override{return parts_.front().source->OpenJournal(info);}
    void CheckMedia()override{for(const auto& part:parts_)part.source->CheckMedia();}
    void Read(uint64_t at,void* output,size_t count)override{
        Check(Range(at,count,Size())&&(!count||output),"Read exceeds the DMG logical image");
        std::lock_guard<std::mutex> lock(mutex_);
        auto dest=static_cast<uint8_t*>(output);
        while(count){
            const auto next=std::upper_bound(layout_.runs.begin(),layout_.runs.end(),at,
                [](uint64_t offset,const Run& run){return offset<run.start;});
            const Run* run=next!=layout_.runs.begin()?&*std::prev(next):nullptr;
            size_t take;
            if(!run||at-run->start>=run->bytes){
                // Missing runs are not assumed to be zero. Declared sparse
                // intervals use Zero or Ignore descriptors.
                throw Error("DMG block map does not cover the requested sectors");
            }
            const auto offset=at-run->start;
            take=size_t((std::min)(uint64_t(count),run->bytes-offset));
            if(run->type==Zero||run->type==Ignore)memset(dest,0,take);
            else if(run->type==Raw)ReadPacked(run->offset+offset,dest,take);
            else {
                const auto index=size_t(run-layout_.runs.data());
                if(cached_!=index){
                    cached_=SIZE_MAX;
                    if(ProcessLocal()&&!bytes_.empty())SecureZeroMemory(bytes_.data(),bytes_.size());
                    bytes_.resize(size_t(run->bytes));
                    Bytes packed(size_t(run->packed));
                    struct Wipe {
                        Bytes& data;
                        bool enabled;
                        ~Wipe(){if(enabled&&!data.empty())SecureZeroMemory(data.data(),data.size());}
                    } wipe{packed,ProcessLocal()};
                    ReadPacked(run->offset,packed.data(),packed.size());
                    Check(Decode(run->type,packed,bytes_),"Corrupt or incomplete DMG compressed block");
                    cached_=index;
                }
                memcpy(dest,bytes_.data()+size_t(offset),take);
            }
            count-=take;at+=take;dest+=take;
        }
    }
};
}

bool DecodeAppleDataCompression(const HfsPlus::Bytes& packed,HfsPlus::Bytes& decoded)
{
    return DecodeAdc(packed,decoded);
}

bool Unwrap(std::unique_ptr<HfsPlus::Source>& source)
{
    const auto footer=ReadFooter(*source);
    if(!footer)return false;
    auto parts=ReadParts(*source,*footer);
    auto layout=Parse(*source,*footer,parts);
    parts.insert(parts.begin(),Part{*footer,std::move(source),{}});
    source=std::make_unique<Image>(std::move(parts),std::move(layout));
    return true;
}
}
