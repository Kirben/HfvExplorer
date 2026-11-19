// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DmgImage.h"
#include "HfsPlusDisk.h"
#include <atlbase.h>
#include <xmllite.h>
#include <wincrypt.h>
#include <mutex>
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
#pragma comment(lib,"xmllite.lib")
#pragma comment(lib,"crypt32.lib")

namespace Dmg {
namespace {
using namespace HfsPlus;
using namespace HfsPlus::Disk;
constexpr uint64_t SectorBytes=512;
constexpr size_t MaxBlock=64*1024*1024;
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
    // Apple plists commonly name an external DTD. Strip that declaration
    // from the prolog and keep DTD processing disabled.
    constexpr std::string_view prefix="<!DOCTYPE plist";
    const std::string_view text(reinterpret_cast<const char*>(xml.data()),xml.size());
    const auto doctype=text.find(prefix);
    const auto root=text.find("<plist");
    if(doctype!=text.npos&&root!=text.npos&&doctype<root){
        const auto end=text.find('>',doctype);
        Check(end<root&&text.substr(doctype,end-doctype).find('[')==text.npos,
              "DMG plist contains an unsupported DOCTYPE");
        xml.erase(xml.begin()+doctype,xml.begin()+end+1);
    }
    CComPtr<IStream> stream;
    Check(SUCCEEDED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)),"Cannot allocate DMG plist stream");
    ULONG done=0;
    LARGE_INTEGER zero{};
    Check(SUCCEEDED(stream->Write(xml.data(),ULONG(xml.size()),&done))&&done==xml.size()&&
          SUCCEEDED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)),"Cannot load DMG plist stream");
    CComPtr<IXmlReader> reader;
    Check(SUCCEEDED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(&reader),nullptr))&&
          SUCCEEDED(reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit))&&
          SUCCEEDED(reader->SetInput(stream)),"Cannot initialize DMG plist reader");

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

void ParseMap(const Bytes& map,uint64_t sectors,uint64_t data,uint64_t forkBytes,std::vector<Run>& runs)
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
        runs.push_back({(first+start)*SectorBytes,length*SectorBytes,packed?data+base+offset:0,packed,type});
    }
    throw Error("Missing DMG block-map terminator");
}

Layout Parse(Source& file,const Bytes& footer)
{
    const auto data=U64(footer.data()+24),forkBytes=U64(footer.data()+32);
    const auto xmlOffset=U64(footer.data()+216),xmlSize=U64(footer.data()+224);
    const auto sectors=U64(footer.data()+492);
    const auto end=file.Size()-512;
    Check(U32(footer.data()+4)==4&&U32(footer.data()+8)==512&&U32(footer.data()+56)<=1&&
          U32(footer.data()+60)<=1&&sectors&&sectors<=INT64_MAX/SectorBytes,
          "Unsupported UDIF version, segment count or logical image size");
    Check(xmlSize&&xmlSize<=32*1024*1024&&Range(xmlOffset,xmlSize,end)&&Range(data,forkBytes,xmlOffset),
          "Invalid DMG data fork or XML plist");
    Layout layout{sectors*SectorBytes,{}};
    ReadPlist(Disk::Read(file,xmlOffset,size_t(xmlSize)),[&](const Bytes& map){
        ParseMap(map,sectors,data,forkBytes,layout.runs);
    });
    Check(!layout.runs.empty(),"No usable block maps in the DMG plist");
    std::sort(layout.runs.begin(),layout.runs.end(),[](const Run& a,const Run& b){return a.start<b.start;});
    uint64_t last=0;
    for(const auto& run:layout.runs){
        Check(run.start>=last,"Overlapping logical runs in DMG block maps");
        last=run.start+run.bytes;
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
    std::unique_ptr<Source> file_;
    const Layout layout_;
    std::mutex mutex_;
    size_t cached_=SIZE_MAX;
    Bytes bytes_;
public:
    Image(std::unique_ptr<Source> file,Layout layout):file_(std::move(file)),layout_(std::move(layout)){}
    uint64_t Size()const override{return layout_.bytes;}
    bool Writable()const override{return false;}
    bool ProcessLocal()const override{return file_->ProcessLocal();}
    ~Image()override{if(ProcessLocal()&&!bytes_.empty())SecureZeroMemory(bytes_.data(),bytes_.size());}
    void Flush()override{}
    void Write(uint64_t,const void*,size_t)override{throw Error("DMG images are read-only");}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return file_->WorkerFiles();}
    std::shared_ptr<Source> OpenJournal(const Bytes& info)override{return file_->OpenJournal(info);}
    void CheckMedia()override{file_->CheckMedia();}
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
            else if(run->type==Raw)file_->Read(run->offset+offset,dest,take);
            else {
                const auto index=size_t(run-layout_.runs.data());
                if(cached_!=index){
                    auto packed=Disk::Read(*file_,run->offset,size_t(run->packed));
                    Bytes plain(size_t(run->bytes),0);
                    Check(Decode(run->type,packed,plain),"Corrupt or incomplete DMG compressed block");
                    bytes_.swap(plain);
                    cached_=index;
                }
                memcpy(dest,bytes_.data()+size_t(offset),take);
            }
            count-=take;at+=take;dest+=take;
        }
    }
};
}

bool Unwrap(std::unique_ptr<HfsPlus::Source>& source)
{
    if(source->Size()<512)return false;
    const auto footer=HfsPlus::Disk::Read(*source,source->Size()-512,512);
    if(memcmp(footer.data(),"koly",4))return false;
    auto layout=Parse(*source,footer);
    source=std::make_unique<Image>(std::move(source),std::move(layout));
    return true;
}
}
