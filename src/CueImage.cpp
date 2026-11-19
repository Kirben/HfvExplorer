// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CueImage.h"
#include "RawCdImage.h"
#include "HfsPlusDisk.h"
#include "VolumeCoordinator.h"
#include <sstream>

namespace CdMedia {
namespace {
using namespace HfsPlus;
using namespace HfsPlus::Disk;

enum class Mode {Audio,Mode1,RawMode1,RawMode2,XaMode2,Form2};
struct CueTrack {
    unsigned number;
    Mode type;
    std::optional<uint64_t> pregap,index;
};
struct CueFile {std::wstring name;std::vector<CueTrack> tracks;};

unsigned Stride(Mode mode)
{
    switch(mode){
    case Mode::Mode1:return 2048;
    case Mode::XaMode2:return 2336;
    case Mode::Form2:return 2324;
    default:return RawSectorBytes;
    }
}
bool Browsable(Mode mode){return mode!=Mode::Audio&&mode!=Mode::Form2;}
Mode ParseMode(const std::wstring& name)
{
    if(!_wcsicmp(name.c_str(),L"AUDIO"))return Mode::Audio;
    if(!_wcsicmp(name.c_str(),L"MODE1/2048"))return Mode::Mode1;
    if(!_wcsicmp(name.c_str(),L"MODE1/2352"))return Mode::RawMode1;
    if(!_wcsicmp(name.c_str(),L"MODE2/2352"))return Mode::RawMode2;
    if(!_wcsicmp(name.c_str(),L"MODE2/2336"))return Mode::XaMode2;
    if(!_wcsicmp(name.c_str(),L"MODE2/2324"))return Mode::Form2;
    throw Error("Unsupported CUE track type");
}
std::wstring Text(const Bytes& bytes)
{
    const auto p=reinterpret_cast<const char*>(bytes.data());
    const int skip=bytes.size()>=3&&!memcmp(p,"\xef\xbb\xbf",3)?3:0;
    const int len=int(bytes.size())-skip;
    UINT encoding=CP_UTF8;
    auto count=MultiByteToWideChar(encoding,MB_ERR_INVALID_CHARS,p+skip,len,nullptr,0);
    if(!count&&!skip){
        encoding=CP_ACP;
        count=MultiByteToWideChar(encoding,MB_ERR_INVALID_CHARS,p,len,nullptr,0);
    }
    Check(count>0,"Invalid CUE text encoding");
    std::wstring out(count,0);
    Check(MultiByteToWideChar(encoding,MB_ERR_INVALID_CHARS,p+skip,len,out.data(),count)==count&&
          out.find(L'\0')==out.npos,"Invalid CUE text");
    return out;
}
uint64_t CueTime(const std::wstring& token)
{
    unsigned minute=0,second=0,frame=0;
    wchar_t trailing=0;
    Check(token.size()<=11&&swscanf_s(token.c_str(),L"%u:%u:%u%c",&minute,&second,&frame,&trailing,1u)==3&&
          minute<=9999&&second<60&&frame<75,"Invalid CUE index");
    return uint64_t(minute)*4500+second*75+frame;
}
std::vector<CueFile> Parse(Source& source)
{
    Check(source.Size()&&source.Size()<=1024*1024,"Invalid CUE sheet length");
    std::wistringstream text(Text(Disk::Read(source,0,size_t(source.Size()))));
    std::vector<CueFile> files;
    std::wstring line;
    unsigned last=0;
    while(std::getline(text,line)){
        std::wistringstream tokens(line);
        std::wstring command;
        if(!(tokens>>command))continue;
        if(!_wcsicmp(command.c_str(),L"FILE")){
            tokens>>std::ws;
            std::wstring name,type;
            if(tokens.peek()==L'"'){
                tokens.get();
                std::getline(tokens,name,L'"');
            }else tokens>>name;
            tokens>>type;
            Check(!name.empty()&&name!=L"."&&name!=L".."&&name.back()!=L'.'&&name.back()!=L' '&&
                  name.find_first_of(L":/\\\"")==name.npos&&
                  (!_wcsicmp(type.c_str(),L"BINARY")||!_wcsicmp(type.c_str(),L"MOTOROLA")),
                  "CUE sheets must reference sibling BINARY files");
            if(!files.empty())Check(!files.back().tracks.empty(),"CUE FILE has no tracks");
            files.push_back({std::move(name),{}});
        }else if(!_wcsicmp(command.c_str(),L"TRACK")){
            unsigned number=0;std::wstring type;
            Check(!files.empty()&&bool(tokens>>number>>type)&&number>last&&number<=99,"Invalid CUE track order");
            files.back().tracks.push_back({number,ParseMode(type),{},{}});
            last=number;
        }else if(!_wcsicmp(command.c_str(),L"INDEX")){
            unsigned number=0;std::wstring time;
            Check(!files.empty()&&!files.back().tracks.empty()&&bool(tokens>>number>>time)&&number<=99,
                  "Invalid CUE index");
            auto& track=files.back().tracks.back();
            auto* index=number==0?&track.pregap:number==1?&track.index:nullptr;
            if(index){
                Check(!*index,"Duplicate CUE track index");
                *index=CueTime(time);
            }
        }
    }
    Check(!files.empty()&&!files.back().tracks.empty(),"No tracks in the CUE sheet");
    return files;
}
class TrackImage final:public Source {
    std::shared_ptr<Source> sheet_,file_;
    std::unique_ptr<VolumeCoord::ImageDependency> binding_;
    uint64_t start_,sectors_;
    Mode mode_;
public:
    TrackImage(std::shared_ptr<Source> sheet,std::shared_ptr<Source> file,uint64_t start,
               uint64_t sectors,Mode mode,const std::string& key,const std::wstring& filePath)
        :sheet_(std::move(sheet)),file_(std::move(file)),start_(start),sectors_(sectors),mode_(mode){
        if(sheet_)binding_=std::make_unique<VolumeCoord::ImageDependency>(key,filePath);
    }
    uint64_t Size()const override{return sectors_*2048;}
    bool Writable()const override{return false;}
    void Flush()override{}
    void Write(uint64_t,const void*,size_t)override{throw Error("Raw optical images are read-only");}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{
        auto out=sheet_?sheet_->WorkerFiles():std::vector<std::pair<std::wstring,uintptr_t>>{};
        auto file=file_->WorkerFiles();out.insert(out.end(),file.begin(),file.end());return out;
    }
    void Read(uint64_t offset,void* dest,size_t count)override{
        Check(Range(offset,count,Size()),"Read outside the optical track image");
        if(mode_==Mode::Mode1){file_->Read(start_+offset,dest,count);return;}
        auto out=static_cast<BYTE*>(dest);
        std::array<BYTE,RawSectorBytes> buffer{};
        while(count){
            const auto stride=Stride(mode_);
            const auto base=start_+(offset/2048)*stride;
            const auto skip=offset%2048;
            file_->Read(base,buffer.data(),stride);
            const auto mode=RawSectorMode(buffer.data());
            unsigned lead=0;
            if(mode_==Mode::RawMode1){
                Check(mode==SectorMode::Mode1,"Invalid Mode 1 sector in the optical image");lead=16;
            }else{
                if(mode_==Mode::RawMode2)Check(mode==SectorMode::Mode2,"Invalid Mode 2 sector in the optical image");
                const auto sub=buffer.data()+(mode_==Mode::RawMode2?16:0);
                Check(!memcmp(sub,sub+4,4)&&!(sub[2]&0x20),"A filesystem read reached a non-Form 1 Mode 2 sector");
                lead=unsigned(sub-buffer.data())+8;
            }
            const auto take=size_t((std::min)(uint64_t(count),2048-skip));
            memcpy(out,buffer.data()+lead+skip,take);
            out+=take;offset+=take;count-=take;
        }
    }
};
void AddTracks(const std::vector<CueTrack>& tracks,std::shared_ptr<Source> sheet,
               std::shared_ptr<Source> file,const std::string& key,const std::wstring& path,
               std::vector<std::unique_ptr<Source>>& result)
{
    uint64_t base=0,lastFrame=0;
    unsigned stride=Stride(tracks.front().type);
    for(size_t i=0;i<tracks.size();++i){
        const auto& track=tracks[i];
        Check(track.index.has_value()&&(!track.pregap||*track.pregap<=*track.index),
              "Missing or invalid CUE INDEX 01");
        const auto start=track.pregap.value_or(*track.index);
        Check(start>=lastFrame&&(start-lastFrame)<=(file->Size()-base)/stride,"CUE index is outside the binary file");
        base+=(start-lastFrame)*stride;
        stride=Stride(track.type);
        lastFrame=start;
        const auto skip=*track.index-start;
        Check(skip<=(file->Size()-base)/stride,"CUE pregap exceeds the binary file");
        uint64_t length=(file->Size()-base)/stride;
        if(i+1<tracks.size()){
            const auto& next=tracks[i+1];
            Check(next.index.has_value(),"Missing CUE INDEX 01");
            const auto end=next.pregap.value_or(*next.index);
            Check(end>start&&end-start<=length,"CUE tracks overlap or exceed the binary file");
            length=end-start;
        }else Check((file->Size()-base)%stride==0,"Partial raw sector at the end of the binary file");
        Check(length>skip,"Empty CUE track extent");
        if(Browsable(track.type))
            result.push_back(std::make_unique<TrackImage>(sheet,file,base+skip*stride,length-skip,track.type,key,path));
    }
}
}
std::optional<std::vector<std::unique_ptr<HfsPlus::Source>>> OpenTrackImage(
    const std::wstring& path,std::unique_ptr<HfsPlus::Source>& input,const std::string& key)
{
    const auto slash=path.find_last_of(L"\\/");
    const auto dot=path.find_last_of(L'.');
    if(dot==path.npos||(slash!=path.npos&&dot<slash))return {};
    const auto ext=path.c_str()+dot;
    std::vector<std::unique_ptr<Source>> out;
    if(!_wcsicmp(ext,L".cue")){
        const auto files=Parse(*input);
        std::shared_ptr<Source> sheet(std::move(input));
        for(const auto& entry:files){
            const auto filePath=path.substr(0,slash+1)+entry.name;
            const auto attributes=GetFileAttributesW(filePath.c_str());
            Check(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),
                  "CUE binary file is missing or is a link");
            auto file=HfsPlus::OpenImage(filePath.c_str(),false);
            AddTracks(entry.tracks,sheet,std::shared_ptr<Source>(std::move(file)),key,filePath,out);
        }
        return out;
    }
    if(_wcsicmp(ext,L".bin")||!input->Size()||input->Size()%RawSectorBytes)return {};
    const auto header=Disk::Read(*input,0,16);
    const auto mode=RawSectorMode(header.data());
    if(mode==SectorMode::Unknown)return {};
    const auto sectors=input->Size()/RawSectorBytes;
    out.push_back(std::make_unique<TrackImage>(nullptr,std::shared_ptr<Source>(std::move(input)),0,sectors,
                  mode==SectorMode::Mode1?Mode::RawMode1:Mode::RawMode2,key,path));
    return out;
}
}
