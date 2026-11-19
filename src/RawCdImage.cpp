// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "RawCdImage.h"
#include "ReadOnlyCd.h"
#include "HfsPlusDisk.h"
#include "BrowserPresentation.h"
#include "CommandStorage.h"
#include <objbase.h>
#include <cstddef>

namespace CdMedia {
SectorMode RawSectorMode(const BYTE* p)
{
    if(p[0]||p[11]||!std::all_of(p+1,p+11,[](BYTE n){return n==0xff;}))return SectorMode::Unknown;
    if(p[15]==1)return SectorMode::Mode1;
    if(p[15]==2)return SectorMode::Mode2;
    return SectorMode::Unknown;
}
bool IsForm2(const BYTE* p)
{
    return RawSectorMode(p)==SectorMode::Mode2&&!memcmp(p+16,p+20,4)&&(p[18]&0x20);
}
std::array<BYTE,44> WaveHeader(DWORD dataBytes)
{
    std::array<BYTE,44> wave{};
    const auto word=[&](size_t offset,DWORD n,unsigned bytes){
        for(unsigned i=0;i<bytes;++i)wave[offset+i]=BYTE(n>>(8*i));
    };
    memcpy(wave.data(),"RIFF",4);
    memcpy(wave.data()+8,"WAVEfmt ",8);
    memcpy(wave.data()+36,"data",4);
    word(4,dataBytes+36,4);word(16,16,4);word(20,1,2);word(22,2,2);
    word(24,44100,4);word(28,176400,4);word(32,4,2);word(34,16,2);word(40,dataBytes,4);
    return wave;
}

namespace {
using HfsPlus::Bytes;
using HfsPlus::Disk::Check;
using HfsPlus::Disk::Range;

class Native final:public RawSource {
    ReadOnlyCd disc_;
public:
    bool Open(int drive)override{return disc_.OpenDrive(drive);}
    void Close()override{disc_.Close();}
    std::vector<Track> Tracks()const override{return disc_.Tracks();}
    bool Verify(DWORD& id)override{return disc_.MediaGeneration(id);}
    bool Read(const Track& track,uint64_t sector,unsigned count,void* dest,DWORD& got)override{
        got=0;
        if(!disc_.ReadRawTrack(track,sector,count,dest))return false;
        got=count*RawSectorBytes;
        return true;
    }
    DWORD Error()const override{return disc_.Error();}
};

Storage::Result Fail(DWORD error,std::string message) {
    return {1,error?error:ERROR_IO_DEVICE,std::move(message)};
}
struct SourceGuard {RawSource& source;~SourceGuard(){source.Close();}};
std::wstring Absolute(const std::wstring& input)
{
    wchar_t out[32768]{};
    const auto n=GetFullPathNameW(input.c_str(),ARRAYSIZE(out),out,nullptr);
    Check(n&&n<ARRAYSIZE(out),"Invalid optical output path");
    return {out,n};
}
std::wstring Extension(const std::wstring& path,const wchar_t* suffix)
{
    const auto slash=path.find_last_of(L"\\/");
    const auto dot=path.find_last_of(L'.');
    return (dot!=path.npos&&(slash==path.npos||dot>slash)?path.substr(0,dot):path)+suffix;
}
std::wstring WavPath(const std::wstring& base,unsigned number,bool multiple)
{
    if(!multiple)return Extension(base,L".wav");
    const auto prefix=Extension(base,L"");
    return prefix+L"-"+(number<10?L"0":L"")+std::to_wstring(number)+L".wav";
}
std::string Utf8(const std::wstring& s){return BrowserPresentation::Utf8(std::u16string(s.begin(),s.end()));}

class StagedFile {
    std::wstring final_;
    std::unique_ptr<HfsPlus::NewImage> file_;
    uint64_t written_=0;
public:
    StagedFile(std::wstring final,uint64_t bytes):final_(std::move(final)),
        file_(HfsPlus::CreateStagedOutput(final_,bytes)){}
    void Write(const void* src,size_t size){
        file_->Write(written_,src,size);
        written_+=size;
    }
    void Publish(){
        Check(written_==file_->Size(),"The optical output is incomplete");
        file_->PublishTo(final_);
    }
    void Keep(){file_->Keep();}
};

std::string Time(uint64_t sectors)
{
    const auto two=[](uint64_t i){return (i<10?"0":"")+std::to_string(i);};
    return two(sectors/4500)+":"+two(sectors/75%60)+":"+two(sectors%75);
}
std::string Cue(const std::wstring& bin,const std::vector<Track>& tracks,
                const std::vector<SectorMode>& modes)
{
    const auto basename=bin.substr(bin.find_last_of(L"\\/")+1);
    Check(basename.find_first_of(L"\"\r\n")==basename.npos,"The BIN name cannot be written in a CUE sheet");
    std::string text="FILE \""+Utf8(basename)+"\" BINARY\r\n";
    uint64_t at=0;
    for(size_t i=0;i<tracks.size();++i){
        const auto& track=tracks[i];
        const auto type=!track.data?"AUDIO":modes[i]==SectorMode::Mode1?"MODE1/2352":"MODE2/2352";
        text+="  TRACK "+std::string(track.number<10?"0":"")+std::to_string(track.number)+" "+type+"\r\n";
        std::string flags;
        if(!track.data&&(track.control&1))flags+=" PRE";
        if(track.control&2)flags+=" DCP";
        if(!track.data&&(track.control&8))flags+=" 4CH";
        if(!flags.empty())text+="    FLAGS"+flags+"\r\n";
        text+="    INDEX 01 "+Time(at)+"\r\n";
        at+=track.sectors;
    }
    return text;
}

void Verify(RawSource& source,DWORD expected)
{
    DWORD current=0;
    if(!source.Verify(current)||current!=expected)
        throw Storage::MediaChanged("The optical disc changed or its identity could not be verified");
}

bool ValidSelection(const RawSelection& chosen)
{
    if(chosen.tracks.empty())return false;
    if(chosen.format==RawOutput::WaveTracks)
        return std::all_of(chosen.tracks.begin(),chosen.tracks.end(),[](const Track& t){
            return !t.data&&!(t.control&8)&&t.sectors<=(UINT32_MAX-36)/RawSectorBytes;
        });
    if(chosen.format!=RawOutput::BinCue)
        return chosen.tracks.size()==1&&(chosen.format==RawOutput::RawTrack||chosen.tracks[0].data);
    for(size_t i=1;i<chosen.tracks.size();++i){
        const auto& first=chosen.tracks[i-1];const auto& second=chosen.tracks[i];
        if(first.session!=second.session||first.start+first.sectors!=second.start)return false;
    }
    return true;
}

Storage::Result Save(int drive,const std::wstring& base,const RawSelection& chosen,
                     RawSource& source,Storage::Job& job)
{
    const auto& tracks=chosen.tracks;
    if(!ValidSelection(chosen))return Fail(ERROR_NOT_SUPPORTED,"The selected tracks cannot be saved in that format.");
    SourceGuard close{source};
    if(!source.Open(drive))return Fail(source.Error(),"Cannot open the optical disc.");
    Verify(source,chosen.generation);
    const auto observed=source.Tracks();
    size_t previous=0;
    for(size_t i=0;i<tracks.size();++i){
        const auto found=std::find(observed.begin(),observed.end(),tracks[i]);
        if(found==observed.end()||(i&&size_t(found-observed.begin())<=previous))
            throw Storage::MediaChanged("The selected optical track layout changed");
        previous=size_t(found-observed.begin());
        Check(tracks[i].sectors&&tracks[i].start<=UINT32_MAX&&
              tracks[i].sectors<=uint64_t(UINT32_MAX)-tracks[i].start+1,"Invalid optical track extent");
    }

    const bool wave=chosen.format==RawOutput::WaveTracks;
    const bool cue=chosen.format==RawOutput::BinCue;
    const bool form2=chosen.format==RawOutput::Form2Track;
    const DWORD sectorBytes=form2?Form2Bytes:RawSectorBytes;
    uint64_t total=0;
    for(const auto& track:tracks)total+=track.sectors*sectorBytes;

    std::vector<std::unique_ptr<StagedFile>> outputs;
    const auto bin=Extension(base,L".bin");
    if(!wave)outputs.push_back(std::make_unique<StagedFile>(bin,total));
    std::vector<SectorMode> modes;
    Storage::Progress progress;
    progress.byteTransfer=true;progress.scanning=false;progress.totalBytes=total;
    progress.totalFiles=unsigned(tracks.size());
    progress.phase=wave?"Extracting CD audio":"Reading raw optical sectors";
    Bytes buffer(RawBatch*RawSectorBytes);
    for(const auto& track:tracks){
        if(job.cancelled())throw Storage::Cancelled();
        if(wave){
            const auto path=WavPath(base,track.number,tracks.size()>1);
            outputs.push_back(std::make_unique<StagedFile>(path,track.sectors*RawSectorBytes+44));
            const auto header=WaveHeader(DWORD(track.sectors*RawSectorBytes));
            outputs.back()->Write(header.data(),header.size());
        }
        auto& output=*outputs.back();
        progress.path="Track "+std::to_string(track.number);
        progress.fileTotal=track.sectors*sectorBytes;
        progress.fileBytes=0;
        auto mode=SectorMode::Unknown;
        for(uint64_t at=0;at<track.sectors;){
            if(job.cancelled())throw Storage::Cancelled();
            Verify(source,chosen.generation);
            auto count=unsigned((std::min)(uint64_t(RawBatch),track.sectors-at));
            DWORD got=0;
            if(!source.Read(track,at,count,buffer.data(),got)){
                Verify(source,chosen.generation);
                if(count==1||!source.Read(track,at,1,buffer.data(),got))
                    return Fail(source.Error(),"Raw read failed on track "+std::to_string(track.number)+
                                " at sector "+std::to_string(track.start+at)+". Check raw-read support or disc condition.");
                count=1;
            }
            if(got!=count*RawSectorBytes)return Fail(ERROR_READ_FAULT,"A raw sector read was short.");
            Verify(source,chosen.generation);
            if(cue&&track.data&&at==0){
                mode=RawSectorMode(buffer.data());
                if(mode==SectorMode::Unknown)return Fail(ERROR_BAD_FORMAT,"Cannot identify the data-track sector mode for the CUE sheet.");
            }
            if(form2){
                for(unsigned i=0;i<count;++i){
                    const auto sector=buffer.data()+i*RawSectorBytes;
                    if(!IsForm2(sector))return Fail(ERROR_BAD_FORMAT,"The track contains a sector other than Mode 2 Form 2.");
                    output.Write(sector+24,Form2Bytes);
                }
            }else{
                // Normalize MMC audio samples to the little-endian order used by WAV and CUE BINARY.
                if(!track.data&&(wave||cue))
                    for(DWORD i=0;i<got;i+=2)std::swap(buffer[i],buffer[i+1]);
                output.Write(buffer.data(),got);
            }
            at+=count;progress.bytes+=uint64_t(count)*sectorBytes;
            progress.processed=progress.bytes;
            progress.fileBytes=at*sectorBytes;job.progress(progress);
        }
        modes.push_back(mode);
        ++progress.files;job.progress(progress);
    }
    if(cue){
        const auto text=Cue(bin,tracks,modes);
        outputs.push_back(std::make_unique<StagedFile>(Extension(base,L".cue"),text.size()));
        outputs.back()->Write(text.data(),text.size());
    }
    if(job.cancelled())throw Storage::Cancelled();
    Verify(source,chosen.generation);
    // Keep the owned handles open after rename so rollback removes only files
    // this job created, including a BIN already published before its CUE.
    for(auto& output:outputs)output->Publish();
    for(auto& output:outputs)output->Keep();
    return {};
}
}
std::shared_ptr<RawSource> NativeRawSource(){return std::make_shared<Native>();}
std::function<Storage::Result(Storage::Job&)> InspectRawCd(int drive,std::shared_ptr<RawSource> source,
                     std::shared_ptr<std::vector<Track>> tracks,std::shared_ptr<DWORD> generation)
{
    return CommandStorage::Guard({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}},[=](Storage::Job&){
        SourceGuard close{*source};
        if(!source->Open(drive))return Fail(source->Error(),"Cannot open the optical disc.");
        if(!source->Verify(*generation))return Fail(source->Error(),"Cannot identify the optical disc.");
        *tracks=source->Tracks();
        Verify(*source,*generation);
        return tracks->empty()?Fail(ERROR_NOT_SUPPORTED,"No complete optical tracks were found."):Storage::Result{};
    });
}
std::function<Storage::Result(Storage::Job&)> SaveRawCd(int drive,std::wstring output,RawSelection chosen,
                                                      std::shared_ptr<RawSource> source)
{
    return [drive,output=std::move(output),chosen=std::move(chosen),source=std::move(source)](Storage::Job& job){
        Check(drive>=0&&drive<26&&source&&ValidSelection(chosen),"Invalid raw optical output selection");
        const auto base=Absolute(output);
        Check(base.size()>2&&(base[1]!=L':'||towupper(base[0])!=L'A'+drive),
              "Save the optical image on a different drive from the source");
#ifndef HFV_TESTING
        const wchar_t root[]={wchar_t(L'A'+drive),L':',L'\\',0};
        wchar_t volumeRoot[MAX_PATH]{},sourceVolume[MAX_PATH]{},outputVolume[MAX_PATH]{};
        if(GetVolumeNameForVolumeMountPointW(root,sourceVolume,ARRAYSIZE(sourceVolume))&&
           GetVolumePathNameW(base.c_str(),volumeRoot,ARRAYSIZE(volumeRoot))&&
           GetVolumeNameForVolumeMountPointW(volumeRoot,outputVolume,ARRAYSIZE(outputVolume)))
            Check(_wcsicmp(sourceVolume,outputVolume)!=0,"Save the optical image on a different drive from the source");
#endif
        std::vector<VolumeCoord::Access> access{{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}};
        std::vector<std::string> changed;
        const auto reserve=[&](const std::wstring& path){
            const auto target=CommandStorage::ImageWrite(path);
            access.insert(access.end(),target.begin(),target.end());
            changed.push_back(target.back().key);
        };
        if(chosen.format==RawOutput::WaveTracks)
            for(const auto& track:chosen.tracks)reserve(WavPath(base,track.number,chosen.tracks.size()>1));
        else{
            reserve(Extension(base,L".bin"));
            if(chosen.format==RawOutput::BinCue)reserve(Extension(base,L".cue"));
        }
        return CommandStorage::Guard(std::move(access),[&](Storage::Job& running){
            return Save(drive,base,chosen,*source,running);
        },std::move(changed))(job);
    };
}
}
