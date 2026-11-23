// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CommandStorage.h"
#include "CdImage.h"
#include "HfsPlus.h"
#include "BrowserPresentation.h"
#include "MacBrowserModel.h"
#include "WindowsMoveUndo.h"
#include "DeviceWait.h"
#include "ReadOnlyCd.h"
#include "CdImageToc.h"
#include "CdMacProbe.h"
#include <vector>
namespace {
class NativeCdImageSource : public CdImageSource {
 ReadOnlyCd cd_;DWORD error_=0,identity_=0;bool ready_=false;unsigned long long size_=0;
public:
 bool Open(int drive,std::string& reason) override {
  ready_=false;size_=0;error_=0;
  if(!cd_.OpenDrive(drive)){error_=cd_.Error();reason="Could not open the data CD";return false;}
  DWORD before=0,after=0;
  if(!Verify(before)){reason="Could not establish CD media identity";return false;}
  auto tracks=DataTracks();if(tracks.empty()){error_=ERROR_NOT_SUPPORTED;reason="The disc has no supported cooked data tracks.";return false;}
  if(!SelectTrack(tracks.back().number,reason))return false;
  if(!Verify(after)){reason="CD media check failed while reading the table of contents";return false;}
  if(before!=after){error_=ERROR_MEDIA_CHANGED;reason="CD media changed while opening";return false;}
  identity_=before;ready_=true;
  return true;
 }
 void Close()override{ready_=false;cd_.Close();}
 std::vector<CdMedia::Track> DataTracks()const override{return CdMedia::DataTracks(cd_.Tracks());}
 bool SelectTrack(unsigned number,std::string& reason)override{
  if(!cd_.SelectDataTrack(number)){error_=cd_.Error();reason="Cannot select the requested CD data track";return false;}
  size_=cd_.ReadEnd()-cd_.ReadBegin();return true;
 }
 unsigned long long Size() const override{return size_;}
 bool Verify(DWORD& generation) override {
  DWORD returned=0;generation=0;
  if(!HfvMediaControl(cd_.Handle(),IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&generation,sizeof(generation),&returned,nullptr)){error_=GetLastError();return false;}
  if(returned<sizeof(generation)){error_=ERROR_NOT_SUPPORTED;return false;}
  if(ready_&&generation!=identity_){error_=ERROR_MEDIA_CHANGED;return false;}return true;
 }
 bool Read(unsigned long long offset,DWORD count,void* buffer,DWORD& got) override {
  got=0;if(offset>size_||count>size_-offset){error_=ERROR_HANDLE_EOF;return false;}if(!cd_.ReadBytes(cd_.ReadBegin()+offset,count,buffer)){error_=cd_.Error();return false;}got=count;return true;
 }
 DWORD Error() const override{return error_;}
};

class SelectedCdImageSource:public CdImageSource{
 std::shared_ptr<CdImageSource> source_;CdImageSelection selection_;DWORD error_=0;ULONGLONG base_=0,size_=0;
public:
 SelectedCdImageSource(std::shared_ptr<CdImageSource> s,CdImageSelection selection):source_(std::move(s)),selection_(selection){}
 bool Open(int drive,std::string& reason)override{
  if(!source_->Open(drive,reason)){error_=source_->Error();return false;}
  if(selection_.identityKnown){DWORD generation;if(!source_->Verify(generation)||generation!=selection_.generation){error_=ERROR_MEDIA_CHANGED;reason="CD media changed after track selection";return false;}}
  if(selection_.track){auto tracks=source_->DataTracks();auto found=std::find_if(tracks.begin(),tracks.end(),[&](const CdMedia::Track& t){return t.number==selection_.track;});
   if(found==tracks.end()||(selection_.identityKnown&&!(*found==selection_.expected))){error_=selection_.identityKnown?ERROR_MEDIA_CHANGED:ERROR_NOT_SUPPORTED;reason="The selected data track or session layout changed";return false;}
   if(!source_->SelectTrack(selection_.track,reason)){error_=source_->Error();return false;}}
  if(selection_.identityKnown){DWORD generation=0;if(!source_->Verify(generation)||generation!=selection_.generation){error_=ERROR_MEDIA_CHANGED;reason="CD media changed while selecting the data track";return false;}}
  base_=0;size_=source_->Size();if(!size_||size_%2048){error_=ERROR_INVALID_DATA;reason="Invalid CD data-track length";return false;}
  if(selection_.macVolumeOnly){auto tracks=source_->DataTracks();auto chosen=std::find_if(tracks.begin(),tracks.end(),[&](const CdMedia::Track& t){return t.number==selection_.track;});auto origin=chosen==tracks.end()?0:chosen->start*2048;
   DWORD identity=0;if(!source_->Verify(identity)){error_=source_->Error();reason="Cannot verify the Mac-volume source";return false;}
   CdMedia::MacVolume volume;bool ioFailure=false;auto read=[&](ULONGLONG at,DWORD n,void* p){if(ioFailure)return false;DWORD before=0,after=0,got=0;if(!source_->Verify(before)||before!=identity){error_=ERROR_MEDIA_CHANGED;ioFailure=true;return false;}if(!source_->Read(at,n,p,got)||got!=n){error_=source_->Error()?source_->Error():ERROR_READ_FAULT;ioFailure=true;return false;}if(!source_->Verify(after)||after!=identity){error_=ERROR_MEDIA_CHANGED;ioFailure=true;return false;}return true;};
   if(!CdMedia::ProbeMac(size_,origin,read,volume)||ioFailure){if(!error_)error_=ERROR_UNRECOGNIZED_VOLUME;reason="No readable Mac filesystem was found in the selected data track";return false;}base_=volume.offset;size_=volume.bytes;
  }
  return true;
 }
 void Close()override{source_->Close();}
 unsigned long long Size()const override{return size_;}
 bool Verify(DWORD& value)override{if(!source_->Verify(value)){error_=source_->Error();return false;}return true;}
 bool Read(unsigned long long at,DWORD n,void* p,DWORD& got)override{if(at>size_||n>size_-at){error_=ERROR_HANDLE_EOF;got=0;return false;}if(!source_->Read(base_+at,n,p,got)){error_=source_->Error();return false;}return true;}
 DWORD Error()const override{return error_?error_:source_->Error();}
};
struct DestinationGuard {
 HANDLE handle=INVALID_HANDLE_VALUE;
 ~DestinationGuard(){Close();}
 void Close(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(std::exchange(handle,INVALID_HANDLE_VALUE));}
};
Storage::Result failure(DWORD error,const std::string& reason,unsigned long long offset=0) {
 if(!error)error=ERROR_IO_DEVICE;
 return {1,error,reason+" (Windows error "+std::to_string(error)+", byte offset "+std::to_string(offset)+")."};
}
}
std::function<Storage::Result(Storage::Job&)> MakeMediaImageJob(int drive,std::string output,std::shared_ptr<MediaImageSource> source,std::string medium) {
 auto work=[=](Storage::Job& job)->Storage::Result {
  if(drive<0||drive>25)return failure(ERROR_INVALID_PARAMETER,"Invalid source drive");
  char destination[MAX_PATH];DWORD length=GetFullPathNameA(output.c_str(),MAX_PATH,destination,nullptr);
  if(!length||length>=MAX_PATH)return failure(ERROR_FILENAME_EXCED_RANGE,"The output path is too long or invalid");
#ifndef HFV_TESTING
  char sourceRoot[]={char('A'+drive),':','\\',0};
  char sourceVolume[MAX_PATH]={},destinationVolume[MAX_PATH]={};
  // Resolve mount points too: saving onto the source would modify it while
  // the imaging operation promises read-only source access.
  if(GetVolumeNameForVolumeMountPointA(sourceRoot,sourceVolume,MAX_PATH)){
   char volumePath[MAX_PATH]={};
   if(GetVolumePathNameA(destination,volumePath,MAX_PATH)&&GetVolumeNameForVolumeMountPointA(volumePath,destinationVolume,MAX_PATH)&&!_stricmp(sourceVolume,destinationVolume))
    return failure(ERROR_INVALID_PARAMETER,"Save the image on a different drive from the source");
  }
#endif
  if(destination[1]==':' && (destination[0]==char('A'+drive)||destination[0]==char('a'+drive)))
   return failure(ERROR_INVALID_PARAMETER,"Save the image on a different drive from the source");
  const auto outputPath=BrowserPresentation::WindowsPath(VolumeCoord::Wide(destination));
  const auto slash=outputPath.find_last_of(L'\\');
  const auto folder=outputPath.substr(0,slash==6?7:slash);
  const bool durable=WindowsMoveUndo::Supported(folder);
  if(durable)WindowsMoveUndo::RecoverExports(folder);
  const bool replace=GetFileAttributesW(outputPath.c_str())!=INVALID_FILE_ATTRIBUTES;
  DestinationGuard original;
  MacBrowser::ObjectIdentity approved;
  if(replace){
   if(!durable)return failure(ERROR_NOT_SUPPORTED,"Replacing an image requires a local NTFS folder. Choose a new filename.");
   // Bind approval to this file and prevent competing writes or renames during the dump.
   original.handle=CreateFileW(outputPath.c_str(),FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ,
       nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
   if(original.handle==INVALID_HANDLE_VALUE)return failure(GetLastError(),"Cannot protect the current destination");
   BY_HANDLE_FILE_INFORMATION info{};
   if(!GetFileInformationByHandle(original.handle,&info)||
      (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
    return failure(ERROR_ACCESS_DENIED,"Choose an ordinary output file");
   approved=MacBrowser::WindowsIdentity(info);
   auto choice=job.replace(destination);
   if(choice==Storage::ConflictChoice::Skip)return {};
   if(choice==Storage::ConflictChoice::Cancel)throw Storage::Cancelled();
  }
  if(job.cancelled())throw Storage::Cancelled();
  if(!source)return failure(ERROR_INVALID_PARAMETER,"No image source was supplied");
  struct CloseSource{MediaImageSource* source;~CloseSource(){source->Close();}}close{source.get()};
  Storage::Progress p;p.path=destination;p.phase="Opening "+medium;p.byteTransfer=true;job.progress(p);
  std::string reason;
  if(!source->Open(drive,reason))return failure(source->Error(),reason);
  auto size=source->Size();
  if(!size||size%512||size>0x7fffffffffffffffULL)return failure(ERROR_INVALID_DATA,"The drive reported an invalid data-image length");
  DWORD generation=0,current=0;
  if(!source->Verify(generation))return failure(source->Error(),"Could not establish "+medium+" media identity");
  auto file=HfsPlus::CreateStagedOutput(outputPath,size,replace);
  std::vector<BYTE> buffer(64*1024);p.scanning=false;p.totalBytes=size;p.phase="Reading "+medium+" image";job.progress(p);
  for(unsigned long long offset=0;offset<size;){
   if(job.cancelled())throw Storage::Cancelled();
   if(!source->Verify(current))return failure(source->Error(),medium+" media check failed",offset);
   if(current!=generation)return failure(ERROR_MEDIA_CHANGED,medium+" media changed; the image was discarded",offset);
   DWORD count=DWORD(min(size-offset,(unsigned long long)buffer.size())),got=0;
   if(!source->Read(offset,count,buffer.data(),got))return failure(source->Error(),medium+" sector read failed; missing sectors were not synthesized",offset);
   if(got!=count)return failure(ERROR_READ_FAULT,"Short "+medium+" sector read; the image was discarded",offset);
   if(!source->Verify(current))return failure(source->Error(),medium+" media check failed after reading",offset);
   if(current!=generation)return failure(ERROR_MEDIA_CHANGED,medium+" media changed while reading; the image was discarded",offset);
   try{file->Write(offset,buffer.data(),count);}
   catch(const HfsPlus::Error&){return failure(GetLastError(),"Writing the image file failed",offset);}
   offset+=count;p.bytes=p.processed=offset;job.progress(p);
  }
  if(job.cancelled())throw Storage::Cancelled();
  if(!source->Verify(current))return failure(source->Error(),"Final "+medium+" media check failed",size);
  if(current!=generation)return failure(ERROR_MEDIA_CHANGED,medium+" media changed at completion; the image was discarded",size);
  p.phase="Saving "+medium+" image";job.progress(p);
  file->Flush();
  if(job.cancelled())throw Storage::Cancelled();
  if(replace){
   WindowsMoveUndo::RunExport(folder,[&]{
    auto& transaction=*WindowsMoveUndo::Session::Current();
    const auto stagedFile=file->WorkerFiles().front();
    const auto handle=HANDLE(stagedFile.second);
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle,&info))
     throw HfsPlus::Error("Cannot identify the completed image");
    const auto completed=MacBrowser::WindowsIdentity(info);
    const auto staged=transaction.StageTemporary(stagedFile.first,handle);
    file->Keep();
    file.reset();
    if(job.cancelled())throw Storage::Cancelled();
    original.Close();
    transaction.Rename(outputPath,transaction.Slot(),&approved);
#ifdef HFV_TESTING
    if(MacBrowser::windowsPublicationFault)
     MacBrowser::windowsPublicationFault("media-image-original-staged");
#endif
    if(job.cancelled())throw Storage::Cancelled();
    transaction.Rename(staged,outputPath,&completed);
    if(job.cancelled())throw Storage::Cancelled();
   });
  }else{
   try{file->PublishTo(outputPath);}
   catch(const HfsPlus::Error& error){return failure(GetLastError(),error.what(),size);}
   file->Keep();
  }
  p.phase=medium+" image saved";job.progress(p);return {};
 };
 if(drive<0||drive>25)return work;
 auto wide=VolumeCoord::Wide(output);auto changed=VolumeCoord::PathKey(wide);
 return CommandStorage::Guard({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read},{changed,VolumeCoord::Mode::Write},{VolumeCoord::ImageKey(wide),VolumeCoord::Mode::Write}},work,{changed});
}

