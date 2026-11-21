// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MacIcons.h"
#include "ClassicCatalog.h"
#include "AppleImagePlist.h"
#include <wincodec.h>
#include <algorithm>
#include <cstring>
#include <map>
#pragma comment(lib,"windowscodecs.lib")

namespace MacIcons {
namespace {
using HfsPlus::Bytes;
using MacBrowser::Entry;
using MacBrowser::Volume;
using Icons = BrowserDirectory::OwnedIcons;
constexpr size_t MaxIcon = 16 * 1024 * 1024;
constexpr size_t MaxPlist = 1024 * 1024;

uint64_t Big(const uint8_t* bytes, size_t count)
{
    uint64_t value = 0;
    while(count--) value = (value << 8) | *bytes++;
    return value;
}

bool Tag(const uint8_t* bytes, const char* tag)
{
    return memcmp(bytes,tag,4)==0;
}

struct Chunk {
    const uint8_t* type;
    const uint8_t* data;
    size_t size;
};

struct Apartment {
    const HRESULT result = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~Apartment(){if(SUCCEEDED(result))CoUninitialize();}
};

HICON MakeIcon(UINT size, const Bytes& pixels)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=LONG(size);
    info.bmiHeader.biHeight=-LONG(size);
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    const auto color=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!color)return nullptr;
    memcpy(bits,pixels.data(),pixels.size());
    const auto maskStride=((size+15)/16)*2;
    Bytes mask(maskStride*size);
    for(UINT y=0;y<size;++y)
        for(UINT x=0;x<size;++x)
            if(!pixels[(size_t(y)*size+x)*4+3])mask[y*maskStride+x/8]|=0x80>>(x%8);
    const auto transparency=CreateBitmap(size,size,1,1,mask.data());
    ICONINFO icon{};
    icon.fIcon=TRUE;
    icon.hbmMask=transparency;
    icon.hbmColor=color;
    const auto result=transparency?CreateIconIndirect(&icon):nullptr;
    if(transparency)DeleteObject(transparency);
    DeleteObject(color);
    return result;
}

UINT CompressedSize(const Chunk& chunk)
{
    for(const auto& item : {std::pair{"icp4",16u},{"icp5",32u},{"icp6",64u},
         {"ic07",128u},{"ic08",256u},{"ic09",512u},{"ic10",1024u},
         {"ic11",32u},{"ic12",64u},{"ic13",256u},{"ic14",512u},
         {"ic04",16u},{"ic05",32u},{"icsb",18u},{"icsB",36u},
         {"sb24",24u},{"SB24",48u}}){
        if(Tag(chunk.type,item.first))return item.second;
    }
    return 0;
}

bool CompressedImage(const Chunk& chunk)
{
    constexpr uint8_t png[]={137,80,78,71,13,10,26,10};
    constexpr uint8_t jp2[]={0,0,0,12,106,80,32,32,13,10,135,10};
    return (chunk.size>=sizeof(png)&&!memcmp(chunk.data,png,sizeof(png))) ||
           (chunk.size>=sizeof(jp2)&&!memcmp(chunk.data,jp2,sizeof(jp2)));
}

HICON DecodeBitmap(IWICImagingFactory& factory,const Chunk& chunk,UINT expected,UINT output)
{
    CComPtr<IWICStream> stream;
    CComPtr<IWICBitmapDecoder> decoder;
    CComPtr<IWICBitmapFrameDecode> frame;
    UINT width=0,height=0;
    if(FAILED(factory.CreateStream(&stream)) ||
       FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(chunk.data),DWORD(chunk.size))) ||
       FAILED(factory.CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnDemand,&decoder)) ||
       FAILED(decoder->GetFrame(0,&frame)) || FAILED(frame->GetSize(&width,&height)) ||
       width!=expected || height!=expected)return nullptr;

    CComPtr<IWICFormatConverter> converter;
    if(FAILED(factory.CreateFormatConverter(&converter)) ||
       FAILED(converter->Initialize(frame,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,
                                     nullptr,0,WICBitmapPaletteTypeCustom)))return nullptr;
    IWICBitmapSource* source=converter;
    CComPtr<IWICBitmapScaler> scaler;
    if(output!=expected){
        if(FAILED(factory.CreateBitmapScaler(&scaler)) ||
           FAILED(scaler->Initialize(converter,output,output,WICBitmapInterpolationModeFant)))return nullptr;
        source=scaler;
    }
    Bytes pixels(size_t(output)*output*4);
    if(FAILED(source->CopyPixels(nullptr,output*4,UINT(pixels.size()),pixels.data())))return nullptr;
    return MakeIcon(output,pixels);
}

