// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "FileOperations.h"
#include "LegacyEngine.h"
#include "HfsPlusDisk.h"
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
void CheckMedia(const Location& item){
 if(!item.mediaGenerationKnown)return;DWORD generation=0;
 if(!physical_floppy_generation(physical_floppy_path(item.classicImage.c_str()),&generation)||generation!=item.mediaGeneration)throw Failure(ERROR_MEDIA_CHANGED,"Floppy media changed after selection; select the volume again");
}
std::pair<std::shared_ptr<Volume>,uint32_t> Source(const Location& value,bool mutate=false){
 if(!value.expectedResource.empty())Check(VolumeCoord::ImageKey(value.classicImage)==value.expectedResource,"The selected source image was replaced");
 if(value.volume){auto e=value.volume->Stat(value.id);
  if(!value.expectedIdentity.empty())Check(e.identity==value.expectedIdentity,"The selected entry was replaced before the operation started");
  if(!value.expectedName.empty())Check(e.name==value.expectedName,"The selected entry was renamed before the operation started");
  if(value.expectedParent)Check(e.parent==value.expectedParent,"The selected entry was moved before the operation started");
  return {value.volume,value.id};}
 std::shared_ptr<Volume> volume;std::u16string name;
 if(value.type==LIST_FAT){
  Check(Absolute(value.windowsPath),"Use an absolute Windows source path");
  // Resolve only the captured entry. Enumerating its parent also examines
  // unrelated junctions (for example Documents' legacy library links).
  auto source=ResolveWindowsFile(value.windowsPath);auto entry=source.first->Stat(source.second);
  if(!value.expectedIdentity.empty())Check(entry.identity==value.expectedIdentity,"The selected source was replaced or changed");return source;
 }
 else{auto pos=value.classicPath.find_last_of(':');Check(pos!=std::string::npos,"Use an absolute classic HFS path");volume=OpenClassic(value.classicImage,value.classicPath.substr(0,pos+1),mutate);CheckMedia(value);name=Ansi(value.classicPath.substr(pos+1));}
 for(auto& e:volume->List())if(HfsPlus::CompareNames(e.name,name,volume->CaseSensitive())==0){if(value.expectedCatalogId)Check(e.resolvedId==value.expectedCatalogId,"The selected classic HFS entry was replaced");if(!value.expectedIdentity.empty())Check(e.identity==value.expectedIdentity,"The selected source was replaced or changed");return {volume,e.id};}
 throw HfsPlus::Error("The selected source no longer exists");
}
std::pair<std::shared_ptr<Volume>,uint32_t> Destination(const Location& value){
 if(value.volume)return {value.volume,value.id};
 if(value.type==LIST_FAT){Check(Absolute(value.windowsPath),"Use an absolute Windows destination path");return {OpenWindows(value.windowsPath),2};}
 return {OpenClassic(value.classicImage,value.classicPath,true),2};
}
std::unique_ptr<DirectFloppy> WritableFloppy(const Location& target,bool verify=false){
 int drive=target.type==LIST_HFS?physical_floppy_path(target.classicImage.c_str()):-1;if(drive<0)return {};
 physical_floppy_suspend(drive);std::unique_ptr<DirectFloppy> disk;
#ifdef HFV_FLOPPY_TESTING
 auto api=dynamic_cast<FloppyWriteApi*>(physical_floppy_test_api(drive));Check(api!=nullptr,"No simulated writable floppy is attached");disk.reset(new DirectFloppy(api));
#else
 disk.reset(new DirectFloppy);
#endif
 if(!disk->Open(drive,verify))throw Failure(disk->Error(),"Cannot open and lock the floppy");CheckMedia(target);return disk;
}
void Finish(std::unique_ptr<DirectFloppy>& disk){HFSIFACE_get_dir_flush();if(disk&&!disk->Finish())throw Failure(disk->Error(),"Floppy write or verification failed (Windows error "+std::to_string(disk->Error())+"). The disk may be incomplete");}
void DeleteMovedSources(const std::vector<Location>& sources,const std::vector<std::string>& snapshots,Storage::Job& job,Completion& completion){
 std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> fresh(sources.size());
 for(size_t i=0;i<sources.size();++i)if(sources[i].cut){Cancel(job);bool physical=sources[i].type==LIST_HFS&&physical_floppy_path(sources[i].classicImage.c_str())>=0;
  fresh[i]=Source(sources[i],!physical);Check(MacBrowser::Snapshot(*fresh[i].first,fresh[i].second,job)==snapshots[i],"Source changed during the copy; source items were retained");if(physical)fresh[i].first->Close();
 }
 for(size_t i=0;i<sources.size();++i)if(sources[i].cut){Cancel(job);auto disk=WritableFloppy(sources[i],true);
  if(disk){fresh[i]=Source(sources[i],true);Check(MacBrowser::Snapshot(*fresh[i].first,fresh[i].second,job)==snapshots[i],"Source floppy changed before move cleanup; source items were retained");}
  Mark(completion,sources[i]);RemoveTree(*fresh[i].first,fresh[i].second,job);if(!sources[i].volume)fresh[i].first->Close();Finish(disk);
 }
}
bool MoveInPlace(const Request& request,const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>>& sources,Volume& target,uint32_t parent,Storage::Job& job,Completion& completion){
 if(!target.Supports().rename)return false;
 for(size_t i=0;i<sources.size();++i)if(!request.sources[i].cut||!target.CanMoveFrom(*sources[i].first))return false;
 auto existing=target.List(parent);std::vector<Entry> entries;
 for(size_t i=0;i<sources.size();++i){auto& source=*sources[i].first;auto id=sources[i].second;Check(id!=2,"Select files or folders rather than a volume root");source.ValidateCopyTo(id,target,parent);auto entry=source.Stat(id);
  for(auto& e:existing)if(HfsPlus::CompareNames(e.name,entry.name,target.CaseSensitive())==0)return false;
  for(auto& e:entries)if(HfsPlus::CompareNames(e.name,entry.name,target.CaseSensitive())==0)return false;
  if(request.sources[i].volume&&entry.directory){std::set<uint32_t> visited;for(uint32_t at=parent;at!=1;at=target.Stat(at).parent){Check(at!=id&&visited.insert(at).second,"Cannot move a folder into itself or its descendants");}}
  Check(!entry.protectedFile&&!entry.hardLink&&!entry.compressed&&entry.readError.empty()&&!(HfsPlus::Disk::U16(entry.finder.data()+8)&HFS_FNDR_NAMELOCKED),"The selected entry cannot be moved");entries.push_back(entry);
 }
 // Classic adapters use root-relative paths. Avoid invalidating a later
 // captured descendant path by renaming its selected ancestor first.
 for(size_t i=0;i<sources.size();++i)for(size_t j=0;j<sources.size();++j)if(i!=j&&!request.sources[i].volume){auto a=request.sources[i].classicPath,b=request.sources[j].classicPath;if(b.size()>a.size()&&b[a.size()]==':'&&!_strnicmp(a.c_str(),b.c_str(),a.size()))return false;}
 Storage::Progress progress;progress.phase="Moving";progress.scanning=false;progress.fileCopy=progress.utf8=true;progress.totalFiles=unsigned(sources.size());job.progress(progress);
 for(size_t i=0;i<sources.size();++i){Cancel(job);Mark(completion,request.sources[i]);progress.path=Utf8(entries[i].name);job.progress(progress);target.MoveHere(*sources[i].first,sources[i].second,parent,entries[i]);++progress.files;job.progress(progress);}
 return true;
}
Storage::Result CopyRequest(const Request& request,Storage::Job& job,Completion& completion){
 Check(!request.sources.empty(),"Select a source file or folder");HFSIFACE_get_dir_flush();auto disk=WritableFloppy(request.target,std::any_of(request.sources.begin(),request.sources.end(),[](const Location& p){return p.cut;}));
 auto destination=Destination(request.target);Check(destination.first->Supports().create,"The destination does not support file creation");
 std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> opened;std::vector<std::string> snapshots;
 for(auto& item:request.sources){Cancel(job);auto source=Source(item);if(item.cut&&item.volume)Check(source.first->Writable(),"A cut source is read-only");opened.push_back(source);}
 Mark(completion,request.target);
 if(MoveInPlace(request,opened,*destination.first,destination.second,job,completion)){
  if(!request.target.volume)destination.first->Close();for(size_t i=0;i<request.sources.size();++i)if(!request.sources[i].volume)opened[i].first->Close();Finish(disk);completion.complete=true;completion.completed=unsigned(opened.size());Storage::Result result;result.moved=true;return result;
 }
 for(size_t i=0;i<request.sources.size();++i)snapshots.push_back(request.sources[i].cut?MacBrowser::Snapshot(*opened[i].first,opened[i].second,job,bool(request.sources[i].volume)):std::string());
 bool complete;try{complete=CopyMany(opened,*destination.first,destination.second,job);}catch(...){if(disk&&disk->Error())throw Failure(disk->Error(),"Floppy write or verification failed (Windows error "+std::to_string(disk->Error())+"). The disk may be incomplete");throw;}
 if(!request.target.volume)destination.first->Close();
 for(size_t i=0;i<request.sources.size();++i)if(!request.sources[i].volume)opened[i].first->Close();
 Finish(disk);disk.reset(); // Includes all final unmount writes; never delete sources before this.
 if(complete)DeleteMovedSources(request.sources,snapshots,job,completion);
 completion.complete=complete;completion.completed=job.progress().files;
 Storage::Result result;result.moved=complete&&std::any_of(request.sources.begin(),request.sources.end(),[](const Location& p){return p.cut;});return result;
}
Storage::Result Mutate(const Request& request,Storage::Job& job,Completion& completion){
 Check(!request.sources.empty(),"Select a file or folder");if(request.kind==Kind::Rename)Check(request.sources.size()==1,"Rename requires one selected entry");
 Storage::Progress progress;progress.fileCopy=false;progress.phase=request.kind==Kind::Rename?"Renaming":"Deleting";progress.scanning=false;progress.totalFiles=unsigned(request.sources.size());progress.utf8=true;job.progress(progress);
 HFSIFACE_get_dir_flush();int silent=HFSIFACE_silencer(1);HFSIFACE_delete_cancel(CancelClassic,&job);struct Silent{int previous;~Silent(){HFSIFACE_delete_cancel(nullptr,nullptr);HFSIFACE_silencer(previous);}}scope{silent};
 for(auto& item:request.sources){Cancel(job);progress.path=item.volume?Utf8(item.expectedName):item.type==LIST_FAT?Utf8(U16(item.windowsPath)):Utf8(Ansi(item.classicPath));job.progress(progress);
  if(item.volume){auto source=Source(item);auto e=source.first->Stat(source.second);Cancel(job);Check(e.id!=2,"Cannot rename or delete a volume root");
   if(request.kind==Kind::Rename){Check(!request.target.volume||request.target.volume==source.first,"Rename must stay within the same volume");Check(source.first->Supports().rename&&!e.protectedFile&&!e.hardLink&&!e.compressed,"The selected entry cannot be renamed");Mark(completion,item);source.first->Rename(source.second,request.target.id,request.name);}
   else{Check(source.first->Supports().remove,"The selected entry cannot be deleted");Mark(completion,item);RemoveTree(*source.first,source.second,job,true);}
  }else if(item.type==LIST_FAT){
   auto source=Source(item,true);Cancel(job);source.first->Close();
   std::string path; // ANSI legacy path is retained verbatim.
   int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,item.windowsPath.c_str(),-1,nullptr,0,nullptr,nullptr);std::vector<char> converted(n);BOOL lost=FALSE;if(n)WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,item.windowsPath.c_str(),-1,converted.data(),n,nullptr,&lost);Check(n&&!lost,"Windows path cannot be represented by the legacy backend");path=converted.data();Check(Absolute(path),"Use an absolute Windows path");
   Mark(completion,item);
   if(request.kind==Kind::Rename){auto entry=source.first->Stat(source.second);Cancel(job);source.first->Rename(source.second,entry.parent,request.name);
   }else if(!DeleteHostSelectionJob(CString(path.c_str()),item.directory,&job)){Cancel(job);throw Failure(GetLastError(),"Could not completely delete the selected Windows entry; remaining files were retained");}
  }else{
   Check(item.type==LIST_HFS&&!item.classicImage.empty(),"Invalid classic HFS source");if(!item.expectedResource.empty())Check(VolumeCoord::ImageKey(item.classicImage)==item.expectedResource,"The selected source image was replaced");auto disk=WritableFloppy(item);auto image=item.classicImage,path=item.classicPath;hfsdirent entry{};
   if(HFSIFACE_get_properties(&image[0],&path[0],&entry))throw HfsPlus::Error(ClassicError("Cannot read the selected HFS entry"));
   Cancel(job);if(item.expectedCatalogId)Check(uint32_t(entry.cnid)==item.expectedCatalogId,"The selected classic HFS entry was replaced");Check(entry.cnid!=2,"Cannot rename or delete a volume root");
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
 if(request.kind==Kind::LegacyCopy){auto key=[](const std::string& volume,const std::string& path){return Absolute(path)?VolumeCoord::HostKey(VolumeCoord::Wide(path)):VolumeCoord::ImageKey(volume);};access.push_back({key(request.sourceVolume,request.sourcePath),(request.move||(request.background&&!Absolute(request.sourcePath)))?Mode::Write:Mode::Read});access.push_back({key(request.targetVolume,request.targetPath),Mode::Write});return access;}
 for(auto& source:request.sources)access.push_back({Resource(source),(request.kind==Kind::Rename||request.kind==Kind::Delete||source.cut||(request.background&&source.type!=LIST_FAT))?Mode::Write:Mode::Read});
 if(request.kind==Kind::Copy||request.kind==Kind::ReplaceResource||request.kind==Kind::ExportFork)access.push_back({Resource(request.target),Mode::Write});return access;
}
Storage::Result Execute(const Request& request,Storage::Job& job,Completion& completion){
 Cancel(job);
 if(request.kind==Kind::Copy)return CopyRequest(request,job,completion);
 if(request.kind==Kind::ExportFork)return ExportFork(request,job,completion);
 if(request.kind==Kind::ReplaceResource)return ReplaceResource(request,job,completion);
 if(request.kind!=Kind::LegacyCopy)return Mutate(request,job,completion);
 if(!request.sources.empty()){
  // Shared browser selections use captured paths/identities, never another
  // window's volume indexes. Validate before invoking the old converters.
  auto source=Source(request.sources.front());
  auto target=request.target.type==LIST_FAT?OpenWindows(request.target.windowsPath):OpenClassic(request.target.classicImage,request.target.classicPath,false);
  CheckMedia(request.target);source.first->ValidateCopyTo(source.second,*target,2);
  source.first->Close();target->Close();
 }
  auto describe=[](const std::string& volume,const std::string& path){Location out;out.type=Absolute(path)?LIST_FAT:LIST_HFS;if(out.type==LIST_FAT)out.windowsPath=Wide(Ansi(path));else{out.classicImage=volume;out.classicPath=path;}return out;};
 Mark(completion,describe(request.targetVolume,request.targetPath));if(request.move)Mark(completion,describe(request.sourceVolume,request.sourcePath));
 auto sourceLocation=request.sources.empty()?describe(request.sourceVolume,request.sourcePath):request.sources.front();sourceLocation.cut=request.move;
 bool physicalMove=request.move&&((sourceLocation.type==LIST_HFS&&physical_floppy_path(sourceLocation.classicImage.c_str())>=0)||physical_floppy_path(request.targetVolume.c_str())>=0);
 std::vector<std::string> snapshots;auto options=request.conversion;options.targetGenerationKnown=request.target.mediaGenerationKnown;options.targetGeneration=request.target.mediaGeneration;
 if(physicalMove){auto source=Source(sourceLocation);snapshots.push_back(MacBrowser::Snapshot(*source.first,source.second,job));if(!sourceLocation.volume)source.first->Close();options.verifyFloppy=true;}
 auto result=MakeCopyJob(request.sourceVolume,request.sourcePath,request.targetVolume,request.targetPath,request.directory,physicalMove?0:request.move,options)(job);
 completion.completed=job.progress().files;bool copied=!result.code&&!job.progress().skipped;
 if(physicalMove&&copied){DeleteMovedSources({sourceLocation},snapshots,job,completion);result.moved=true;}
 completion.complete=copied;
 return result;
}
}
Operation::Operation(std::shared_ptr<State> state,std::shared_ptr<Storage::Job> job):state_(std::move(state)),job_(std::move(job)){}
Completion Operation::Snapshot()const{std::lock_guard<std::mutex> lock(state_->mutex);return state_->completion;}
Operation Submit(Request request){
 auto state=std::make_shared<Operation::State>();auto values=std::make_shared<const Request>(std::move(request));
 auto& queue=values->background?Storage::copyWorker():Storage::worker();
 auto access=std::make_shared<std::unique_ptr<VolumeCoord::Lease>>();auto resources=Resources(*values);
 auto job=queue.submit([state,values,access](Storage::Job& job){Completion completion;Storage::Result result;
  try{Cancel(job);if(!*access)throw VolumeCoord::Busy();LegacyEngine::Scope engine;
   result=Execute(*values,job,completion);
  }
  catch(const VolumeCoord::Busy& e){result={1,ERROR_BUSY,e.what()};}catch(const Storage::Cancelled&){result={1,ERROR_CANCELLED,"Cancelled; completed changes were retained"};}
  catch(const Failure& e){result={1,e.error,e.what()};}catch(const std::exception& e){result={1,ERROR_UNHANDLED_EXCEPTION,e.what()};}
  catch(...){result={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected file-operation failure"};}
  std::vector<std::string> changed;
  for(auto& item:completion.affected){try{if(auto volume=item.volume.lock())changed.push_back(volume->ResourceKey());else if(item.type==LIST_FAT)changed.push_back(VolumeCoord::HostKey(item.windowsPath));else if(!item.classicImage.empty())changed.push_back(VolumeCoord::ImageKey(item.classicImage));}catch(...){/* Keep the operation result if a removed path cannot be identified. */}}
  access->reset();VolumeCoord::Changed(changed);
  {std::lock_guard<std::mutex> lock(state->mutex);state->completion=std::move(completion);}return result;
 },[access,resources,values](const std::shared_ptr<Storage::Job>& job){
  if(job->cancelled())return true;
  try{access->reset(new VolumeCoord::Lease(resources));return true;}catch(const VolumeCoord::Busy&){return !values->background;}
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
