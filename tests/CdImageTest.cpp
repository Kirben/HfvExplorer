// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "CdImage.h"
#include "DeviceWait.h"
#include "CdImageToc.h"
#include "HfsPlusCd.h"
#include "HfsPlusDisk.h"
#include "VolumeCoordinator.h"
#include <map>
#include "StorageJobDialog.h"
#include "BackgroundCopies.h"
#include <vector>
#include <stdio.h>
#include <stdexcept>
extern bool ProbeCdImageLayout();
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
class FakeCdImage : public CdImageSource {
public:
 std::vector<BYTE> data;DWORD error=0,generation=7;unsigned reads=0,checks=0,opens=0,changeAtRead=0,changeAtCheck=0;
 DWORD readError=0,openError=0,delay=0;bool shortRead=false;std::function<void()> onRead;
 FakeCdImage():data(3*65536+2048){for(size_t n=0;n<data.size();++n)data[n]=BYTE(n*71+n/2048);data[0]='E';data[1]='R';data[512]='P';data[513]='M';}
 bool Open(int,std::string& why) override {++opens;if(openError){error=openError;why="Injected unsupported or unavailable CD";return false;}return true;}
 unsigned long long Size() const override{return data.size();}
 bool Verify(DWORD& value) override {++checks;if(changeAtCheck==checks)++generation;value=generation;return true;}
 bool Read(unsigned long long offset,DWORD count,void* buffer,DWORD& got) override {
  ++reads;if(delay)Sleep(delay);got=0;
  if(onRead)onRead();if(readError){error=readError;return false;}
  if(offset>data.size()||count>data.size()-offset){error=ERROR_HANDLE_EOF;return false;}
  got=shortRead?count-1:count;memcpy(buffer,data.data()+size_t(offset),got);if(changeAtRead==reads)++generation;return true;
 }
 DWORD Error() const override{return error;}
};
void save(const std::string& path,const std::vector<BYTE>& data){CFile file;check(file.Open(path.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create host fixture");file.Write(data.data(),UINT(data.size()));}
std::vector<BYTE> load(const std::string& path){CFile file;check(file.Open(path.c_str(),CFile::modeRead)!=FALSE,"open image");std::vector<BYTE> data(size_t(file.GetLength()));check(file.Read(data.data(),UINT(data.size()))==data.size(),"read image");return data;}
void noTemporary(const std::string& root){WIN32_FIND_DATAA found;HANDLE h=FindFirstFileA((root+"\\hcd*.tmp").c_str(),&found);if(h!=INVALID_HANDLE_VALUE)FindClose(h);check(h==INVALID_HANDLE_VALUE,"partial temporary image leaked");}
}
int RunCdImageTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory;
 FILE* report=nullptr;if(fopen_s(&report,(root+"\\cd-image-report.txt").c_str(),"w")||!report)return 2;
 try {
  check(ProbeCdImageLayout(),"CD format choices and audio-only filename note");
  CDROM_TOC toc={};toc.FirstTrack=toc.LastTrack=1;toc.TrackData[0].TrackNumber=1;toc.TrackData[0].Control=4;toc.TrackData[1].TrackNumber=0xaa;toc.TrackData[1].Address[2]=1;
  check(CdImageSupportedToc(toc,sizeof(toc))&&CdImageTocBytes(toc)==256*2048,"single data track and big-endian lead-out length");
  auto invalid=toc;invalid.TrackData[0].Control=0;check(!CdImageSupportedToc(invalid,sizeof(invalid)),"audio CD rejected");
  invalid=toc;invalid.LastTrack=2;check(!CdImageSupportedToc(invalid,sizeof(invalid)),"incomplete multitrack TOC rejected");
  invalid=toc;invalid.TrackData[0].Address[3]=1;check(CdImageSupportedToc(invalid,sizeof(invalid))&&CdImageTocBytes(invalid)==255*2048,"nonzero first-track origin accepted without including its prefix");
  invalid=toc;invalid.TrackData[1].TrackNumber=2;check(!CdImageSupportedToc(invalid,sizeof(invalid))&&!CdImageSupportedToc(toc,4),"missing lead-out and truncated TOCs rejected");
  toc.TrackData[1].Address[0]=1;check(CdImageTocBytes(toc)>0xffffffffULL,"TOC length does not truncate above 4 GiB");
  auto run=[&](const std::string& path,std::shared_ptr<FakeCdImage> source,Storage::ConflictChoice choice=Storage::ConflictChoice::Replace){
   auto job=Storage::worker().submit(MakeCdImageJob(3,path,source));DWORD start=GetTickCount();
   while(!job->done()){if(!job->conflict().empty())job->resolve(choice);if(GetTickCount()-start>10000){job->cancel();throw std::runtime_error("CD image job timed out");}Sleep(1);}
   noTemporary(root);return job;
  };
  std::string image=root+"\\disc.iso";auto source=std::make_shared<FakeCdImage>();
  auto success=run(image,source);
  check(success->result().code==0&&load(image)==source->data&&source->reads==4&&source->checks==10,"dump preserves all sectors/partition-map bytes with before/after/final generation checks");
  check(success->progress().processed==source->data.size()&&success->progress().totalBytes==source->data.size()&&success->progress().byteTransfer,"whole-image byte progress reaches exact length");
  const std::vector<BYTE> keep={'K','E','E','P'};
  for(int test=0;test<5;++test){
   save(image,keep);auto bad=std::make_shared<FakeCdImage>();
   if(test==0)bad->readError=ERROR_CRC;if(test==1)bad->shortRead=true;if(test==2)bad->changeAtRead=2;if(test==3)bad->changeAtCheck=10;if(test==4)bad->openError=ERROR_NOT_SUPPORTED;
   auto failed=run(image,bad);check(failed->result().code!=0&&!failed->result().message.empty()&&load(image)==keep,"read errors, short reads, media changes and unsupported media preserve existing image");
   if(test==0)check(failed->result().error==ERROR_CRC&&failed->result().message.find("byte offset 0")!=std::string::npos,"read failure records offset and error");
   if(test==2||test==3)check(failed->result().error==ERROR_MEDIA_CHANGED,"media changes are not mistaken for success");
  }
  for(auto choice:{Storage::ConflictChoice::Skip,Storage::ConflictChoice::Cancel}){
   auto untouched=std::make_shared<FakeCdImage>();auto job=run(image,untouched,choice);
   check(untouched->opens==0&&load(image)==keep,"skip/cancel preserve destination without opening CD");
   check(choice==Storage::ConflictChoice::Skip?job->result().code==0:job->result().error==ERROR_CANCELLED,"overwrite choice delivered");
  }
  auto replaced=std::make_shared<FakeCdImage>();check(run(image,replaced)->result().code==0&&load(image)==replaced->data,"explicit replacement publishes complete image");
  std::string race=root+"\\race.iso";auto racing=std::make_shared<FakeCdImage>();racing->onRead=[&]{if(racing->reads==1)save(race,keep);};
  check(run(race,racing)->result().code!=0&&load(race)==keep,"newly appearing destination is not overwritten");racing->onRead=nullptr;
  auto missing=std::make_shared<FakeCdImage>();check(run(root+"\\missing\\disc.iso",missing)->result().code!=0,"destination creation errors reported");
  auto cancelling=std::make_shared<FakeCdImage>();
  auto cancelled=Storage::worker().submit([&](Storage::Job& job){cancelling->onRead=[&job]{job.cancel();};return MakeCdImageJob(3,root+"\\cancelled.iso",cancelling)(job);});cancelled->wait();cancelling->onRead=nullptr;
  check(cancelled->result().error==ERROR_CANCELLED&&GetFileAttributesA((root+"\\cancelled.iso").c_str())==INVALID_FILE_ATTRIBUTES,"mid-read cancellation discards partial image");noTemporary(root);
  CWnd owner;CString ownerClass=AfxRegisterWndClass(0);check(owner.CreateEx(0,ownerClass,"Hidden CD image owner",WS_OVERLAPPED,0,0,100,100,nullptr,0)!=FALSE,"create hidden image owner");
  AfxGetApp()->m_pMainWnd=&owner;InitializeStorageUi();storageUiTest={};
  auto slow=std::make_shared<FakeCdImage>();slow->delay=80;
  auto ui=ShowStorageJob(MakeCdImageJob(3,root+"\\ui.iso",slow),"Hidden CD image",true,true,true);
  check(ui.code==0&&storageUiTest.dialogCreations==1&&storageUiTest.copyBarCount==1&&storageUiTest.timerTicks>=2&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"slow image dump uses one responsive progress dialog and restores owner");
  storageUiTest={};storageUiTest.cancelOnTimer=true;auto uiCancelSource=std::make_shared<FakeCdImage>();uiCancelSource->delay=100;
  auto uiCancel=ShowStorageJob(MakeCdImageJob(3,root+"\\ui-cancel.iso",uiCancelSource),"Hidden cancel CD image",true,true,true);
  check(uiCancel.error==ERROR_CANCELLED&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled()&&GetFileAttributesA((root+"\\ui-cancel.iso").c_str())==INVALID_FILE_ATTRIBUTES,"Cancel button cancels between reads, removes partial image and restores owner");noTemporary(root);
  storageUiTest={};storageUiTest.cancelOnTimer=true;
  auto background=std::make_shared<FakeCdImage>();background->delay=100;
  const auto backgroundPath=root+"\\background-cancel.iso";
  check(BackgroundCopies::StartStorage(MakeCdImageJob(3,backgroundPath,background),&owner,
       "Hidden background image","CD image dump",{},{true}),"start modeless CD dump");
  check(BackgroundCopies::Active()&&owner.IsWindowEnabled(),"CD dump disabled its browser");
  const auto progress=BackgroundCopies::DialogWindow();
  const auto started=GetTickCount();
  while(BackgroundCopies::Active()&&GetTickCount()-started<5000){
   MSG message{};
   while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT)DispatchMessage(&message);}
   Sleep(1);
  }
  check(!BackgroundCopies::Active()&&BackgroundCopies::LastResult().error==ERROR_CANCELLED&&
        !storageUiTest.disabledDuring&&owner.IsWindowEnabled()&&
        VolumeCoord::Available({{VolumeCoord::DeviceKey(3),VolumeCoord::Mode::Write}})&&
        GetFileAttributesA(backgroundPath.c_str())==INVALID_FILE_ATTRIBUTES,
        "modeless CD dump cancellation left an image, lock or disabled browser");
  if(IsWindow(progress))SendMessage(progress,WM_CLOSE,0,0);
  noTemporary(root);
  owner.DestroyWindow();AfxGetApp()->m_pMainWnd=nullptr;
  fprintf(report,"PASS: complete-sector CD image dump, partition-map byte fidelity, exact progress, read/short-read errors, media changes including final check, unsupported media, overwrite/skip/cancel, replacement publication, destination race/errors, partial cleanup, slow responsive dialog, Cancel button and pre-close owner restoration; all CD sources simulated.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());fclose(report);return 1;}catch(CException* error){char text[512];error->GetErrorMessage(text,sizeof(text));fprintf(report,"FAIL: %s\n",text);error->Delete();fclose(report);return 1;}
 fclose(report);return 0;
}

namespace {
struct OpticalMemory:HfsPlus::Source{
 HfsPlus::Bytes bytes;explicit OpticalMemory(size_t n):bytes(n,0){}
 uint64_t Size()const override{return bytes.size();}bool Writable()const override{return true;}
 void Read(uint64_t at,void* p,size_t n)override{check(HfsPlus::Disk::Range(at,n,bytes.size()),"optical memory read");memcpy(p,bytes.data()+at,n);}
 void Write(uint64_t at,const void* p,size_t n)override{check(HfsPlus::Disk::Range(at,n,bytes.size()),"optical memory write");memcpy(bytes.data()+at,p,n);}void Flush()override{}
};
HfsPlus::Bytes opticalVolume(const std::u16string& name,HfsPlus::Volume::FormatKind kind){OpticalMemory memory(8*1024*1024);HfsPlus::Volume::Format(memory,name,true,kind);HfsPlus::Volume v(memory,true);v.Put(2,u"Both forks",8193,1025,[](unsigned f,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<BYTE*>(p)[i]=BYTE((at+i)*37+f*71);});return memory.bytes;}
struct TrackDisc:MediaImageSource{
 std::vector<CdMedia::Track> layout;std::map<unsigned,std::vector<BYTE>> content;std::vector<unsigned> readTracks;
 unsigned selected=0,opens=0,closes=0;DWORD generation=7,error=0,delay=0;bool shortRead=false,changeSelect=false,workerOnly=false,wrongThread=false;std::function<void()> onRead;
 void Thread(){if(workerOnly&&GetCurrentThreadId()==StorageUi::state().thread)wrongThread=true;}
 bool Open(int,std::string& reason)override{Thread();++opens;auto tracks=DataTracks();if(tracks.empty()){error=ERROR_NOT_SUPPORTED;reason="No data tracks";return false;}selected=tracks.back().number;return true;}
 void Close()override{++closes;}
 std::vector<CdMedia::Track> DataTracks()const override{return CdMedia::DataTracks(layout);}
 bool SelectTrack(unsigned track,std::string& reason)override{Thread();for(auto& t:layout)if(t.number==track&&t.data&&content.count(track)){selected=track;if(changeSelect)++generation;return true;}error=ERROR_NOT_SUPPORTED;reason="No such data track";return false;}
 unsigned long long Size()const override{auto i=content.find(selected);return i==content.end()?0:i->second.size();}
 bool Verify(DWORD& value)override{Thread();value=generation;return true;}
 bool Read(unsigned long long at,DWORD n,void* p,DWORD& got)override{Thread();if(delay)Sleep(delay);readTracks.push_back(selected);got=0;if(!content.count(selected)||at>content.at(selected).size()||n>content.at(selected).size()-at){error=ERROR_HANDLE_EOF;return false;}got=shortRead?n-1:n;memcpy(p,content.at(selected).data()+at,got);if(onRead)onRead();return true;}
 DWORD Error()const override{return error;}
 void Add(unsigned number,unsigned session,uint64_t start,std::vector<BYTE> bytes,bool data=true){CdMedia::Track t;t.number=number;t.session=session;t.start=start;t.sectors=bytes.size()/2048;t.data=data;layout.push_back(t);if(data)content[number]=std::move(bytes);}
};
std::vector<BYTE> fullToc(){
 std::vector<BYTE> bytes(4,0);bytes[2]=1;bytes[3]=2;
 auto add=[&](BYTE session,BYTE point,uint64_t value,bool data,bool trackField=false){CDROM_TOC_FULL_TOC_DATA_BLOCK d={};d.SessionNumber=session;d.Adr=1;d.Control=data?4:0;d.Point=point;if(trackField)d.Msf[0]=BYTE(value);else{value+=150;d.Msf[0]=BYTE(value/(60*75));d.Msf[1]=BYTE(value/75%60);d.Msf[2]=BYTE(value%75);}auto p=reinterpret_cast<const BYTE*>(&d);bytes.insert(bytes.end(),p,p+sizeof(d));};
 add(1,0xa0,1,false,true);add(1,0xa1,2,false,true);add(1,0xa2,5000,false);add(1,1,0,false);add(1,2,1000,true);
 add(2,0xa0,3,false,true);add(2,0xa1,4,false,true);add(2,0xa2,9500,false);add(2,3,7000,true);add(2,4,9000,false);
 unsigned length=unsigned(bytes.size()-2);bytes[0]=BYTE(length>>8);bytes[1]=BYTE(length);return bytes;
}
void opticalLayoutTests(){
 auto bytes=fullToc();std::vector<CdMedia::Track> tracks;std::string error;
 check(CdMedia::Full(bytes.data(),DWORD(bytes.size()),tracks,error)&&tracks.size()==4,"parse mixed/multisession FULL TOC");auto data=CdMedia::DataTracks(tracks);check(data.size()==2&&data[0].number==2&&data[0].session==1&&data[0].start==1000&&data[0].sectors==4000&&data[1].number==3&&data[1].session==2&&data[1].start==7000&&data[1].sectors==2000,"data-track bounds include audio or session gaps");
 for(unsigned mode=0;mode<5;++mode){auto bad=bytes;if(mode==0)bad.pop_back();if(mode==1)bad[4+2*11+3]=0xa3;if(mode==2)bad[4+2*11+9]=60;if(mode==3)bad[4+4*11+3]=1;if(mode==4)bad[4+7*11+10]=75;check(!CdMedia::Full(bad.data(),DWORD(bad.size()),tracks,error),"malformed/truncated/duplicate session TOC accepted");}
 // Verify the actual FULL TOC request uses MSF as required by Windows.
 {DWORD code=0;unsigned calls=0;auto query=[&](CDROM_READ_TOC_EX& request,void* out,DWORD size,DWORD& returned,DWORD& error){++calls;check(request.Format==CDROM_READ_TOC_EX_FORMAT_FULL_TOC&&request.Msf==1&&request.SessionTrack==1,"invalid native FULL TOC request");check(size>=bytes.size(),"full TOC buffer");memcpy(out,bytes.data(),bytes.size());returned=DWORD(bytes.size());error=0;return true;};check(CdMedia::ReadLayout(query,tracks,code,error)&&calls==1&&tracks.size()==4,"native full TOC query failed");}
 {DWORD code=0;unsigned calls=0;auto query=[&](CDROM_READ_TOC_EX&,void*,DWORD,DWORD& returned,DWORD& error){++calls;returned=0;error=ERROR_CRC;return false;};check(!CdMedia::ReadLayout(query,tracks,code,error)&&code==ERROR_CRC&&calls==1,"TOC read error hidden by fallback");}
 // Exercise native-query fallbacks with fake IOCTL output, never a device.
 CDROM_TOC basic={};basic.FirstTrack=1;basic.LastTrack=4;uint32_t starts[]={0,1000,7000,9000,9500};
 for(unsigned i=0;i<5;++i){basic.TrackData[i].TrackNumber=i==4?0xaa:BYTE(i+1);basic.TrackData[i].Control=i==1||i==2?4:0;HfsPlus::Disk::P32(basic.TrackData[i].Address,starts[i]);}
 for(unsigned mode=0;mode<3;++mode){DWORD code=0;unsigned calls=0;auto query=[&](CDROM_READ_TOC_EX& request,void* out,DWORD n,DWORD& returned,DWORD& fault){++calls;returned=0;fault=ERROR_NOT_SUPPORTED;
   if(request.Format==CDROM_READ_TOC_EX_FORMAT_FULL_TOC)return false;
   if(request.Format==CDROM_READ_TOC_EX_FORMAT_TOC){check(n>=sizeof(basic),"TOC buffer");memcpy(out,&basic,sizeof(basic));returned=sizeof(basic);fault=0;return true;}
   if(mode==2)return false;CDROM_TOC_SESSION_DATA session={};session.FirstCompleteSession=1;session.LastCompleteSession=mode?1:2;session.TrackData[0]=basic.TrackData[mode?0:2];memcpy(out,&session,sizeof(session));returned=sizeof(session);fault=0;return true;};
  bool ok=CdMedia::ReadLayout(query,tracks,code,error);check(calls==3,"fallback did not query complete-session information");if(mode==0)check(ok&&tracks.size()==2&&tracks.front().number==3&&tracks.front().session==2&&tracks.front().sectors==2000,"fallback guessed an earlier session lead-out");if(mode==1)check(ok&&tracks.size()==4,"single-session mixed mode fallback failed");if(mode==2)check(!ok&&code==ERROR_NOT_SUPPORTED,"unknown multisession boundaries accepted");}
}
}
extern bool TestCdTrackPicker(const std::vector<CdMedia::Track>&);
int RunOpticalDataTrackTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory;FILE* report=fopen((root+"\\optical-data-tracks-report.txt").c_str(),"w");if(!report)return 2;
 try{
  opticalLayoutTests();auto old=opticalVolume(u"Older",HfsPlus::Volume::FormatKind::HfsPlus),latest=opticalVolume(u"Newest",HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
  auto disc=[&](bool absolute,bool isoLatest=false){auto media=std::make_shared<TrackDisc>();media->Add(1,1,0,old);media->Add(2,1,4096,std::vector<BYTE>(200*2048),false);
   if(isoLatest)media->Add(3,2,7000,std::vector<BYTE>(4096));
   else if(!absolute)media->Add(3,2,7000,latest);
   else {std::vector<BYTE> bytes(65536+latest.size(),0);memcpy(bytes.data()+65536,latest.data(),latest.size());HfsPlus::Disk::P16(bytes.data(),0x4552);HfsPlus::Disk::P16(bytes.data()+2,512);auto p=bytes.data()+512;HfsPlus::Disk::P16(p,0x504d);HfsPlus::Disk::P32(p+4,1);HfsPlus::Disk::P32(p+8,7000*4+128);HfsPlus::Disk::P32(p+12,DWORD(latest.size()/512));memcpy(p+48,"Apple_HFSX",11);media->Add(3,2,7000,std::move(bytes));}return media;};
  for(bool absolute:{false,true}){auto media=disc(absolute);auto source=HfsPlus::OpenCd(3,media);HfsPlus::Volume volume(*source,false);check(volume.Name()==u"Newest"&&volume.IsHfsX(),"newest supported HFSX data session was not selected");auto list=volume.List();auto file=std::find_if(list.begin(),list.end(),[](const HfsPlus::Entry& e){return e.name==u"Both forks";});check(file!=list.end(),"optical session payload missing");for(unsigned fork=0;fork<2;++fork){std::vector<BYTE> bytes(size_t(file->forks[fork].size));check(volume.Read(file->id,fork,0,bytes.data(),bytes.size())==bytes.size(),"multisession fork read length");for(size_t i=0;i<bytes.size();++i)check(bytes[i]==BYTE(i*37+fork*71),"multisession fork data changed");}check(std::find(media->readTracks.begin(),media->readTracks.end(),2)==media->readTracks.end(),"audio track read during Mac detection");}
  {auto media=disc(false,true);auto source=HfsPlus::OpenCd(3,media);HfsPlus::Volume volume(*source);check(volume.Name()==u"Older","ISO-only latest session hid earlier supported Mac data");}
  {auto media=disc(false);media->content[3].assign(4096,0);auto h=media->content[3].data()+1024;HfsPlus::Disk::P16(h,0x4244);HfsPlus::Disk::P16(h+18,1);HfsPlus::Disk::P32(h+20,512);h[36]=7;memcpy(h+37,"Classic",7);media->layout.back().sectors=2;auto source=HfsPlus::OpenCd(3,media);BYTE signature[2];source->Read(1024,signature,2);check(signature[0]==0x42&&signature[1]==0x44&&!HfsPlus::Volume::Recognizes(*source),"earlier HFS+ incorrectly displaced a newer classic HFS session");}
  fprintf(report,"PASS: strict full/basic session TOCs, mixed-mode data selection, nonzero origins, excluded audio/session gaps, conservative last-session fallback, newest HFS+/HFSX selection, ISO-only fallback and newer classic precedence; absolute APM partition offsets mount without rewriting sectors.\n");
  auto run=[&](std::shared_ptr<TrackDisc> media,const std::string& name,CdImageSelection selection){auto job=Storage::worker().submit(MakeCdImageJob(3,root+"\\"+name,media,selection));DWORD end=GetTickCount()+20000;while(!job->done()){if(!job->conflict().empty())job->resolve(Storage::ConflictChoice::Replace);if(LONG(end-GetTickCount())<0){job->cancel();throw std::runtime_error("track dump timeout");}Sleep(1);}noTemporary(root);return job;};
  CdImageSelection selected;selected.track=3;selected.identityKnown=true;selected.generation=7;selected.expected=disc(true)->layout.back();
  auto raw=disc(true);check(!run(raw,"data-track.iso",selected)->result().code&&load(root+"\\data-track.iso")==raw->content[3],"selected track dump lost sectors/absolute APM bytes");
  selected.macVolumeOnly=true;auto volume=disc(true);check(!run(volume,"mac-volume.dsk",selected)->result().code&&load(root+"\\mac-volume.dsk")==latest,"portable Mac-volume extraction changed filesystem bytes");{auto image=HfsPlus::OpenImage(VolumeCoord::Wide(root+"\\mac-volume.dsk").c_str(),false);HfsPlus::Volume v(*image);check(v.Name()==u"Newest","extracted absolute-session Mac image cannot reopen");}
  const std::vector<BYTE> keep={'K','E','E','P'};
  for(unsigned mode=0;mode<5;++mode){save(root+"\\kept.iso",keep);auto media=disc(true);auto choice=selected;if(mode==0)media->generation=8;if(mode==1)++choice.expected.sectors;if(mode==2){choice.track=2;choice.identityKnown=false;}if(mode==3)media->shortRead=true;if(mode==4)media->changeSelect=true;auto failed=run(media,"kept.iso",choice);check(failed->result().code&&load(root+"\\kept.iso")==keep,"track selection/media/read failure replaced destination");}
  {auto media=disc(true);auto result=std::make_shared<std::vector<CdMedia::Track>>();auto generation=std::make_shared<DWORD>();auto job=Storage::worker().submit(MakeCdTrackInspectionJob(3,media,result,generation));job->wait();check(!job->result().code&&result->size()==2&&*generation==7&&media->closes==1,"track inspection did not capture data-only snapshot/release media");check(TestCdTrackPicker(*result),"track-picker selection or portable-image option failed");}
  {auto media=disc(true);media->changeSelect=true;bool failed=false;try{HfsPlus::OpenCd(3,media);}catch(const HfsPlus::Error&){failed=true;}check(failed,"media change during track selection was accepted");}
  fprintf(report,"PASS: exact selected-track images and portable Mac-volume extraction/reopen, origin/length/media snapshot validation, audio rejection, short-read preservation, track inspection and actual hidden track-picker controls. No physical media or computer control.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());fclose(report);return 1;}
 fclose(report);return 0;
}
#endif