HICON ClassicBitmap(const Chunk& mono,const Chunk* color,UINT size)
{
    const size_t count=size_t(size)*size;
    if(mono.size!=count/4 || (color && color->size!=count))return nullptr;
    Bytes pixels(count*4);
    for(size_t i=0;i<count;++i){
        const unsigned bit=0x80u>>(i%8);
        if(!(mono.data[count/8+i/8]&bit))continue;
        auto* pixel=pixels.data()+i*4;
        unsigned rgb=(mono.data[i/8]&bit)?0:0xffffff;
        if(color){
            // Classic Mac 8-bit system palette: RGB cube, then primary and gray ramps.
            constexpr unsigned ramp[]={238,221,187,170,136,119,85,68,34,17};
            const unsigned index=color->data[i];
            if(index<215)rgb=((5-index/36)*51<<16)|((5-index/6%6)*51<<8)|((5-index%6)*51);
            else if(index<255){
                const unsigned group=(index-215)/10;
                rgb=ramp[(index-215)%10]*(group==0?0x10000:group==1?0x100:group==2?1:0x10101);
            }else rgb=0;
        }
        pixel[0]=uint8_t(rgb);pixel[1]=uint8_t(rgb>>8);pixel[2]=uint8_t(rgb>>16);pixel[3]=255;
    }
    return MakeIcon(size,pixels);
}

bool SafeLeaf(const std::u16string& name)
{
    return !name.empty()&&name.size()<=255&&name!=u"."&&name!=u".."&&
           name.find_first_of(u"/\\:")==name.npos&&name.find(u'\0')==name.npos;
}

std::u16string XmlIconName(Bytes& bytes)
{
    auto reader=AppleImagePlist::Open(bytes);
    enum class Role { Other, Plist, Root, Key, Value };
    struct Node { Role role; std::wstring text; };
    std::vector<Node> stack;
    XmlNodeType node;
    bool wantValue=false, seenIcon=false;
    std::wstring icon;
    HRESULT hr;
    while((hr=reader->Read(&node))==S_OK){
        if(node==XmlNodeType_Element){
            const wchar_t* name=nullptr;
            UINT length=0;
            if(stack.size()>=64||FAILED(reader->GetLocalName(&name,&length)))return {};
            const std::wstring_view tag(name,length);
            const auto parent=stack.empty()?Role::Other:stack.back().role;
            if(parent==Role::Key||parent==Role::Value)return {};
            Role role=Role::Other;
            if(stack.empty()&&tag==L"plist")role=Role::Plist;
            else if(parent==Role::Plist&&tag==L"dict")role=Role::Root;
            else if(parent==Role::Root){
                if(tag==L"key")role=Role::Key;
                else if(wantValue&&tag==L"string")role=Role::Value;
                wantValue=false;
            }
            if(!reader->IsEmptyElement())stack.push_back({role,{}});
        }else if(node==XmlNodeType_EndElement){
            if(stack.empty())return {};
            auto value=std::move(stack.back());
            stack.pop_back();
            if(value.role==Role::Key)wantValue=value.text==L"CFBundleIconFile";
            if(value.role==Role::Value){
                if(seenIcon)return {};
                seenIcon=true;
                icon=std::move(value.text);
            }
        }else if(!stack.empty()&&
                 (stack.back().role==Role::Key||stack.back().role==Role::Value)&&
                 (node==XmlNodeType_Text||node==XmlNodeType_CDATA||node==XmlNodeType_Whitespace)){
            const wchar_t* value=nullptr;
            UINT length=0;
            auto& text=stack.back().text;
            if(FAILED(reader->GetValue(&value,&length))||length>255-text.size())return {};
            text.append(value,length);
        }
    }
    return hr==S_FALSE&&stack.empty()?MacBrowser::U16(icon):std::u16string{};
}

class BinaryPlist {
public:
    explicit BinaryPlist(const Bytes& file): file_(file) {}
    std::u16string IconName()
    {
        if(file_.size()<40)return {};
        const auto* trailer=file_.data()+file_.size()-32;
        offsetWidth_=trailer[6]; refWidth_=trailer[7];
        count_=Big(trailer+8,8);
        const auto root=Big(trailer+16,8);
        table_=Big(trailer+24,8);
        if(!offsetWidth_||offsetWidth_>8||!refWidth_||refWidth_>8||table_<8||
           table_>file_.size()-32||count_>(file_.size()-32-table_)/offsetWidth_)return {};
        uint64_t size=0;
        const auto* pairs=Object(root,0xd0,size);
        if(!pairs || size>Available(pairs)/refWidth_/2)return {};
        std::u16string result;
        for(uint64_t i=0;i<size;++i){
            if(String(Big(pairs+i*refWidth_,refWidth_))!=u"CFBundleIconFile")continue;
            if(!result.empty())return {};
            result=String(Big(pairs+(i+size)*refWidth_,refWidth_));
        }
        return result;
    }
private:
    const Bytes& file_;
    uint64_t table_=0,count_=0;
    unsigned offsetWidth_=0,refWidth_=0;

