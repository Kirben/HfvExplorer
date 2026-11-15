// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "CopyProgress.h"
#include "StorageJobDialog.h"
#include "charset.h"
#include "FloppyWriter.h"
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
extern char* cph_error;
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
struct TemporaryImage {
 char path[MAX_PATH]={};
 ~TemporaryImage(){if(*path)DeleteFileA(path);}
 bool create(const std::vector<BYTE>& bytes) {
  char folder[MAX_PATH];if(!GetTempPathA(MAX_PATH,folder)||!GetTempFileNameA(folder,"hfc",0,path))return false;
  char image[MAX_PATH];if(strlen(path)+4>=MAX_PATH){SetLastError(ERROR_FILENAME_EXCED_RANGE);return false;}
  strcpy_s(image,path);strcat_s(image,".dsk");if(!MoveFileA(path,image))return false;strcpy_s(path,image);
  HANDLE h=CreateFileA(path,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(h==INVALID_HANDLE_VALUE)return false;
  DWORD wrote=0;bool ok=WriteFile(h,bytes.data(),DWORD(bytes.size()),&wrote,nullptr)&&wrote==bytes.size();
  DWORD error=ok?0:GetLastError();CloseHandle(h);SetLastError(error);return ok;
 }
};
void floppyCopyProgress(const char* phase,unsigned completed,unsigned total,void* context) {
 auto& job=*static_cast<Storage::Job*>(context);Storage::Progress p;
 // Readiness checks are intentionally not separate visible phases.
 p.phase=strstr(phase,"readback")||!strcmp(phase,"Verifying")?"Verifying floppy":
  strstr(phase,"original")?"Checking destination floppy":
  strstr(phase,"Flushing")?"Flushing floppy":"Writing floppy";
 p.scanning=false;p.totalBytes=total;p.processed=completed;job.progress(p);
}
Storage::Result copyToFloppy(Storage::Job& job,int drive,const std::string& sourceVolume,const std::string& source,
 const std::string& targetVolume,const std::string& target,int directory,int move,char mode) {
 FloppyReadApi* readApi=nullptr;FloppyWriteApi* writeApi=nullptr;
#ifdef HFV_FLOPPY_TESTING
 readApi=physical_floppy_test_api(drive);writeApi=dynamic_cast<FloppyWriteApi*>(readApi);
 if(!writeApi)return {1,ERROR_NOT_SUPPORTED,"No simulated writable floppy is attached"};
#endif
 Storage::Progress p;p.phase="Reading destination floppy";job.progress(p);
 ReadOnlyFloppy reader(readApi);
 if(!reader.OpenDrive(drive)||!reader.FindHfs())return {1,reader.Error(),"Could not read the destination HFS floppy"};
 if(reader.MediaSize()!=1474560)return {1,ERROR_NOT_SUPPORTED,"File copying currently supports 1.44 MB HFS floppies"};
 std::vector<BYTE> original(reader.MediaSize());
 if(!reader.ReadBytes(0,DWORD(original.size()),original.data()))return {1,reader.Error(),"Could not read every sector of the destination floppy"};
 reader.Close();if(job.cancelled())throw Storage::Cancelled();
 TemporaryImage stage;
 if(!stage.create(original))return {1,GetLastError(),"Could not create the temporary floppy image"};
 // Only the working image is writable during filesystem changes. Source files
 // stay intact even if the later physical write or readback fails.
 bool sameVolume=!_stricmp(sourceVolume.c_str(),targetVolume.c_str());
 auto result=MakeCopyJob(sameVolume?stage.path:sourceVolume,source,stage.path,target,directory,sameVolume?move:0,mode)(job);
 if(result.code)return result;
 std::vector<BYTE> modified;std::string error;
 if(!LoadFloppyHfs(stage.path,modified,error))return {1,ERROR_INVALID_DATA,error};
 if(job.cancelled())throw Storage::Cancelled();
 if(modified==original)return {};
 physical_floppy_suspend(drive);
 FloppyWriter writer(writeApi);
 if(!writer.WriteImage(drive,modified,false,floppyCopyProgress,&job,&original)) {
  char detail[256];sprintf_s(detail,"Floppy copy failed at %s (Windows error %lu). %s",writer.Stage().c_str(),writer.Error(),
   writer.WritesStarted()?"Writing started; the disk may be incomplete. Source files were retained.":"No sectors were written.");
  return {1,writer.Error(),detail};
 }
 result.moved=sameVolume&&result.moved;return result;
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
std::function<Storage::Result(Storage::Job&)> MakeCopyJob(std::string sourceVolume,std::string source,std::string targetVolume,std::string target,int directory,int move,char mode) {
 return [=](Storage::Job& job)->Storage::Result {
  if(!macVolume(sourceVolume)&&GetFileAttributesA(source.c_str())==INVALID_FILE_ATTRIBUTES){
   DWORD error=GetLastError();char message[160];sprintf_s(message,"Could not access the source file or folder (Windows error %lu).",error);
   return {1,error,message};
  }
  int floppy=physical_floppy_path(targetVolume.c_str());
  if(floppy>=0)return copyToFloppy(job,floppy,sourceVolume,source,targetVolume,target,directory,move,mode);
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
}
int CopyWithProgress(char* sourcevol,char* source,char* targetvol,char* target,int directory,int move,int* moved) {

 *moved=0;
 auto result=ShowStorageJob(MakeCopyJob(sourcevol,source,targetvol,target,directory,move,HFSIFACE_copy_mode()),macVolume(sourcevol)?"Copying from Macintosh":"Copying files",false,directory==0);
 if(result.code && result.error!=ERROR_CANCELLED)AfxMessageBox(result.message.empty()?"The copy failed.":result.message.c_str(),MB_OK|MB_ICONERROR);
 // A failed physical write can have changed the disk too. Discard the old
 // directory view rather than presenting its cached contents as current.
 if(result.code&&physical_floppy_path(targetvol)>=0&&AfxGetMainWnd())AfxGetMainWnd()->PostMessage(WM_COMMAND,ID_VIEW_REFRESH);
 *moved=result.moved?1:0;return result.code;
}
