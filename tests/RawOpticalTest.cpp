// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#ifdef HFV_TESTING
#include "RawCdImage.h"
#include "HfsPlusDisk.h"
#include "MacBrowserModel.h"
#include "CopyProcess.h"
#include "VolumeCoordinator.h"

namespace {
using namespace CdMedia;
using HfsPlus::Bytes;
using HfsPlus::Disk::Check;
class Disc final:public RawSource {
public:
    std::vector<Track> tracks;
    std::map<unsigned,Bytes> data;
    DWORD generation=71;
    DWORD error=0;
    bool shortRead=false,swap=false;
    Storage::Job* cancel=nullptr;
    unsigned opens=0,closes=0,reads=0;
    bool Open(int)override{++opens;return true;}
    void Close()override{++closes;}
    std::vector<Track> Tracks()const override{return tracks;}
    bool Verify(DWORD& id)override{id=generation;return true;}
    bool Read(const Track& t,uint64_t at,unsigned sectors,void* output,DWORD& bytes)override{
        Check(std::find(tracks.begin(),tracks.end(),t)!=tracks.end()&&at<t.sectors&&sectors<=t.sectors-at,
              "Raw reader crossed a track boundary");
        ++reads;
        if(error)return false;
        const auto& payload=data.at(t.number);
        bytes=sectors*RawSectorBytes;
        memcpy(output,payload.data()+at*RawSectorBytes,bytes);
        if(shortRead)--bytes;
        if(swap)++generation;
        if(cancel)cancel->cancel();
        return true;
    }
    DWORD Error()const override{return error;}
};
uint32_t Little(const BYTE* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
void Both16(BYTE* p,unsigned n){p[0]=BYTE(n);p[1]=BYTE(n>>8);HfsPlus::Disk::P16(p+2,uint16_t(n));}
void Both32(BYTE* p,unsigned n){for(unsigned i=0;i<4;++i)p[i]=BYTE(n>>(i*8));HfsPlus::Disk::P32(p+4,n);}
unsigned Record(BYTE* p,unsigned lba,unsigned length,bool directory,std::string_view name)
{
    const auto bytes=unsigned(33+name.size()+(name.size()%2==0));
    p[0]=BYTE(bytes);Both32(p+2,lba);Both32(p+10,length);p[25]=directory?2:0;Both16(p+28,1);
    p[32]=BYTE(name.size());memcpy(p+33,name.data(),name.size());
    return bytes;
}
Bytes IsoRaw()
{
    constexpr unsigned blocks=32,sector=2048;
    Bytes iso(blocks*sector,0);
    auto p=iso.data()+16*sector;
    p[0]=1;memcpy(p+1,"CD001",5);p[6]=1;memset(p+40,' ',32);
    memcpy(p+40,"Mixed disc",10);Both32(p+80,blocks);Both16(p+128,sector);
    Record(p+156,19,sector,true,std::string_view("\0",1));
    p=iso.data()+17*sector;p[0]=255;memcpy(p+1,"CD001",5);p[6]=1;
    p=iso.data()+19*sector;
    p+=Record(p,19,sector,true,std::string_view("\0",1));
    p+=Record(p,19,sector,true,std::string_view("\1",1));
    Record(p,20,4,false,"TEST.TXT;1");
    memcpy(iso.data()+20*sector,"data",4);
    Bytes raw(blocks*RawSectorBytes,0);
    for(unsigned i=0;i<blocks;++i){
        p=raw.data()+i*RawSectorBytes;
        memset(p+1,0xff,10);p[15]=1;
        memcpy(p+16,iso.data()+i*sector,sector);
    }
    return raw;
}
Bytes Load(const std::wstring& file)
{
    auto source=HfsPlus::OpenImage(file.c_str(),false);
    return HfsPlus::Disk::Read(*source,0,size_t(source->Size()));
}
void Save(const std::wstring& path,const char* text)
{
    auto file=HfsPlus::CreateFormatOutput(path.c_str(),strlen(text));
    file->Write(0,text,strlen(text));
    file->Keep();
}
void Missing(const std::wstring& path){Check(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES,"A failed optical job published an output");}
void CheckFile(MacBrowser::Volume& volume)
{
    MacBrowser::Entry entry;
    Check(!volume.Writable()&&!strcmp(volume.Kind(),"ISO 9660")&&volume.FindChild(2,u"TEST.TXT",entry),
          "Raw optical image did not mount via the normal browser");
    char buffer[4]{};
    volume.Read(entry.id,0,0,buffer,sizeof(buffer));
    Check(!memcmp(buffer,"data",4),"Cooked file bytes from the raw track were incorrect");
}
void NoTemps(const std::wstring& folder)
{
    WIN32_FIND_DATAW info{};
    auto found=FindFirstFileW((folder+L"\\.hfv-optical-*.tmp").c_str(),&info);
    if(found!=INVALID_HANDLE_VALUE)FindClose(found);
    Check(found==INVALID_HANDLE_VALUE,"Optical job leaked a staged file");
}
}

int RunRawOpticalTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const std::string narrow=directory;
    const auto root=VolumeCoord::Wide(narrow);
    FILE* report=fopen((narrow+"\\raw-optical-report.txt").c_str(),"w");
    if(!report)return 2;
    int status=0;
    try{
        auto disc=std::make_shared<Disc>();
        Track audio{1,1,0,2,false,1},data{2,1,2,32,true,4},other{3,2,12000,1,false,0};
        disc->tracks={audio,data,other};
        auto& music=disc->data[audio.number];
        music.resize(audio.sectors*RawSectorBytes);
        for(size_t at=0;at<music.size();at+=2){music[at]=0x12;music[at+1]=0x34;}
        disc->data[data.number]=IsoRaw();
        disc->data[other.number]=Bytes(RawSectorBytes,0x44);
        const auto audioCdb=RawReadCommand(0x12345678,16,true),dataCdb=RawReadCommand(3,2,false);
        Check(audioCdb[0]==0xbe&&audioCdb[1]==4&&audioCdb[2]==0x12&&audioCdb[5]==0x78&&
              audioCdb[8]==16&&audioCdb[9]==0x10&&dataCdb[1]==0&&dataCdb[9]==0xf8,
              "MMC READ CD request did not select bounded audio/raw data geometry");

        auto wave=Storage::worker().submit(SaveRawCd(3,root+L"\\Music.wav",{RawOutput::WaveTracks,{audio,other},71},disc));
        wave->wait();
        const auto waveResult=wave->result();
        Check(!waveResult.code,waveResult.message.c_str());
        const auto wav=Load(root+L"\\Music-01.wav"),last=Load(root+L"\\Music-03.wav");
        Check(wav.size()==44+music.size()&&last.size()==44+RawSectorBytes&&
              Little(wav.data()+4)==wav.size()-8&&Little(wav.data()+20)==0x00020001&&
              Little(wav.data()+24)==44100&&Little(wav.data()+32)==0x00100004&&
              Little(wav.data()+40)==music.size()&&wav[44]==0x34&&wav[45]==0x12,
              "WAV channel framing, size or audio byte order is incorrect");

        const auto save=[&](const std::wstring& path,RawOutput format,std::vector<Track> tracks){
            auto job=Storage::worker().submit(SaveRawCd(3,path,{format,std::move(tracks),71},disc));
            job->wait();
            return job->result();
        };
        const auto cue=save(root+L"\\Mixed.cue",RawOutput::BinCue,{audio,data});
        Check(!cue.code,cue.message.c_str());
        const auto bytes=Load(root+L"\\Mixed.bin");
        const auto cueBytes=Load(root+L"\\Mixed.cue");
        const std::string sheet(cueBytes.begin(),cueBytes.end());
        Check(sheet.find("FILE \"Mixed.bin\" BINARY")!=sheet.npos&&sheet.find("TRACK 01 AUDIO")!=sheet.npos&&
              sheet.find("FLAGS PRE")!=sheet.npos&&sheet.find("TRACK 02 MODE1/2352")!=sheet.npos&&
              sheet.find("INDEX 01 00:00:02")!=sheet.npos&&
              bytes.size()==music.size()+disc->data[2].size()&&bytes[0]==0x34&&bytes[1]==0x12&&
              std::equal(bytes.begin()+music.size(),bytes.end(),disc->data[2].begin()),
              "BIN/CUE layout, main-channel bytes or audio order is incorrect");
        auto mounted=MacBrowser::OpenBrowserImage(root+L"\\Mixed.cue");
        Check(bool(mounted),"Open the generated mixed-mode BIN/CUE");
        CheckFile(*mounted);
        MacBrowser::Entry selected;
        Check(mounted->FindChild(2,u"TEST.TXT",selected),"Locate CUE browser copy source");
        const auto exportFolder=root+L"\\export";
        Check(CreateDirectoryW(exportFolder.c_str(),nullptr)!=FALSE,"Create CUE export folder");
        FileOperations::Request copy=FileOperations::OrdinaryCopy{
            {FileOperations::Location::Mounted(mounted,selected.id)},FileOperations::Location::Windows(exportFolder)};
        copy.background=true;
        CopyProcess::enabled=true;
        CopyProcess::lastProcess=0;
        auto job=FileOperations::Submit(std::move(copy));
        const auto start=GetTickCount();
        while(!job.Job()->done()){
            Check(GetTickCount()-start<15000,"CUE worker copy timed out");
            Sleep(5);
        }
        const auto result=job.Job()->result();
        CopyProcess::enabled=false;
        Check(!result.code,result.message.c_str());
        Check(CopyProcess::lastProcess&&Load(exportFolder+L"\\TEST.TXT")==Bytes({'d','a','t','a'}),
              "Copy subprocess did not reopen the CUE and BIN together");
        mounted.reset();

        const auto bin=save(root+L"\\Data.bin",RawOutput::RawTrack,{data});
        Check(!bin.code,bin.message.c_str());
        Check(Load(root+L"\\Data.bin")==disc->data[2],"Raw 2352 output changed the sectors");
        auto raw=MacBrowser::OpenImage(root+L"\\Data.bin");
        Check(bool(raw),"Open standalone raw Mode 1 BIN");
        CheckFile(*raw);

        auto mode2=IsoRaw();
        for(size_t at=0;at<mode2.size();at+=RawSectorBytes){
            auto p=mode2.data()+at;
            memmove(p+24,p+16,2048);
            memset(p+16,0,8);
            p[15]=2;
        }
        {
            auto file=HfsPlus::CreateFormatOutput((root+L"\\Mode2.bin").c_str(),mode2.size());
            file->Write(0,mode2.data(),mode2.size());
            file->Keep();
        }
        Save(root+L"\\Mode2.cue","FILE \"Mode2.bin\" MOTOROLA\r\n TRACK 01 MODE2/2352\r\n INDEX 01 00:00:00\r\n");
        auto xaMounted=MacBrowser::OpenImage(root+L"\\Mode2.cue");
        Check(bool(xaMounted),"Open raw Mode 2 Form 1 CUE");
        CheckFile(*xaMounted);

        Track form{4,2,12001,1,true,4};
        disc->tracks.push_back(form);
        auto& form2=disc->data[4];
        form2.resize(RawSectorBytes);
        std::fill_n(form2.begin()+1,10,0xff);
        form2[15]=2;form2[18]=form2[22]=0x20;
        std::fill(form2.begin()+24,form2.begin()+24+Form2Bytes,0x5a);
        const auto xa=save(root+L"\\Form2.bin",RawOutput::Form2Track,{form});
        Check(!xa.code,xa.message.c_str());
        Check(Load(root+L"\\Form2.bin")==Bytes(Form2Bytes,0x5a),"Mode 2 Form 2 user data differed");
        auto invalid=save(root+L"\\WrongMode.bin",RawOutput::Form2Track,{data});
        Check(invalid.code,"Mode 1 was accepted as Mode 2 Form 2");
        Missing(root+L"\\WrongMode.bin");

        Save(root+L"\\existing.cue","KEEP");
        const auto conflict=save(root+L"\\existing.cue",RawOutput::BinCue,{audio,data});
        Check(conflict.code&&Load(root+L"\\existing.cue")==Bytes({'K','E','E','P'}),"An existing CUE was overwritten");
        Missing(root+L"\\existing.bin");
        bool raced=false;
        HfsPlus::workerIoFault=[&](bool writing,uint64_t offset,size_t count){
            if(!writing||offset||count>=512||raced)return;
            auto handle=CreateFileW((root+L"\\raced.cue").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL,nullptr);
            Check(handle!=INVALID_HANDLE_VALUE,"Create competing CUE output");
            DWORD wrote=0;
            const auto ok=WriteFile(handle,"KEEP",4,&wrote,nullptr);
            CloseHandle(handle);
            Check(ok&&wrote==4,"Write competing CUE bytes");
            raced=true;
        };
        const auto collision=save(root+L"\\raced.cue",RawOutput::BinCue,{audio,data});
        HfsPlus::workerIoFault={};
        Check(raced&&collision.code&&Load(root+L"\\raced.cue")==Bytes({'K','E','E','P'}),
              "A late output collision lost another writer's CUE");
        Missing(root+L"\\raced.bin");
        disc->swap=true;
        const auto swap=save(root+L"\\swapped.bin",RawOutput::RawTrack,{data});
        Check(swap.code&&disc->generation!=71,"Media change during raw read was accepted");
        Missing(root+L"\\swapped.bin");
        disc->swap=false;disc->generation=71;disc->shortRead=true;
        const auto shortRead=save(root+L"\\short.bin",RawOutput::RawTrack,{data});
        Check(shortRead.code,"Short raw read was accepted");
        Missing(root+L"\\short.bin");
        disc->shortRead=false;
        Storage::Job cancelled;
        disc->cancel=&cancelled;
        try{(void)SaveRawCd(3,root+L"\\cancelled.bin",{RawOutput::RawTrack,{data},71},disc)(cancelled);
            throw HfsPlus::Error("Cancelled raw read returned success");}
        catch(const Storage::Cancelled&){}
        disc->cancel=nullptr;
        Missing(root+L"\\cancelled.bin");
        NoTemps(root);
        Check(disc->opens==disc->closes,"Raw jobs did not release the source");

        const auto rejectCue=[&](const std::wstring& filename,const char* contents){
            Save(filename,contents);
            try{MacBrowser::OpenImage(filename);}
            catch(const HfsPlus::Error&){return;}
            throw HfsPlus::Error("Invalid CUE was accepted");
        };
        rejectCue(root+L"\\path.cue","FILE \"..\\Data.bin\" BINARY\r\n TRACK 01 MODE1/2352\r\n INDEX 01 00:00:00\r\n");
        rejectCue(root+L"\\bounds.cue","FILE \"Data.bin\" BINARY\r\n TRACK 01 MODE1/2352\r\n INDEX 01 00:02:00\r\n");
        fprintf(report,"PASS: WAV PCM naming and byte order, mixed-mode BIN/CUE round-trip, raw 2352 export, Mode 2 Form 2 2324 export, and bounded MMC read requests.\n");
        fprintf(report,"PASS: short reads, cancellation, media swaps, existing outputs and malformed or escaping CUE files. No optical hardware or computer control.\n");
    }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());status=1;}
    HfsPlus::workerIoFault={};
    CopyProcess::enabled=false;
    fclose(report);
    return status;
}
#endif