    size_t Available(const uint8_t* p) const{return size_t(file_.data()+table_-p);}
    const uint8_t* Object(uint64_t ref,unsigned type,uint64_t& length) const
    {
        if(ref>=count_)return nullptr;
        const auto offset=Big(file_.data()+table_+ref*offsetWidth_,offsetWidth_);
        if(offset<8||offset>=table_)return nullptr;
        const auto* p=file_.data()+offset;
        const auto marker=*p++;
        if((marker&0xf0)!=type)return nullptr;
        length=marker&15;
        if(length<15)return p;
        if(!Available(p)||(*p&0xf0)!=0x10||(*p&15)>3)return nullptr;
        const auto width=1u<<(*p++&15);
        if(width>Available(p))return nullptr;
        length=Big(p,width);
        return p+width;
    }
    std::u16string String(uint64_t ref) const
    {
        uint64_t length=0;
        auto* p=Object(ref,0x50,length);
        const bool wide=!p;
        if(wide)p=Object(ref,0x60,length);
        if(!p||length>255||length>Available(p)/(wide?2:1))return {};
        std::u16string text;
        for(uint64_t i=0;i<length;++i)
            text+=wide?char16_t(Big(p+2*i,2)):char16_t(p[i]);
        return text;
    }
};

bool Child(Volume& volume,uint32_t parent,const std::u16string& name,bool directory,Entry& entry)
{
    return volume.FindChild(parent,name,entry)&&entry.directory==directory&&
           !entry.symbolicLink&&!entry.hardLink&&entry.readError.empty();
}

Bytes Read(Volume& volume,const Entry& entry,unsigned fork,size_t limit)
{
    const auto size=entry.forks[fork].size;
    if(!size||size>limit)return {};
    Bytes bytes(static_cast<size_t>(size));
    volume.Read(entry.id,fork,0,bytes.data(),bytes.size());
    return bytes;
}

std::shared_ptr<Icons> Bundle(Volume& volume,const Entry& app)
{
    Entry contents,info,resources,icon;
    if(!Child(volume,app.id,u"Contents",true,contents) ||
       !Child(volume,contents.id,u"Info.plist",false,info) ||
       !Child(volume,contents.id,u"Resources",true,resources))return {};
    auto plist=Read(volume,info,0,MaxPlist);
    if(plist.empty())return {};
    auto name=plist.size()>=8&&!memcmp(plist.data(),"bplist00",8)?
              BinaryPlist(plist).IconName():XmlIconName(plist);
    if(!SafeLeaf(name))return {};
    const auto filename=MacBrowser::Wide(name);
    if(filename.size()<5||CompareStringOrdinal(filename.c_str()+filename.size()-5,
                                                -1,L".icns",-1,TRUE)!=CSTR_EQUAL)name+=u".icns";
    // HFS+ catalog lookup expects the filesystem's decomposed spelling.
    if(!strcmp(volume.Kind(),"HFS+")||!strcmp(volume.Kind(),"HFSX"))
        name=HfsPlus::Normalize(name,volume.CaseSensitive());
    if(!Child(volume,resources.id,name,false,icon))return {};
    return Decode(Read(volume,icon,0,MaxIcon));
}

