// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CommandStorage.h"
#include "BrowserSession.h"
#include "resource.h"
#include "CopyProgress.h"
#include "FileOperations.h"
#include "StorageJobDialog.h"
#include "charset.h"
#include "FloppyWriter.h"
#include "DirectFloppy.h"
#include "FloppyFileWrite.h"
#include "MacBrowserModel.h"
#include "ReadOnlyCdBackend.h"
#include <vector>
#include <fcntl.h>
#include <stdexcept>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
#include "hfs/copyout.h"
#include "hfs/copyin.h"
#include "hfs/Copyhfs.h"
}
namespace {
// Error pointers in the old libraries can retain a default or a previous error.
bool usefulError(const char* text) {return text&&*text&&_stricmp(text,"no error");}
std::string copyError() {
 if(usefulError(hfs_error))return hfs_error;
 if(usefulError(cpi_error))return cpi_error;
 if(usefulError(cph_error))return cph_error;
 if(usefulError(cpo_error))return cpo_error;
 return errno?std::string("Copy failed: ")+strerror(errno):"The copy failed; no further error information was supplied.";
}
Storage::Result copyToFloppy(Storage::Job& job,int drive,const std::string& sourceVolume,const std::string& source,
 const std::string& targetVolume,const std::string& target,int directory,int move,LegacyCopyOptions options) {
 const auto generation=options.targetGenerationKnown?std::optional<DWORD>(options.targetGeneration):std::nullopt;
 const bool sameVolume=!_stricmp(sourceVolume.c_str(),targetVolume.c_str());
 Storage::Result result;
 try {
  const bool wrote=RunFloppyWrite(drive,generation,[&]{
   result=MakeCopyJob(sourceVolume,source,targetVolume,target,directory,sameVolume?move:0,options)(job);
   auto progress=job.progress();progress.phase="Verifying floppy";job.progress(progress);
  });
  if(result.code){result.moved=false;if(wrote)result.message+=" The destination may contain partial files.";}
 }catch(const FloppyWriteFailure& error){
  return {1,error.error,error.what()};
 }
 return result;
}
bool macVolume(const std::string& path) {
 size_t n=path.size();return n>=4 && path[n-4]=='.' &&
  ((toupper(path[n-3])=='H'&&toupper(path[n-2])=='F')||!_stricmp(path.c_str()+n-3,"DSK"));
}
std::pair<std::shared_ptr<MacBrowser::Volume>,uint32_t> directorySource(const std::string& volume,const std::string& path,bool writable){
 if(!macVolume(volume))return MacBrowser::ResolveWindowsFile(VolumeCoord::Wide(path));
 auto at=path.find_last_of(':');if(at==path.npos||at+1==path.size())throw std::runtime_error("Select a folder rather than a volume root for a move");
 auto parent=path.substr(0,at+1),name=path.substr(at+1);auto converted=std::vector<unsigned char>(name.begin(),name.end());converted.push_back(0);pc_to_mac_charset(converted.data());
 int n=MultiByteToWideChar(10000,0,reinterpret_cast<const char*>(converted.data()),-1,nullptr,0);std::wstring wide(n,0);MultiByteToWideChar(10000,0,reinterpret_cast<const char*>(converted.data()),-1,&wide[0],n);wide.pop_back();
 auto v=MacBrowser::OpenClassic(volume,parent,writable);MacBrowser::Entry e;if(!v->FindChild(2,MacBrowser::U16(wide),e))throw std::runtime_error("The source folder no longer exists");return {v,e.id};
}
unsigned long long amount(const hfsdirent& entry,char mode) {
 if(mode=='R')return entry.u.file.rsize;
 if(mode=='r'||mode=='t'||mode=='R'||(mode=='a'&&(!strcmp(entry.u.file.type,"TEXT")||!strcmp(entry.u.file.type,"ttro")||!entry.u.file.rsize)))return entry.u.file.dsize;
 return (unsigned long long)entry.u.file.dsize+entry.u.file.rsize;
}
void scan(hfsvol* volume,const std::string& path,char mode,Storage::Job& job,Storage::Progress& progress,unsigned depth=0) {
 if(job.cancelled())throw Storage::Cancelled();
 if(depth>128 || path.size()>=MAX_PATH-2)throw std::runtime_error("The selected folder exceeds the supported path depth or length");
 hfsdirent entry;
 if(hfs_stat(volume,const_cast<char*>(path.c_str()),&entry)<0)throw std::runtime_error(hfs_error?hfs_error:"Could not read the selected item");
 progress.path=path;job.progress(progress);
 if(!(entry.flags&HFS_ISDIR)){++progress.totalFiles;progress.totalBytes+=amount(entry,mode);return;}
 hfsdir* directory=hfs_opendir(volume,const_cast<char*>(path.c_str()));
 if(!directory)throw std::runtime_error(hfs_error?hfs_error:"Could not open the selected folder");
 struct Close{hfsdir* value;~Close(){hfs_closedir(value);}} close={directory};
 while(hfs_readdir(directory,&entry)>=0)scan(volume,path+(path.back()==':'?"":":")+entry.name,mode,job,progress,depth+1);
 if(errno!=ENOENT)throw std::runtime_error(hfs_error?hfs_error:"Folder scan failed");
}
struct ExportJobContext {
 Storage::Job& job;Storage::Progress progress;std::string metadataError;
 struct Directory {
  MacBrowser::Entry source,expected,retained;std::shared_ptr<MacBrowser::Volume> windows;uint32_t id=0;long cnid=0;bool merge=false;uint64_t skipped=0;
 };
 static MacBrowser::Entry ClassicMetadata(hfsvol* volume,const char* path){
  hfsdirent raw;if(hfs_stat(volume,const_cast<char*>(path),&raw)<0)throw std::runtime_error(copyError());
  MacBrowser::Entry e;e.directory=(raw.flags&HFS_ISDIR)!=0;e.resolvedId=uint32_t(raw.cnid);e.datesValid=true;
  e.created=uint32_t(int64_t(raw.crdate)+2082844800LL);e.modified=uint32_t(int64_t(raw.mddate)+2082844800LL);
  if(hfs_getfinderinfo(volume,const_cast<char*>(path),e.finder.data())<0)throw std::runtime_error(copyError());return e;
 }
 static int directory(int action,hfsvol* from,hfsvol* to,const char* source,const char* target,void** token,void* context){
  auto& self=*static_cast<ExportJobContext*>(context);
  try{
   if(action==0){
    auto d=std::make_unique<Directory>();d->skipped=self.progress.skipped;
    if(from)d->source=ClassicMetadata(from,source);else{auto item=MacBrowser::ResolveWindowsFile(VolumeCoord::Wide(source));d->source=item.first->Stat(item.second);d->source.attributes=item.first->GetAttributes(item.second);d->source.attributesKnown=true;}
    if(to){hfsdirent raw;if(hfs_stat(to,const_cast<char*>(target),&raw)==0){if(!(raw.flags&HFS_ISDIR))throw std::runtime_error("Folder destination is not a directory");d->merge=true;d->retained=ClassicMetadata(to,target);d->cnid=d->retained.resolvedId;}
     if(!d->merge&&(!d->source.attributes.empty()||d->source.created>UINT32_MAX||d->source.modified>UINT32_MAX))throw std::runtime_error("Classic HFS cannot preserve the source folder metadata");
    }else{DWORD attrs=GetFileAttributesA(target);if(attrs!=INVALID_FILE_ATTRIBUTES){auto item=MacBrowser::ResolveWindowsFile(VolumeCoord::Wide(target));d->windows=item.first;d->id=item.second;d->retained=d->windows->Stat(d->id);if(!d->retained.directory)throw std::runtime_error("Folder destination is not a directory");d->expected=d->retained;d->merge=true;}}
    *token=d.release();return 0;
   }
   auto d=static_cast<Directory*>(*token);if(!d)return 0;
   if(action==1){if(to){auto e=ClassicMetadata(to,target);d->cnid=e.resolvedId;}else{if(!d->windows){auto item=MacBrowser::ResolveWindowsFile(VolumeCoord::Wide(target));d->windows=item.first;d->id=item.second;}d->expected=d->windows->Stat(d->id);}return 0;}
   std::unique_ptr<Directory> owned(d);*token=nullptr;
   if(d->merge||(action==2&&d->skipped==self.progress.skipped)){
    auto& desired=d->merge?d->retained:d->source;
    if(to){time_t dates[2]={time_t(int64_t(desired.created)-2082844800LL),time_t(int64_t(desired.modified)-2082844800LL)};
     if(hfs_setdirmetadata(to,const_cast<char*>(target),d->cnid,d->merge?nullptr:desired.finder.data(),dates)<0)throw std::runtime_error(copyError());
    }else d->windows->FinalizeDirectory(d->id,desired,d->expected,d->merge);
   }return 0;
  }catch(const std::exception& e){self.metadataError=std::string("Folder metadata: ")+e.what();hfs_error=&self.metadataError[0];return 1;}
 }

