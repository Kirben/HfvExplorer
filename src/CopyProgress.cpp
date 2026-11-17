// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CommandStorage.h"
#include "resource.h"
#include "CopyProgress.h"
#include "FileOperations.h"
#include "StorageJobDialog.h"
#include "charset.h"
#include "FloppyWriter.h"
#include "DirectFloppy.h"
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
void HFSIFACE_get_dir_flush(void);
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
 FloppyWriteApi* api=nullptr;
#ifdef HFV_FLOPPY_TESTING
 api=dynamic_cast<FloppyWriteApi*>(physical_floppy_test_api(drive));
 if(!api)return {1,ERROR_NOT_SUPPORTED,"No simulated writable floppy is attached"};
#endif
 HFSIFACE_get_dir_flush();physical_floppy_suspend(drive);
 DirectFloppy disk(api);
 if(!disk.Open(drive,options.verifyFloppy))return {1,disk.Error(),"Could not open and lock the destination HFS floppy (Windows error "+std::to_string(disk.Error())+")."};
 if(options.targetGenerationKnown&&disk.Generation()!=options.targetGeneration)return {1,ERROR_MEDIA_CHANGED,"Floppy media changed after selection; select the volume again"};
 bool sameVolume=!_stricmp(sourceVolume.c_str(),targetVolume.c_str());
 Storage::Result result;
 try {result=MakeCopyJob(sourceVolume,source,targetVolume,target,directory,sameVolume?move:0,options)(job);}
 catch(...){HFSIFACE_get_dir_flush();disk.Finish();throw;}
 HFSIFACE_get_dir_flush();
 auto p=job.progress();p.phase=options.verifyFloppy?"Verifying floppy":"Flushing floppy";job.progress(p);
 if(!disk.Finish())return {1,disk.Error(),"Floppy copy failed while writing/flushing buffered data (Windows error "+std::to_string(disk.Error())+"). "+(sameVolume&&move?"The floppy may be incomplete; the move may have been partly applied.":"The destination may be incomplete; source files were retained.")};
 if(result.code){result.moved=false;if(disk.WritesStarted())result.message+=" The destination may contain partial files.";}
 return result;
}
bool macVolume(const std::string& path) {
 size_t n=path.size();return n>=4 && path[n-4]=='.' &&
  ((toupper(path[n-3])=='H'&&toupper(path[n-2])=='F')||!_stricmp(path.c_str()+n-3,"DSK"));
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
 Storage::Job& job;Storage::Progress progress;
 static void observe(const char* path,unsigned long bytes,unsigned long long total,int completed,void* context) {
  auto& self=*static_cast<ExportJobContext*>(context);
  if(!bytes&&!completed)self.progress.fileBytes=0;
  self.progress.path=path;self.progress.fileTotal=total;
  self.progress.fileBytes+=bytes;self.progress.bytes+=bytes;self.progress.processed+=bytes;self.progress.files+=completed?1:0;
  self.job.progress(self.progress);
 }
 static int overwrite(const char* path,void* context) {
  auto& self=*static_cast<ExportJobContext*>(context);auto choice=self.job.replace(path);
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
   ~Cleanup(){copyout_set_progress(nullptr,nullptr);copyout_set_overwrite(nullptr,nullptr);HFSIFACE_copy_options(merge);HFSIFACE_silencer(silent);HFSIFACE_exchange_copy_mode(mode);}
  } cleanup={silent,merge,oldMode};
  copyout_set_progress(ExportJobContext::observe,&context);copyout_set_overwrite(ExportJobContext::overwrite,&context);
  hfs_error=nullptr;cpi_error=nullptr;cph_error=nullptr;cpo_error=nullptr;errno=0;SetLastError(0);
  int moved=0;int result=HFSIFACE_copy(sv.data(),s.data(),tv.data(),t.data(),directory,move,&moved);
  Storage::Result outcome;outcome.code=result;outcome.moved=moved!=0;outcome.error=result?GetLastError():0;
  if(job.cancelled()){outcome.code=1;outcome.error=ERROR_CANCELLED;outcome.message="Copy cancelled";}
  else if(result)outcome.message=copyError();
  return outcome;
 };
 auto resource=[](const std::string& volume,const std::string& path){return macVolume(volume)?VolumeCoord::ImageKey(volume):VolumeCoord::HostKey(VolumeCoord::Wide(path));};
 auto from=resource(sourceVolume,source),to=resource(targetVolume,target);
 return CommandStorage::Guard({{from,move?VolumeCoord::Mode::Write:VolumeCoord::Mode::Read},{to,VolumeCoord::Mode::Write}},work,{to});
}
int CopyWithProgress(char* sourcevol,char* source,char* targetvol,char* target,int directory,int move,int* moved,CWnd* owner) {
 *moved=0;FileOperations::Request request;request.kind=FileOperations::Kind::LegacyCopy;
 request.sourceVolume=sourcevol;request.sourcePath=source;request.targetVolume=targetvol;request.targetPath=target;
 request.directory=directory!=0;request.move=move!=0;request.conversion=CaptureLegacyCopyOptions(HFSIFACE_copy_mode());
 auto operation=FileOperations::Submit(std::move(request));
 auto result=ShowSubmittedStorageJob(operation.Job(),macVolume(sourcevol)?"Copying from Macintosh":"Copying files",false,directory==0,true,owner);
 if(result.code&&result.error!=ERROR_CANCELLED){const char* text=result.message.empty()?"The copy failed.":result.message.c_str();if(owner)owner->MessageBox(text,"HFVExplorer",MB_OK|MB_ICONERROR);else AfxMessageBox(text,MB_OK|MB_ICONERROR);}
 if(result.code&&physical_floppy_path(targetvol)>=0&&owner)owner->PostMessage(WM_COMMAND,ID_VIEW_REFRESH);
 *moved=result.moved?1:0;return result.code;
}