class ResourceMap {
public:
    bool Parse(const Bytes& bytes)
    {
        const auto length=ResourceForkLength(bytes);
        if(!length||length>bytes.size())return false;
        const size_t data=Big(bytes.data(),4),map=Big(bytes.data()+4,4);
        const size_t dataSize=Big(bytes.data()+8,4),mapSize=Big(bytes.data()+12,4);
        const size_t types=Big(bytes.data()+map+24,2);
        if(types<28||types>mapSize-2)return false;
        const size_t count=(Big(bytes.data()+map+types,2)+1)&0xffff;
        if(count>(mapSize-types-2)/8)return false;
        for(size_t i=0;i<count;++i){
            const auto* type=bytes.data()+map+types+2+i*8;
            const size_t refs=Big(type+6,2);
            const size_t number=(Big(type+4,2)+1)&0xffff;
            if(refs<2+count*8||refs>mapSize-types||number>(mapSize-types-refs)/12)return false;
            for(size_t j=0;j<number;++j){
                const auto* ref=bytes.data()+map+types+refs+j*12;
                if(ref[4]&1)continue;
                const size_t offset=Big(ref+5,3);
                if(offset>dataSize||dataSize-offset<4)return false;
                const size_t size=Big(bytes.data()+data+offset,4);
                if(size>dataSize-offset-4||resources_.size()>=65536)return false;
                const uint64_t key=(Big(type,4)<<16)|Big(ref,2);
                if(!resources_.emplace(key,Chunk{type,bytes.data()+data+offset+4,size}).second)
                    return false;
            }
        }
        return true;
    }

    const Chunk* Find(const char* type,uint16_t id) const
    {
        const auto key=(Big(reinterpret_cast<const uint8_t*>(type),4)<<16)|id;
        const auto found=resources_.find(key);
        return found==resources_.end()?nullptr:&found->second;
    }

    const Chunk* Bundle() const
    {
        const auto type=Big(reinterpret_cast<const uint8_t*>("BNDL"),4);
        const auto found=resources_.lower_bound(type<<16);
        return found!=resources_.end()&&found->first>>16==type?&found->second:nullptr;
    }
private:
    std::map<uint64_t,Chunk> resources_;
};

std::shared_ptr<Icons> ResourceIcons(const ResourceMap& resources,uint16_t id)
{
    if(const auto* modern=resources.Find("icns",id))
        if(auto icon=Decode(Bytes(modern->data,modern->data+modern->size)))return icon;
    auto result=std::make_shared<Icons>();
    if(auto mono=resources.Find("ics#",id))result->small=ClassicBitmap(*mono,resources.Find("ics8",id),16);
    if(auto mono=resources.Find("ICN#",id))result->large=ClassicBitmap(*mono,resources.Find("icl8",id),32);
    return result->small||result->large?result:nullptr;
}

std::shared_ptr<Icons> ResourceIcons(const Bytes& bytes)
{
    ResourceMap map;
    return map.Parse(bytes)?ResourceIcons(map,0xbfb9):nullptr;
}

}

std::shared_ptr<Icons> Decode(const Bytes& bytes)
{
    if(bytes.size()<8||bytes.size()>MaxIcon||!Tag(bytes.data(),"icns")||
       Big(bytes.data()+4,4)!=bytes.size())return {};
    std::vector<Chunk> compressed, classic;
    for(size_t i=8;i<bytes.size();){
        if(bytes.size()-i<8)return {};
        const auto length=Big(bytes.data()+i+4,4);
        if(length<8||length>bytes.size()-i)return {};
        const Chunk chunk{bytes.data()+i,bytes.data()+i+8,size_t(length-8)};
        if(CompressedSize(chunk)&&CompressedImage(chunk))compressed.push_back(chunk);
        else if(Tag(chunk.type,"ics#")||Tag(chunk.type,"ICN#")||Tag(chunk.type,"ics8")||Tag(chunk.type,"icl8"))
            classic.push_back(chunk);
        i+=size_t(length);
        if(compressed.size()+classic.size()>64)return {};
    }

    auto icons=std::make_shared<Icons>();
    Apartment apartment;
    CComPtr<IWICImagingFactory> factory;
    if(!compressed.empty()&&(SUCCEEDED(apartment.result)||apartment.result==RPC_E_CHANGED_MODE))
        factory.CoCreateInstance(CLSID_WICImagingFactory);
    const auto render=[&](UINT size,const char* mono,const char* color){
        const auto rank=[size](const Chunk& chunk){
            const auto width=CompressedSize(chunk);
            return width>=size?width-size:1024+size-width;
        };
        if(factory){
            std::stable_sort(compressed.begin(),compressed.end(),[&](const Chunk& left,const Chunk& right){
                return rank(left)<rank(right);
            });
            for(const auto& chunk:compressed)
                if(auto icon=DecodeBitmap(*factory,chunk,CompressedSize(chunk),size))return icon;
        }
        const Chunk *bitmap=nullptr,*palette=nullptr;
        for(const auto& chunk:classic){
            if(Tag(chunk.type,mono))bitmap=&chunk;
            if(Tag(chunk.type,color))palette=&chunk;
        }
        return bitmap?ClassicBitmap(*bitmap,palette,size):nullptr;
    };
    icons->small=render(16,"ics#","ics8");
    icons->large=render(32,"ICN#","icl8");
    return icons->small||icons->large?icons:nullptr;
}