 static void observe(const char* path,unsigned long bytes,unsigned long long total,int completed,void* context) {
  auto& self=*static_cast<ExportJobContext*>(context);
  if(!bytes&&!completed)self.progress.fileBytes=0;
  self.progress.path=path;self.progress.fileTotal=total;
  self.progress.fileBytes+=bytes;self.progress.bytes+=bytes;self.progress.processed+=bytes;self.progress.files+=completed?1:0;
  self.job.progress(self.progress);
 }
 static int overwrite(const char* path,void* context) {
  auto& self=*static_cast<ExportJobContext*>(context);
  std::string target=path;auto slash=target.find_last_of("\\/");
  if(slash!=target.npos&&slash+2<target.size()&&target[slash+1]=='%'){
   auto owner=target.substr(0,slash+1)+target.substr(slash+2);
   if(GetFileAttributesA(owner.c_str())!=INVALID_FILE_ATTRIBUTES&&MacBrowser::WindowsResourceCompanion(VolumeCoord::Wide(target))){
    self.metadataError="The destination is another item's metadata companion; it was preserved";hfs_error=&self.metadataError[0];return 0;
   }
  }
  auto choice=self.job.replace(path);
  if(choice==Storage::ConflictChoice::Skip) {
   ++self.progress.skipped;self.progress.processed+=self.progress.fileTotal;self.job.progress(self.progress);return 2;
  }
  return choice==Storage::ConflictChoice::Replace?1:0;
 }
};
std::vector<char> buffer(const std::string& text) {
 if(text.size()>=MAX_PATH-2)throw std::runtime_error("A copy path is too long");
 std::vector<char> value(MAX_PATH);memcpy(value.data(),text.c_str(),text.size()+1);return value;
}
}
LegacyCopyOptions CaptureLegacyCopyOptions(char output){LegacyCopyOptions options;int ad;HFSIFACE_copy_settings(&options.output,&options.input,&ad);options.output=output;options.appledouble=ad!=0;return options;}
std::function<Storage::Result(Storage::Job&)> MakeCopyJob(std::string sv,std::string source,std::string tv,std::string target,int directory,int move,char mode){
 return MakeCopyJob(std::move(sv),std::move(source),std::move(tv),std::move(target),directory,move,CaptureLegacyCopyOptions(mode));
}
std::function<Storage::Result(Storage::Job&)> MakeCopyJob(std::string sourceVolume,std::string source,std::string targetVolume,std::string target,int directory,int move,LegacyCopyOptions options) {
 auto work=[=](Storage::Job& job)->Storage::Result {
  if(job.cancelled())throw Storage::Cancelled();
  char mode=options.output,oldOut,oldIn;int oldAd;HFSIFACE_copy_settings(&oldOut,&oldIn,&oldAd);
  struct Settings{char out,in;int ad;~Settings(){HFSIFACE_restore_copy_settings(out,in,ad);}}settings{oldOut,oldIn,oldAd};
  HFSIFACE_restore_copy_settings(options.output,options.input,options.appledouble);

  if(!macVolume(sourceVolume)&&GetFileAttributesA(source.c_str())==INVALID_FILE_ATTRIBUTES){
   DWORD error=GetLastError();char message[160];sprintf_s(message,"Could not access the source file or folder (Windows error %lu).",error);
   return {1,error,message};
  }
  int floppy=physical_floppy_path(targetVolume.c_str());
  if(floppy>=0&&!DirectFloppy::Current(floppy))return copyToFloppy(job,floppy,sourceVolume,source,targetVolume,target,directory,move,options);
  auto sv=buffer(sourceVolume),s=buffer(source),tv=buffer(targetVolume),t=buffer(target);
  ExportJobContext context={job,{}};context.progress.path=source;context.progress.fileCopy=true;context.progress.phase="Scanning selected files";job.progress(context.progress);
  if(macVolume(sourceVolume)&&!macVolume(targetVolume)) {
   hfsvol* volume=hfs_mount(sv.data(),0,O_RDONLY);
   if(!volume)return {1,GetLastError(),hfs_error?hfs_error:"Could not open the source volume"};
   struct Close{hfsvol* value;~Close(){hfs_umount(value);}} close={volume};
   auto path=buffer(source);pc_to_mac_charset((unsigned char*)path.data());
   scan(volume,path.data(),mode,job,context.progress);
  }
  if(job.cancelled())return {1,ERROR_CANCELLED,"Copy cancelled"};
  context.progress.scanning=false;context.progress.phase="Copying";job.progress(context.progress);
  int silent=HFSIFACE_silencer(1),merge=HFSIFACE_copy_options(1);char oldMode=HFSIFACE_exchange_copy_mode(mode);
  struct Cleanup {
   int silent,merge;char mode;
   ~Cleanup(){HFSIFACE_directory_copy_hook(nullptr,nullptr);copyout_set_progress(nullptr,nullptr);copyout_set_overwrite(nullptr,nullptr);HFSIFACE_copy_options(merge);HFSIFACE_silencer(silent);HFSIFACE_exchange_copy_mode(mode);}
  } cleanup={silent,merge,oldMode};
  HFSIFACE_directory_copy_hook(ExportJobContext::directory,&context);
  copyout_set_progress(ExportJobContext::observe,&context);copyout_set_overwrite(ExportJobContext::overwrite,&context);
  hfs_error=nullptr;cpi_error=nullptr;cph_error=nullptr;cpo_error=nullptr;errno=0;SetLastError(0);
  std::string sourceSnapshot;
  if(directory&&move){auto item=directorySource(sourceVolume,source,false);sourceSnapshot=MacBrowser::Snapshot(*item.first,item.second,job);item.first->Close();}
  // Folder moves retain the entire source until conversion and postorder folder
  // metadata publication have both succeeded. A skipped child keeps the source.
  int moved=0;int result=HFSIFACE_copy(sv.data(),s.data(),tv.data(),t.data(),directory,directory?0:move,&moved);
  if(directory&&move&&!result&&!job.cancelled()&&!context.progress.skipped){
   auto item=directorySource(sourceVolume,source,true);
   if(MacBrowser::Snapshot(*item.first,item.second,job)!=sourceSnapshot)throw std::runtime_error("The source folder changed during copying; source retained");
   MacBrowser::RemoveTree(*item.first,item.second,job);item.first->Close();moved=1;
  }
  Storage::Result outcome;outcome.code=result;outcome.moved=moved!=0;outcome.error=result?GetLastError():0;
  if(job.cancelled()){outcome.code=1;outcome.error=ERROR_CANCELLED;outcome.message="Copy cancelled";}
  else if(result)outcome.message=copyError();
  return outcome;
 };
 auto resource=[](const std::string& volume,const std::string& path){return macVolume(volume)?VolumeCoord::ImageKey(volume):VolumeCoord::PathKey(VolumeCoord::Wide(path));};
 auto from=resource(sourceVolume,source),to=macVolume(targetVolume)?resource(targetVolume,target):VolumeCoord::LegacyOutputKey(VolumeCoord::Wide(target));
 std::vector<VolumeCoord::Access> access{{from,move?VolumeCoord::Mode::Write:VolumeCoord::Mode::Read},{to,VolumeCoord::Mode::Write}};if(!macVolume(sourceVolume)){auto path=VolumeCoord::Wide(source);auto at=path.find_last_of(L"\\/");auto companion=path.substr(0,at+1)+L"%"+path.substr(at+1);access.push_back({VolumeCoord::PathKey(companion),move?VolumeCoord::Mode::Write:VolumeCoord::Mode::Read});}
 return CommandStorage::Guard(access,work,{to});
}
bool StageWindowsLaunchDocument(BrowserSession& session,const FileOperations::Location& source,CString& output)
{
 output.Empty();
 char temp[MAX_PATH], reservation[MAX_PATH];
 DWORD length=GetTempPath(ARRAYSIZE(temp),temp);
 auto owner=session.Owner();
 if(!length||length>=ARRAYSIZE(temp)||!GetTempFileName(temp,"hfv",0,reservation)){
  owner->MessageBox("Could not reserve a directory for the temporary document.","HFVExplorer",MB_OK|MB_ICONERROR);
  return false;
 }
 CString folder=CString(reservation)+".dir";
 BOOL created=folder.GetLength()<MAX_PATH&&CreateDirectory(folder,nullptr);
 DeleteFile(reservation);
 if(!created){
  owner->MessageBox("Could not create the temporary document directory.","HFVExplorer",MB_OK|MB_ICONERROR);
  return false;
 }

 auto target=FileOperations::Location::Windows(VolumeCoord::Wide(std::string(static_cast<LPCSTR>(folder))));
 FileOperations::LegacyConversion conversion{source,target,false,false,false,CaptureLegacyCopyOptions('r')};
 conversion.selection=FileOperations::ConversionSelection{source,target};
 auto operation=FileOperations::Submit(FileOperations::Request(std::move(conversion)));
 auto result=ShowSubmittedStorageJob(operation.Job(),"Copying from Macintosh",!owner->IsWindowVisible(),true,true,owner);
 if(result.code){
  RemoveDirectory(folder);
  if(result.error!=ERROR_CANCELLED)owner->MessageBox(result.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);
  return false;
 }

 // The export can add an extension. This directory belongs only to this launch.
 WIN32_FIND_DATA data{};
 HANDLE search=FindFirstFile(folder+"\\*",&data);
 CString leaf;
 bool complete=false;
 if(search!=INVALID_HANDLE_VALUE){
  do{
   if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
   if(!leaf.IsEmpty()){leaf.Empty();break;}
   leaf=data.cFileName;
  }while(FindNextFile(search,&data));
  complete=GetLastError()==ERROR_NO_MORE_FILES;
  FindClose(search);
 }
 if(complete&&!leaf.IsEmpty()&&folder.GetLength()+1+leaf.GetLength()<MAX_PATH){
  output=folder+"\\"+leaf;
  return true;
 }
 owner->MessageBox("Cannot identify the temporary document to launch. Export directory: "+folder,"HFVExplorer",MB_OK|MB_ICONERROR);
 RemoveDirectory(folder);
 return false;
}
