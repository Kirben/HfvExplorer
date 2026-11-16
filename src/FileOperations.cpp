// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "FileOperations.h"
#include "VolumeCoordinator.h"
#include "DirectFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "RenameName.h"
#include "charset.h"
#include <algorithm>
#include <set>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
void HFSIFACE_get_dir_flush(void);
}
extern int DeleteHostSelectionJob(const CString&,bool,Storage::Job*);
namespace FileOperations {
struct Operation::State {mutable std::mutex mutex;Completion completion;};
struct Registry {
 struct Entry{std::weak_ptr<Operation::State> state;std::weak_ptr<Storage::Job> job;};
 std::mutex mutex;std::vector<Entry> entries;
 static Registry& Get(){static Registry registry;return registry;}
};
namespace {
using namespace MacBrowser;
struct Failure:std::runtime_error {DWORD error;Failure(DWORD e,const std::string& text):std::runtime_error(text),error(e?e:ERROR_WRITE_FAULT){}};
void Check(bool ok,const char* text){if(!ok)throw HfsPlus::Error(text);}
std::string ClassicError(const char* fallback){return hfs_error&&*hfs_error&&_stricmp(hfs_error,"no error")?hfs_error:fallback;}
void Mark(Completion& c,const Location& item){Change value;value.type=item.type;value.volume=item.volume;value.windowsPath=item.windowsPath;value.classicImage=item.classicImage;value.classicPath=item.classicPath;c.affected.push_back(std::move(value));}
int CancelClassic(void* job){return static_cast<Storage::Job*>(job)->cancelled()?1:0;}
void Cancel(Storage::Job& job){if(job.cancelled())throw Storage::Cancelled();}
std::u16string Ansi(const std::string& s){int n=MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),nullptr,0);std::wstring w(n,0);if(n)MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),&w[0],n);return U16(w);}
std::string Utf8(const std::u16string& text){auto w=Wide(text);int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);std::string out(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),&out[0],n,nullptr,nullptr);return out;}
bool Absolute(const std::string& p){return p.size()>=3&&((isalpha((unsigned char)p[0])&&p[1]==':'&&p[2]=='\\')||(p[0]=='\\'&&p[1]=='\\'));}
bool Absolute(const std::wstring& p){return p.size()>=3&&((iswalpha(p[0])&&p[1]==L':'&&(p[2]==L'\\'||p[2]==L'/'))||(p[0]==L'\\'&&p[1]==L'\\'));}
std::pair<std::shared_ptr<Volume>,uint32_t> Source(const Location& value,bool mutate=false){
 if(value.volume){auto e=value.volume->Stat(value.id);
  if(!value.expectedIdentity.empty())Check(e.identity==value.expectedIdentity,"The selected entry was replaced before the operation started");
  if(!value.expectedName.empty())Check(e.name==value.expectedName,"The selected entry was renamed before the operation started");
  if(value.expectedParent)Check(e.parent==value.expectedParent,"The selected entry was moved before the operation started");
  return {value.volume,value.id};}
 std::shared_ptr<Volume> volume;std::u16string name;
 if(value.type==LIST_FAT){Check(Absolute(value.windowsPath),"Use an absolute Windows source path");auto pos=value.windowsPath.find_last_of(L"\\/");Check(pos!=std::wstring::npos,"Use an absolute Windows source path");auto parent=value.windowsPath.substr(0,pos);if(parent.size()==2&&parent[1]==L':')parent+=L'\\';volume=OpenWindows(parent);name=U16(value.windowsPath.substr(pos+1));}
 else{auto pos=value.classicPath.find_last_of(':');Check(pos!=std::string::npos,"Use an absolute classic HFS path");volume=OpenClassic(value.classicImage,value.classicPath.substr(0,pos+1),mutate);name=Ansi(value.classicPath.substr(pos+1));}
 for(auto& e:volume->List())if(HfsPlus::CompareNames(e.name,name,volume->CaseSensitive())==0){if(value.expectedCatalogId)Check(e.resolvedId==value.expectedCatalogId,"The selected classic HFS entry was replaced");return {volume,e.id};}
 throw HfsPlus::Error("The selected source no longer exists");
}
std::pair<std::shared_ptr<Volume>,uint32_t> Destination(const Location& value){
 if(value.volume)return {value.volume,value.id};
 if(value.type==LIST_FAT){Check(Absolute(value.windowsPath),"Use an absolute Windows destination path");return {OpenWindows(value.windowsPath),2};}
 return {OpenClassic(value.classicImage,value.classicPath,true),2};
}
std::unique_ptr<DirectFloppy> WritableFloppy(const Location& target){
 int drive=target.type==LIST_HFS?physical_floppy_path(target.classicImage.c_str()):-1;if(drive<0)return {};
 physical_floppy_suspend(drive);std::unique_ptr<DirectFloppy> disk;
#ifdef HFV_FLOPPY_TESTING
 auto api=dynamic_cast<FloppyWriteApi*>(physical_floppy_test_api(drive));Check(api!=nullptr,"No simulated writable floppy is attached");disk.reset(new DirectFloppy(api));
#else
 disk.reset(new DirectFloppy);
#endif
 if(!disk->Open(drive))throw Failure(disk->Error(),"Cannot open and lock the destination floppy");return disk;
}
void Finish(std::unique_ptr<DirectFloppy>& disk){HFSIFACE_get_dir_flush();if(disk&&!disk->Finish())throw Failure(disk->Error(),"Floppy flush failed; the disk may be incomplete and source items were retained");}
Storage::Result CopyRequest(const Request& request,Storage::Job& job,Completion& completion){
 Check(!request.sources.empty(),"Select a source file or folder");HFSIFACE_get_dir_flush();auto disk=WritableFloppy(request.target);
 auto destination=Destination(request.target);Check(destination.first->Supports().create,"The destination does not support file creation");
 std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> opened;std::vector<std::string> snapshots;
 for(auto& item:request.sources){Cancel(job);auto source=Source(item);if(item.cut&&item.volume)Check(source.first->Writable(),"A cut source is read-only");snapshots.push_back(item.cut?MacBrowser::Snapshot(*source.first,source.second,job,bool(item.volume)):std::string());opened.push_back(source);}
 Mark(completion,request.target);
 bool complete=CopyMany(opened,*destination.first,destination.second,job);
 if(!request.target.volume)destination.first->Close();Finish(disk);
 for(size_t i=0;i<request.sources.size();++i)if(!request.sources[i].volume)opened[i].first->Close();
 // Revalidate every moved source before deleting any, after destination finalization.
 if(complete){
  std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> fresh(request.sources.size());
  for(size_t i=0;i<request.sources.size();++i)if(request.sources[i].cut){Cancel(job);fresh[i]=request.sources[i].volume?opened[i]:Source(request.sources[i],true);Check(MacBrowser::Snapshot(*fresh[i].first,fresh[i].second,job)==snapshots[i],"Source changed during the copy; source items were retained");}
  for(size_t i=0;i<request.sources.size();++i)if(request.sources[i].cut){Mark(completion,request.sources[i]);RemoveTree(*fresh[i].first,fresh[i].second,job);if(!request.sources[i].volume)fresh[i].first->Close();}
 }
 completion.complete=complete;completion.completed=job.progress().files;
 Storage::Result result;result.moved=complete&&std::any_of(request.sources.begin(),request.sources.end(),[](const Location& p){return p.cut;});return result;
}
Storage::Result Mutate(const Request& request,Storage::Job& job,Completion& completion){
 Check(!request.sources.empty(),"Select a file or folder");if(request.kind==Kind::Rename)Check(request.sources.size()==1,"Rename requires one selected entry");
 Storage::Progress progress;progress.phase=request.kind==Kind::Rename?"Renaming":"Deleting";progress.scanning=false;progress.totalFiles=unsigned(request.sources.size());progress.utf8=true;job.progress(progress);
 HFSIFACE_get_dir_flush();int silent=HFSIFACE_silencer(1);HFSIFACE_delete_cancel(CancelClassic,&job);struct Silent{int previous;~Silent(){HFSIFACE_delete_cancel(nullptr,nullptr);HFSIFACE_silencer(previous);}}scope{silent};
 for(auto& item:request.sources){Cancel(job);
  if(item.volume){auto source=Source(item);auto e=source.first->Stat(source.second);Check(e.id!=2,"Cannot rename or delete a volume root");
   if(request.kind==Kind::Rename){Check(!request.target.volume||request.target.volume==source.first,"Rename must stay within the same volume");Check(source.first->Supports().rename&&!e.protectedFile&&!e.hardLink&&!e.compressed,"The selected entry cannot be renamed");Mark(completion,item);source.first->Rename(source.second,request.target.id,request.name);}
   else{Check(source.first->Supports().remove,"The selected entry cannot be deleted");Mark(completion,item);RemoveTree(*source.first,source.second,job);}
  }else if(item.type==LIST_FAT){
   std::string path; // ANSI legacy path is retained verbatim.
   int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,item.windowsPath.c_str(),-1,nullptr,0,nullptr,nullptr);std::vector<char> converted(n);BOOL lost=FALSE;if(n)WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,item.windowsPath.c_str(),-1,converted.data(),n,nullptr,&lost);Check(n&&!lost,"Windows path cannot be represented by the legacy backend");path=converted.data();Check(Absolute(path),"Use an absolute Windows path");
   Mark(completion,item);
   if(request.kind==Kind::Rename){auto slash=path.find_last_of('\\');auto leaf=Wide(request.name);BOOL bad=FALSE;char name[MAX_PATH];int count=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,leaf.c_str(),-1,name,ARRAYSIZE(name),nullptr,&bad);Check(count&&!bad&&HfvValidRenameLeaf(name,false),"Invalid Windows filename");auto target=path.substr(0,slash+1)+name;
    if(!MoveFileExA(path.c_str(),target.c_str(),0))throw Failure(GetLastError(),"Cannot rename the selected Windows entry");
   }else if(!DeleteHostSelectionJob(CString(path.c_str()),item.directory,&job)){Cancel(job);throw Failure(GetLastError(),"Could not completely delete the selected Windows entry; remaining files were retained");}
  }else{
   Check(item.type==LIST_HFS&&!item.classicImage.empty(),"Invalid classic HFS source");auto disk=WritableFloppy(item);auto image=item.classicImage,path=item.classicPath;hfsdirent entry{};
   if(HFSIFACE_get_properties(&image[0],&path[0],&entry))throw HfsPlus::Error(ClassicError("Cannot read the selected HFS entry"));
   if(item.expectedCatalogId)Check(uint32_t(entry.cnid)==item.expectedCatalogId,"The selected classic HFS entry was replaced");Check(entry.cnid!=2,"Cannot rename or delete a volume root");
   Mark(completion,item);int code;
   if(request.kind==Kind::Rename){Check(!request.checkNameLock||!(entry.fdflags&HFS_FNDR_NAMELOCKED),"Remove the Finder name lock before renaming this entry");auto wide=Wide(request.name);char leaf[32];BOOL lost=FALSE;int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,wide.c_str(),-1,leaf,ARRAYSIZE(leaf),nullptr,&lost);Check(n&&!lost&&HfvValidRenameLeaf(leaf,true),"Invalid classic HFS filename");auto to=path.substr(0,path.find_last_of(':')+1)+leaf;code=HFSIFACE_rename(&image[0],&path[0],&to[0]);}
   else code=item.directory?HFSIFACE_rmdir(&image[0],&path[0]):HFSIFACE_delete(&image[0],&path[0]);
   std::string error=ClassicError("Classic HFS operation failed");Finish(disk);if(code){Cancel(job);throw HfsPlus::Error(error);}
  }
  ++completion.completed;++progress.files;job.progress(progress);
 }
 completion.complete=true;return {};
}
Storage::Result ExportFork(const Request& request,Storage::Job& job,Completion& completion){
 Check(request.sources.size()==1&&request.fork<2&&Absolute(request.target.windowsPath),"Invalid fork export request");auto source=Source(request.sources[0]);auto entry=source.first->Stat(source.second);Check(!entry.directory,"Select a file for fork export");
 source.first->Read(source.second,request.fork,0,nullptr,0);auto& path=request.target.windowsPath;
 HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw Failure(GetLastError(),"Cannot create output; choose a new filename");
 Mark(completion,request.target);
 try{std::vector<uint8_t> bytes(1024*1024);Storage::Progress progress;progress.fileCopy=progress.byteTransfer=progress.utf8=true;progress.scanning=false;progress.phase="Exporting fork";progress.totalFiles=1;progress.path=Utf8(entry.name);progress.totalBytes=progress.fileTotal=entry.forks[request.fork].size;
  for(uint64_t at=0;at<entry.forks[request.fork].size;){Cancel(job);size_t n=size_t((std::min)(uint64_t(bytes.size()),entry.forks[request.fork].size-at));source.first->Read(source.second,request.fork,at,bytes.data(),n);DWORD written;if(!WriteFile(file,bytes.data(),DWORD(n),&written,nullptr)||written!=n)throw Failure(GetLastError(),"Fork export write failed");at+=n;progress.bytes=progress.fileBytes=progress.processed=at;job.progress(progress);}
  if(!FlushFileBuffers(file))throw Failure(GetLastError(),"Fork export flush failed");CloseHandle(file);
 }catch(...){CloseHandle(file);DeleteFileW(path.c_str());throw;}
 completion.complete=true;completion.completed=1;return {};
}
Storage::Result ReplaceResource(const Request& request,Storage::Job& job,Completion& completion){
 Check(request.sources.size()==1&&bool(request.target.volume),"Invalid resource import request");auto from=Source(request.sources[0]),target=Source(request.target);auto volume=target.first;uint32_t id=target.second;auto original=volume->Stat(id),replacement=original;Check(!original.directory,"Select a file for resource import");
 replacement.forks[1].size=from.first->Stat(from.second).forks[0].size;replacement.modified=uint32_t(time(nullptr)+2082844800LL);
 Storage::Progress progress;progress.fileCopy=progress.byteTransfer=progress.utf8=true;progress.scanning=false;progress.phase="Importing resource fork";progress.totalFiles=1;progress.path=Utf8(original.name);progress.totalBytes=progress.fileTotal=replacement.forks[0].size+replacement.forks[1].size;
 Mark(completion,request.target);
 volume->Put(original.parent,replacement,[&](unsigned fork,uint64_t at,void* data,size_t n){Cancel(job);if(fork)from.first->Read(from.second,0,at,data,n);else volume->Read(id,0,at,data,n);progress.bytes+=n;progress.fileBytes+=n;progress.processed+=n;job.progress(progress);},true);
 if(!request.sources[0].volume)from.first->Close();completion.complete=true;completion.completed=1;return {};
}
std::string Resource(const Location& item){if(item.volume)return item.volume->ResourceKey();if(item.type==LIST_FAT)return VolumeCoord::HostKey(item.windowsPath);return VolumeCoord::ImageKey(item.classicImage);}
std::vector<VolumeCoord::Access> Resources(const Request& request){
 using VolumeCoord::Mode;std::vector<VolumeCoord::Access> access;
 if(request.kind==Kind::LegacyCopy){auto key=[](const std::string& volume,const std::string& path){return Absolute(path)?VolumeCoord::HostKey(VolumeCoord::Wide(path)):VolumeCoord::ImageKey(volume);};access.push_back({key(request.sourceVolume,request.sourcePath),request.move?Mode::Write:Mode::Read});access.push_back({key(request.targetVolume,request.targetPath),Mode::Write});return access;}
 for(auto& source:request.sources)access.push_back({Resource(source),(request.kind==Kind::Rename||request.kind==Kind::Delete||source.cut)?Mode::Write:Mode::Read});
 if(request.kind==Kind::Copy||request.kind==Kind::ReplaceResource||request.kind==Kind::ExportFork)access.push_back({Resource(request.target),Mode::Write});return access;
}
Storage::Result Execute(const Request& request,Storage::Job& job,Completion& completion){
 Cancel(job);
 if(request.kind==Kind::Copy)return CopyRequest(request,job,completion);
 if(request.kind==Kind::ExportFork)return ExportFork(request,job,completion);
 if(request.kind==Kind::ReplaceResource)return ReplaceResource(request,job,completion);
 if(request.kind!=Kind::LegacyCopy)return Mutate(request,job,completion);
  auto describe=[](const std::string& volume,const std::string& path){Location out;out.type=Absolute(path)?LIST_FAT:LIST_HFS;if(out.type==LIST_FAT)out.windowsPath=Wide(Ansi(path));else{out.classicImage=volume;out.classicPath=path;}return out;};
 Mark(completion,describe(request.targetVolume,request.targetPath));if(request.move)Mark(completion,describe(request.sourceVolume,request.sourcePath));
 auto result=MakeCopyJob(request.sourceVolume,request.sourcePath,request.targetVolume,request.targetPath,request.directory,request.move,request.conversion)(job);
 completion.completed=job.progress().files;completion.complete=!result.code&&!job.progress().skipped;
 return result;
}
}
Operation::Operation(std::shared_ptr<State> state,std::shared_ptr<Storage::Job> job):state_(std::move(state)),job_(std::move(job)){}
Completion Operation::Snapshot()const{std::lock_guard<std::mutex> lock(state_->mutex);return state_->completion;}
Operation Submit(Request request){
 auto state=std::make_shared<Operation::State>();auto values=std::make_shared<const Request>(std::move(request));
 auto job=Storage::worker().submit([state,values](Storage::Job& job){Completion completion;Storage::Result result;
  try{Cancel(job);VolumeCoord::Lease access(Resources(*values));result=Execute(*values,job,completion); }
  catch(const VolumeCoord::Busy& e){result={1,ERROR_BUSY,e.what()};}catch(const Storage::Cancelled&){result={1,ERROR_CANCELLED,"Cancelled; completed changes were retained"};}
  catch(const Failure& e){result={1,e.error,e.what()};}catch(const std::exception& e){result={1,ERROR_UNHANDLED_EXCEPTION,e.what()};}
  catch(...){result={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected file-operation failure"};}
  std::vector<std::string> changed;
  for(auto& item:completion.affected){try{if(auto volume=item.volume.lock())changed.push_back(volume->ResourceKey());else if(item.type==LIST_FAT)changed.push_back(VolumeCoord::HostKey(item.windowsPath));else if(!item.classicImage.empty())changed.push_back(VolumeCoord::ImageKey(item.classicImage));}catch(...){/* Keep the operation result if a removed path cannot be identified. */}}
  VolumeCoord::Changed(changed);
  {std::lock_guard<std::mutex> lock(state->mutex);state->completion=std::move(completion);}return result;
 });
 {auto& registry=Registry::Get();std::lock_guard<std::mutex> lock(registry.mutex);
  registry.entries.erase(std::remove_if(registry.entries.begin(),registry.entries.end(),[](const Registry::Entry& e){auto job=e.job.lock();return !job||job->done();}),registry.entries.end());
  registry.entries.push_back({state,job});}
 return Operation(state,std::move(job));
}
std::vector<Operation> Pending(){
 auto& registry=Registry::Get();std::lock_guard<std::mutex> lock(registry.mutex);std::vector<Operation> result;
 for(auto it=registry.entries.begin();it!=registry.entries.end();){auto state=it->state.lock();auto job=it->job.lock();
  if(!state||!job||job->done())it=registry.entries.erase(it);else{result.push_back(Operation(state,job));++it;}}
 return result;
}
void CancelPending(){for(auto& operation:Pending())operation.Job()->cancel();}
}