size_t ResourceForkLength(const Bytes& header)
{
    if(header.size()<16)return 0;
    const size_t data=Big(header.data(),4),map=Big(header.data()+4,4);
    const size_t dataSize=Big(header.data()+8,4),mapSize=Big(header.data()+12,4);
    if(data<16||data>MaxIcon||dataSize>MaxIcon-data||
       map<16||map>MaxIcon||mapSize>MaxIcon-map||mapSize<28||
       (data<map+mapSize&&map<data+dataSize))return 0;
    return (std::max)(data+dataSize,map+mapSize);
}

BundleIcons DecodeBundle(const Bytes& bytes)
{
    ResourceMap resources;
    if(!resources.Parse(bytes))return {};
    const auto* bundle=resources.Bundle();
    if(!bundle||bundle->size<8)return {};
    const size_t sections=(Big(bundle->data+6,2)+1)&0xffff;
    std::map<uint16_t,uint16_t> iconIds,frefIds;
    size_t offset=8;
    for(size_t i=0;i<sections;++i){
        if(bundle->size-offset<6)return {};
        const auto* section=bundle->data+offset;
        const size_t entries=(Big(section+4,2)+1)&0xffff;
        offset+=6;
        if(entries>(bundle->size-offset)/4||entries>4096)return {};
        auto* ids=Tag(section,"FREF")?&frefIds:Tag(section,"ICN#")?&iconIds:nullptr;
        if(ids)
            for(size_t j=0;j<entries;++j){
                const auto* pair=bundle->data+offset+j*4;
                if(!ids->emplace(uint16_t(Big(pair,2)),uint16_t(Big(pair+2,2))).second)return {};
            }
        offset+=entries*4;
    }

    BundleIcons result;
    std::map<uint16_t,std::shared_ptr<Icons>> decoded;
    for(const auto& item:frefIds){
        const auto* reference=resources.Find("FREF",item.second);
        if(!reference||reference->size<6)continue;
        const auto type=uint32_t(Big(reference->data,4));
        if(result.count(type))continue;
        const auto image=iconIds.find(uint16_t(Big(reference->data+4,2)));
        if(image==iconIds.end())continue;
        const auto [icon,fresh]=decoded.try_emplace(image->second);
        if(fresh)icon->second=ResourceIcons(resources,image->second);
        if(icon->second)result.emplace(type,icon->second);
        if(result.size()>=128)break;
    }
    return result;
}

BundleIcons ReadClassicBundle(Volume& volume,uint32_t file,uint64_t forkSize)
{
    if(!forkSize||forkSize>MaxIcon)return {};
    try{
        Bytes bytes(static_cast<size_t>(forkSize));
        MacBrowser::ReadClassicArtwork(volume,file,0,bytes.data(),bytes.size());
        return DecodeBundle(bytes);
    }catch(const HfsPlus::Error&){return {};}
}

std::shared_ptr<Icons> ReadClassic(Volume& volume,uint32_t file,uint64_t forkSize)
{
    if(!forkSize||forkSize>MaxIcon)return {};
    try{
        Bytes bytes(static_cast<size_t>(forkSize));
        MacBrowser::ReadClassicArtwork(volume,file,0,bytes.data(),bytes.size());
        return ResourceIcons(bytes);
    }catch(const HfsPlus::Error&){return {};}
}

void Load(Volume& volume,BrowserDirectory::Row& row)
{
    const auto& entry=row.entry;
    if(row.icon16||row.icon32||entry.symbolicLink||entry.hardLink||!entry.readError.empty())return;
    const auto name=MacBrowser::Wide(entry.name);
    const bool bundle=entry.directory&&name.size()>=4&&
                      CompareStringOrdinal(name.c_str()+name.size()-4,-1,L".app",-1,TRUE)==CSTR_EQUAL;
    const bool custom=(entry.finder[8]&4)!=0;
    if(!bundle&&!custom)return;
    try{
        std::shared_ptr<Icons> icons;
        if(custom){
            Entry carrier=entry;
            if(!entry.directory||Child(volume,entry.id,u"Icon\r",false,carrier))
                icons=ResourceIcons(Read(volume,carrier,1,MaxIcon));
        }
        if(!icons&&bundle)icons=Bundle(volume,entry);
        if(!icons)return;
        row.icon16=icons->small;
        row.icon32=icons->large;
        row.ownedIcons=std::move(icons);
    }catch(const HfsPlus::Error&){
        // Missing or unreadable artwork must not block a folder listing.
    }
}
}