std::shared_ptr<CdImageSource> MakeNativeCdImageSource() {
 return std::make_shared<NativeCdImageSource>();
}
std::function<Storage::Result(Storage::Job&)> MakeCdImageJob(int drive,std::string output,std::shared_ptr<CdImageSource> source,CdImageSelection selection) {
 auto selected=std::make_shared<SelectedCdImageSource>(source?source:MakeNativeCdImageSource(),selection);
 return MakeMediaImageJob(drive,std::move(output),selected,selection.macVolumeOnly?"Mac volume":"CD data track");
}

std::function<Storage::Result(Storage::Job&)> MakeCdTrackInspectionJob(int drive,std::shared_ptr<CdImageSource> source,std::shared_ptr<std::vector<CdMedia::Track>> tracks,std::shared_ptr<DWORD> generation){
 return CommandStorage::Guard({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}},[=](Storage::Job& job){
  struct Close{CdImageSource* p;~Close(){p->Close();}}close{source.get()};if(job.cancelled())throw Storage::Cancelled();std::string reason;
  if(!source->Open(drive,reason))return failure(source->Error(),reason);*tracks=source->DataTracks();if(tracks->empty())return failure(ERROR_NOT_SUPPORTED,"No data tracks are available; audio tracks are excluded");
  if(!source->Verify(*generation))return failure(source->Error(),"Cannot establish data-track media identity");return Storage::Result{};
 });
}
