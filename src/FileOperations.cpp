// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "AttributeReadScope.h"
#include "FileOperations.h"
#include "ClassicHfsRecovery.h"
#include "WindowsMoveUndo.h"
#include "FileOperationWorkerPolicy.h"
#include "OperationCompletion.h"
#include "StorageLocation.h"
#include "BrowserPresentation.h"
#include "CopyProcess.h"
#include "VolumeSession.h"
#include "WorkerEndpoint.h"
#include "TransferConversion.h"
#include "TypeMappingSnapshot.h"
#include "LegacyEngine.h"
#include "HfsPlusDisk.h"
#include "VolumeCoordinator.h"
#include "RenameName.h"
#include "charset.h"
#include <algorithm>
#ifdef HFV_TESTING
#include <atomic>
#endif
#include <set>
#include <string_view>
#include <fcntl.h>
#include <optional>
extern int DeleteHostSelectionJob(const std::wstring&,bool,Storage::Job*);
namespace FileOperations {
struct Operation::State {mutable std::mutex mutex;Completion completion;};
struct Registry {
 struct Entry{std::weak_ptr<Operation::State> state;std::weak_ptr<Storage::Job> job;};
 std::mutex mutex;std::vector<Entry> entries;
 static Registry& Get(){static Registry registry;return registry;}
};
namespace {
using namespace MacBrowser;
using Storage::OperationError;
void Check(bool ok,const char* text){if(!ok)throw HfsPlus::Error(text);}
std::wstring HostPath(const Location& item){return item.HostPath();}
std::wstring ChildPath(std::wstring root,const std::u16string& name){if(!root.empty()&&root.back()!=L'\\')root+=L'\\';return root+MacBrowser::Wide(name);}
std::wstring CompanionPath(const std::wstring& path){auto at=path.find_last_of(L"\\/");return path.substr(0,at+1)+L"%"+path.substr(at+1);}
void Mark(Completion& c,const Location& item){if(!item.IsNone())c.affected.push_back({item.ResourceKey()});}
void MarkChild(Completion& c,const Location& folder,const std::u16string& name){auto path=HostPath(folder);if(path.empty()){Mark(c,folder);return;}Location child;child.SetWindows(ChildPath(path,name));Mark(c,child);}
void Cancel(Storage::Job& job){if(job.cancelled())throw Storage::Cancelled();}
std::u16string Ansi(const std::string& s){int n=MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),nullptr,0);std::wstring w(n,0);if(n)MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),&w[0],n);return U16(w);}
std::string Utf8(const std::u16string& text){auto w=Wide(text);int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);std::string out(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),&out[0],n,nullptr,nullptr);return out;}
std::pair<std::shared_ptr<Volume>,uint32_t> Source(const Location& value,bool mutate=false){
 auto entry=StorageLocation::ResolveEntry(value,mutate);
 return {std::move(entry.volume),entry.id};
}
std::pair<std::shared_ptr<Volume>,uint32_t> Destination(const Location& value){
 if(value.Volume())return {value.Volume(),value.Id()};
 if(value.IsWindows()){
  StorageLocation::ValidateMedia(value);
  auto volume=OpenWindows(value.WindowsPath());
  if(!value.expectedIdentity.empty())StorageLocation::ValidateFolderIdentity(value,volume->Stat(2));
  return {volume,2};
 }
 return {OpenClassic(value.ClassicImage(),value.ClassicPath(),true),2};
}
void DeleteMovedSources(const std::vector<Location>& sources,const std::vector<std::string>& snapshots,Storage::Job& job,Completion& completion){
 std::vector<StorageLocation::ResolvedEntry> checked(sources.size());
 for(size_t i=0;i<sources.size();++i)
  if(sources[i].cut)checked[i]=StorageLocation::PrepareCutSource(sources[i],snapshots[i],job);
 for(size_t i=0;i<sources.size();++i)
  if(sources[i].cut)StorageLocation::FinalizeCutSource(sources[i],std::move(checked[i]),snapshots[i],job,
    [&](Volume& volume,uint32_t id){
     Mark(completion,sources[i]);
     if(sources[i].IsWindows()&&WindowsMoveUndo::Session::Current())StageWindowsCut(volume,id,job);
     else RemoveTree(volume,id,job);
    });
}
bool MoveInPlace(const OrdinaryCopy& request,const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>>& sources,Volume& target,uint32_t parent,Storage::Job& job,Completion& completion){
 if(!target.Supports().rename)return false;
 for(size_t i=0;i<sources.size();++i)if(!request.sources[i].cut||!target.CanMoveEntryFrom(*sources[i].first,sources[i].second,parent))return false;
 auto existing=target.List(parent);std::vector<Entry> entries;
 for(size_t i=0;i<sources.size();++i){auto& source=*sources[i].first;auto id=sources[i].second;Check(id!=2,"Select files or folders rather than a volume root");source.ValidateCopyTo(id,target,parent);auto entry=source.Stat(id);
  for(auto& e:existing)if(HfsPlus::CompareNames(e.name,entry.name,target.CaseSensitive())==0)return false;
  for(auto& e:entries)if(HfsPlus::CompareNames(e.name,entry.name,target.CaseSensitive())==0)return false;
  if(request.sources[i].Volume()&&entry.directory){std::set<uint32_t> visited;for(uint32_t at=parent;at!=1;at=target.Stat(at).parent){Check(at!=id&&visited.insert(at).second,"Cannot move a folder into itself or its descendants");}}
  Check(!entry.protectedFile&&(entry.readError.empty()||entry.symbolicLink)&&!(HfsPlus::Disk::U16(entry.finder.data()+8)&HFS_FNDR_NAMELOCKED),"The selected entry cannot be moved");entries.push_back(entry);
 }
 // Classic adapters use root-relative paths. Avoid invalidating a later
 // captured descendant path by renaming its selected ancestor first.
 for(size_t i=0;i<sources.size();++i)for(size_t j=0;j<sources.size();++j)if(i!=j&&request.sources[i].IsClassic()&&request.sources[j].IsClassic()){auto a=request.sources[i].ClassicPath(),b=request.sources[j].ClassicPath();if(b.size()>a.size()&&b[a.size()]==':'&&!_strnicmp(a.c_str(),b.c_str(),a.size()))return false;}
 Storage::Progress progress;progress.phase="Moving";progress.scanning=false;progress.fileCopy=progress.utf8=true;progress.totalFiles=unsigned(sources.size());job.progress(progress);
 const auto move=[&](bool){
  for(size_t i=0;i<sources.size();++i){
   Cancel(job);
   Mark(completion,request.sources[i]);
   progress.path=Utf8(entries[i].name);
   job.progress(progress);
   target.MoveHereForJob(*sources[i].first,sources[i].second,parent,entries[i],job);
   Storage::OutcomeEvidence::NoteRetainedChange();
   ++progress.files;
   job.progress(progress);
  }
 };
 if(sources.size()>1&&!strcmp(target.Kind(),"HFS"))target.RunWriteBatch(job,move);
 else move(false);
 return true;
}
enum class ExecutionRoute { Local, TemporaryWorker };
struct MetadataPlan {
 // The immutable request owns this location and the handler's typed payload.
 const Location* item;
 VolumeCoord::Mode access;
 bool temporaryForUnmounted;
 bool includeCompanion;
 std::function<void(Storage::Job&,Completion&)> execute;
};
struct TransferExecution {
 ExecutionRoute route;
 std::vector<MacBrowser::ForkSource> sources;
 std::function<std::vector<VolumeCoord::Access>()> bind;
 std::function<Storage::Result(const std::vector<MacBrowser::ForkSource>&,Storage::Job&,Completion&)> run;
 std::function<void(Completion&)> markHandoff;
};
struct MutationPlan {
 ExecutionRoute route;
 std::function<std::vector<VolumeCoord::Access>()> bind;
 std::function<Storage::Result(Storage::Job&,Completion&)> run;
};
struct ExecutionPlan {
 ExecutionRoute route=ExecutionRoute::Local;
 std::vector<VolumeCoord::Access> resources;
 std::optional<TransferExecution> transfer;
 std::optional<MutationPlan> mutation;
 std::optional<MetadataPlan> metadata;
};

struct TransferPlan {
 OrdinaryCopy endpoints;
 TransferAdapter adapter;
};

bool CanCoordinateClassicMove(const OrdinaryCopy& copy)
{
 const auto first=copy.sources.front().Volume();
 const auto target=copy.destination.Volume();
 if(!first||!target||first->ResourceKey()==target->ResourceKey()||
    strcmp(first->Kind(),"HFS")||strcmp(target->Kind(),"HFS"))return false;
 const auto image=[](Volume& volume){return !ExecutionFor(volume).processLocal;};
 const auto removable=[](Volume& volume){
  const auto* media=dynamic_cast<const Storage::MediaWriteBackend*>(&volume);
  return media&&media->SupportsCoordinatedMove();
 };
 if(image(*first)){
  if(!image(*target)&&!removable(*target))return false;
 }else if(!removable(*first)||!image(*target))return false;
 return std::all_of(copy.sources.begin(),copy.sources.end(),[&](const Location& item){
  return item.cut&&item.Volume()==first;
 });
}
bool CanCoordinateWindowsMove(const OrdinaryCopy& copy,
                              const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>>& sources,
                              const Volume& target)
{
 const auto classicImage=[](const Volume& volume){
  return !strcmp(volume.Kind(),"HFS")&&volume.Writable()&&!ExecutionFor(volume).processLocal;
 };
 const auto first=copy.sources.front().Volume();
 if(copy.destination.IsWindows()&&first&&classicImage(*first)){
  return WindowsMoveUndo::Supported(target.HostPath(2))&&
         std::all_of(copy.sources.begin(),copy.sources.end(),[&](const Location& item){
   return item.cut&&item.Volume()==first;
  });
 }
 if(!copy.destination.Volume()||!classicImage(target)||!copy.sources.front().IsWindows())return false;
 const auto root=sources.front().first->HostPath(2);
 if(!WindowsMoveUndo::Supported(root))return false;
 for(size_t i=0;i<sources.size();++i){
  if(!copy.sources[i].cut||!copy.sources[i].IsWindows()||
     _wcsicmp(root.c_str(),sources[i].first->HostPath(2).c_str())!=0)return false;
 }
 return true;
}
void CoordinateClassicMove(Volume& source,Volume& target,Storage::Job& job,
                           const std::function<void()>& transfer)
{
 ClassicHfsRecovery::MoveDecision decision;
 const auto previous=job.progress();
 bool deciding=false;
 bool retained=false;
 Storage::Rollback rollback;
 std::exception_ptr failure;
 {
  Storage::OutcomeEvidence evidence;
  try{
   source.WithCopyTransaction([&](bool sourceActive){
    Check(sourceActive,"The source cannot join a durable classic HFS move");
    target.WithCopyTransaction([&](bool targetActive){
     Check(targetActive,"The destination cannot join a durable classic HFS move");
     transfer();
     source.PrepareCopyTransaction();
     target.PrepareCopyTransaction();
     Cancel(job);
     deciding=true;
     decision.Commit();
    },decision.Id());
   },decision.Id());
  }catch(...){failure=std::current_exception();}
  rollback=evidence.RollbackState();
  retained=evidence.RetainedChanges();
 }
 Storage::OutcomeEvidence::NoteRollback(rollback);
 if(failure){
  if(!deciding&&rollback==Storage::Rollback::Restored)job.progress(previous);
  else if(retained)Storage::OutcomeEvidence::NoteRetainedChange();
  std::rethrow_exception(failure);
 }
 if(retained)Storage::OutcomeEvidence::NoteRetainedChange();
 decision.Forget();
}

Storage::Result RunTransfer(const TransferPlan& transfer,const std::vector<ForkSource>& routes,
                            Storage::Job& job,Completion& completion){
 const auto& request=transfer.endpoints;
 const auto& target=request.destination;
 Check(!request.sources.empty(),"Select a source file or folder");
 const bool moving=std::any_of(request.sources.begin(),request.sources.end(),[](const Location& item){return item.cut;});
 std::vector<std::string> snapshots;
 bool coordinated=!transfer.adapter&&CanCoordinateClassicMove(request);
 bool complete=false,inPlace=false;
 const auto run=[&]{
  if(!target.IsWindows())StorageLocation::ValidateMedia(target);
  if(target.IsClassic())StorageLocation::ValidateImage(VolumeCoord::Wide(target.ClassicImage()),target.expectedResource);
  auto destination=Destination(target);
  const auto folder=destination.first->Stat(destination.second);
  Check(folder.directory&&destination.first->Supports().create,"The destination does not support file creation");
  if(target.IsClassic()&&target.expectedCatalogId)
   Check(folder.resolvedId==target.expectedCatalogId,"The destination HFS folder was replaced");

  std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> opened;
  for(const auto& item:request.sources){
   Cancel(job);
   auto source=Source(item);
   if(item.cut&&item.Volume())
    Check(source.first->Writable()||(!transfer.adapter&&
         destination.first->CanMoveEntryFrom(*source.first,source.second,destination.second)),
         "A cut source is read-only");
   opened.push_back(std::move(source));
  }
  if(transfer.adapter)Mark(completion,target);
  else for(const auto& item:opened)MarkChild(completion,target,item.first->Stat(item.second).name);

  // A transform may change the name or forks, so it must run through CopyMany.
  inPlace=!transfer.adapter&&MoveInPlace(request,opened,*destination.first,destination.second,job,completion);
  if(!inPlace){
   snapshots.reserve(opened.size());
   for(size_t i=0;i<request.sources.size();++i){
    const auto& item=request.sources[i];
    if(item.cut&&item.Volume())Check(opened[i].first->Writable(),"This cut source requires an in-place move");
    snapshots.push_back(item.cut?MacBrowser::Snapshot(*opened[i].first,opened[i].second,job,bool(item.Volume())):std::string());
   }
   const bool windows=CanCoordinateWindowsMove(request,opened,*destination.first);
   coordinated=coordinated||windows;
   if(coordinated){
    auto& source=*opened.front().first;
    auto& dest=*destination.first;
    const bool windowsSource=windows&&request.sources.front().IsWindows();
    // Keep the HFS recovery record until the Windows participant finishes cleanup.
    CoordinateClassicMove(windowsSource?dest:source,windowsSource?source:dest,job,[&]{
     complete=CopyMany(opened,dest,destination.second,job,transfer.adapter,routes,true,true);
     if(complete){
      if(windowsSource)for(const auto& entry:opened)entry.first->Close();
      DeleteMovedSources(request.sources,snapshots,job,completion);
     }
    });
   }else if(!moving&&target.IsWindows()&&
            std::all_of(opened.begin(),opened.end(),[](const auto& item){return !strcmp(item.first->Kind(),"HFS");})&&
            WindowsMoveUndo::Supported(destination.first->HostPath(2))){
    const auto previous=job.progress();
    Storage::Rollback rollback;
    bool retained;
    std::exception_ptr failure;
    {
     Storage::OutcomeEvidence evidence;
     try{
      WindowsMoveUndo::RunExport(destination.first->HostPath(2),[&]{
       complete=CopyMany(opened,*destination.first,destination.second,job,transfer.adapter,routes,false,true);
       destination.first->Close();
       Cancel(job);
      });
     }catch(...){failure=std::current_exception();}
     rollback=evidence.RollbackState();
     retained=evidence.RetainedChanges();
    }
    Storage::OutcomeEvidence::NoteRollback(rollback);
    if(failure&&rollback==Storage::Rollback::Restored)job.progress(previous);
    else if(retained)Storage::OutcomeEvidence::NoteRetainedChange();
    if(failure)std::rethrow_exception(failure);
   }else complete=CopyMany(opened,*destination.first,destination.second,job,transfer.adapter,routes,moving);
  }
  if(!target.Volume())destination.first->Close();
  for(size_t i=0;i<request.sources.size();++i)if(!request.sources[i].Volume())opened[i].first->Close();
 };
 // The coordinator opens both write scopes and retains their undo until it decides.
 if(coordinated)run();
 else StorageLocation::WithVerifiedWrite(target,run);
 if(inPlace){
  completion.complete=true;
  completion.completed=unsigned(request.sources.size());
  Storage::Result result;
  result.moved=true;
  return result;
 }
 if(!coordinated){
  // Without a shared decision, keep the source through destination close and readback.
  for(const auto& source:request.sources)
   StorageLocation::ValidateMedia(source,StorageLocation::FloppyProbe::FreshHandle);
  if(complete)DeleteMovedSources(request.sources,snapshots,job,completion);
 }
 completion.complete=complete;
 completion.completed=job.progress().files;
 Storage::Result result;
 result.moved=complete&&moving;
 return result;
}
Storage::Result Mutate(const RenameEntry* rename, const Location* items, size_t count,Storage::Job& job,Completion& completion){
 Storage::Progress progress;progress.fileCopy=false;progress.phase=rename?"Renaming":"Deleting";progress.scanning=false;progress.totalFiles=unsigned(count);progress.utf8=true;job.progress(progress);
 const auto process=[&](size_t index,bool grouped){const auto& item=items[index];Cancel(job);progress.path=item.Volume()?Utf8(item.expectedName):item.IsWindows()?Utf8(U16(item.WindowsPath())):Utf8(Ansi(item.ClassicPath()));job.progress(progress);
  if(item.IsMounted()||item.IsClassic()){
   auto mutate=[&]{
    auto source=StorageLocation::ResolveEntry(item,item.IsClassic());
    auto& volume=*source.volume;
    const auto& entry=source.entry;
    Cancel(job);
    Check(entry.id!=2 && entry.resolvedId!=2,"Cannot rename or delete a volume root");
    if(rename){
     Check(rename->parent.IsNone()||(rename->parent.IsMounted()&&rename->parent.Volume()==source.volume),
           "Rename must stay within the same mounted volume");
     Check(volume.Supports().rename&&(!item.IsMounted()||!entry.protectedFile),"The selected entry cannot be renamed");
     if(rename->checkNameLock&&!strcmp(volume.Kind(),"HFS"))
      Check(!(HfsPlus::Disk::U16(entry.finder.data()+8)&HFS_FNDR_NAMELOCKED),
            "Remove the Finder name lock before renaming this entry");
     Mark(completion,item);
     auto path=HostPath(item);
     if(!path.empty()){Location parent;parent.SetWindows(path.substr(0,path.find_last_of(L"\\/")+1));MarkChild(completion,parent,rename->name);}
     volume.RenameForJob(source.id,rename->parent.IsNone()?entry.parent:rename->parent.Id(),rename->name,job);
     Storage::OutcomeEvidence::NoteRetainedChange();
    }else{
     Check(volume.Supports().remove,"The selected entry cannot be deleted");
     Mark(completion,item);
     RemoveTree(volume,source.id,job,true);
    }
    if(item.IsClassic())volume.Close();
   };
   if(grouped)mutate();
   else StorageLocation::WithVerifiedWrite(item,mutate);
  }else if(item.IsWindows()){
   auto source=Source(item,true);Cancel(job);source.first->Close();
   Mark(completion,item);
   if(rename){Location parent;parent.SetWindows(item.WindowsPath().substr(0,item.WindowsPath().find_last_of(L"\\/")+1));MarkChild(completion,parent,rename->name);auto entry=source.first->Stat(source.second);Cancel(job);source.first->Rename(source.second,entry.parent,rename->name);
   }else if(!DeleteHostSelectionJob(item.WindowsPath(),item.directory,&job)){Cancel(job);throw OperationError(GetLastError(),"Could not completely delete the selected Windows entry; remaining files were retained");}
  }
  if(!grouped)++completion.completed;
  ++progress.files;
  job.progress(progress);
 };
 for(size_t first=0;first<count;){
  size_t end=first+1;
  auto volume=items[first].Volume();
  if(!rename&&volume&&!strcmp(volume->Kind(),"HFS")&&!ExecutionFor(*volume).processLocal)
   while(end<count&&items[end].Volume()==volume)++end;
  if(end==first+1)process(first,false);
  else StorageLocation::WithVerifiedWrite(items[first],[&]{
   bool grouped=false;
   volume->RunWriteBatch(job,[&](bool active){
    grouped=active;
    for(auto index=first;index<end;++index)process(index,active);
   });
   if(grouped)completion.completed+=unsigned(end-first);
  });
  first=end;
 }
 completion.complete=true;return {};
}
Storage::Result ExportFork(const ExportFileFork& request,Storage::Job& job,Completion& completion){
 const auto fork=static_cast<unsigned>(request.fork);auto source=Source(request.source);auto entry=source.first->Stat(source.second);Check(!entry.directory,"Select a file for fork export");
 source.first->Read(source.second,fork,0,nullptr,0);auto& path=request.output.WindowsPath();
 HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw OperationError(GetLastError(),"Cannot create output; choose a new filename");
 try{Mark(completion,request.output);std::vector<uint8_t> bytes(1024*1024);Storage::Progress progress;progress.fileCopy=progress.byteTransfer=progress.utf8=true;progress.scanning=false;progress.phase="Exporting fork";progress.totalFiles=1;progress.path=Utf8(entry.name);progress.totalBytes=progress.fileTotal=entry.forks[fork].size;
  for(uint64_t at=0;at<entry.forks[fork].size;){Cancel(job);size_t n=size_t((std::min)(uint64_t(bytes.size()),entry.forks[fork].size-at));source.first->Read(source.second,fork,at,bytes.data(),n);DWORD written;if(!WriteFile(file,bytes.data(),DWORD(n),&written,nullptr)||written!=n)throw OperationError(GetLastError(),"Fork export write failed");at+=n;progress.bytes=progress.fileBytes=progress.processed=at;job.progress(progress);}
  Cancel(job);
  if(!FlushFileBuffers(file))throw OperationError(GetLastError(),"Fork export flush failed");
  CloseHandle(file);
 }catch(...){
  FILE_DISPOSITION_INFO remove{TRUE};
  const bool deleted=SetFileInformationByHandle(file,FileDispositionInfo,&remove,sizeof(remove))!=FALSE;
  const auto error=GetLastError();
  CloseHandle(file);
  if(!deleted){Storage::OutcomeEvidence::NoteRetainedChange();throw OperationError(error,"Could not remove the incomplete fork export");}
  throw;
 }
 completion.complete=true;completion.completed=1;return {};
}
Storage::Result ReplaceResource(const ImportResourceFork& request,Storage::Job& job,Completion& completion){
 auto from=Source(request.input),target=Source(request.file);auto volume=target.first;uint32_t id=target.second;auto original=volume->Stat(id),replacement=original;Check(!original.directory,"Select a file for resource import");
 const auto input=from.first->Stat(from.second);
 Check(!input.directory,"Select a file containing the replacement resource fork");
 replacement.forks[1].size=input.forks[0].size;replacement.modified=uint64_t(time(nullptr))+2082844800ULL;Cancel(job);
 Storage::Progress progress;progress.fileCopy=progress.byteTransfer=progress.utf8=true;progress.scanning=false;progress.phase="Importing resource fork";progress.totalFiles=1;progress.path=Utf8(original.name);progress.totalBytes=progress.fileTotal=replacement.forks[0].size+replacement.forks[1].size;
 uint64_t readThrough[2]={};Mark(completion,request.file);
 auto report=[&](unsigned fork,uint64_t at,size_t n){
  Cancel(job);
  auto end=at+n,added=end>readThrough[fork]?end-readThrough[fork]:0;
  readThrough[fork]=(std::max)(readThrough[fork],end);
  progress.bytes+=added;progress.fileBytes+=added;progress.processed+=added;
  job.progress(progress);
  Cancel(job);
 };
 volume->PutResourceReplacementForJob(id,replacement,[&](unsigned fork,uint64_t at,void* data,size_t n){
  Cancel(job);
  if(fork)from.first->Read(from.second,0,at,data,n);
  else volume->Read(id,0,at,data,n);
  report(fork,at,n);
 },[&](uint64_t at,size_t n){report(0,at,n);},job);
 if(!request.input.Volume())from.first->Close();completion.complete=true;completion.completed=1;return {};
}

bool SameProperties(const hfsdirent& a,const hfsdirent& b){
 if(strcmp(a.name,b.name)||a.flags!=b.flags||a.cnid!=b.cnid||a.parid!=b.parid||a.crdate!=b.crdate||a.mddate!=b.mddate||a.bkdate!=b.bkdate||a.fdflags!=b.fdflags||a.fdlocation.h!=b.fdlocation.h||a.fdlocation.v!=b.fdlocation.v)return false;
 if(a.flags&HFS_ISDIR)return a.u.dir.valence==b.u.dir.valence&&a.u.dir.rect.top==b.u.dir.rect.top&&a.u.dir.rect.left==b.u.dir.rect.left&&a.u.dir.rect.bottom==b.u.dir.rect.bottom&&a.u.dir.rect.right==b.u.dir.rect.right;
 return a.u.file.dsize==b.u.file.dsize&&a.u.file.rsize==b.u.file.rsize&&!memcmp(a.u.file.type,b.u.file.type,4)&&!memcmp(a.u.file.creator,b.u.file.creator,4);
}
void MetadataProgress(Storage::Job& job,const char* phase,const std::u16string& name={})
{
 Storage::Progress progress;
 progress.phase=phase;
 progress.utf8=true;
 if(!name.empty())progress.path=Utf8(name);
 job.progress(progress);
}

std::pair<std::shared_ptr<Volume>,uint32_t> ReadProperties(const Location& item,bool editingClassic,
                                                         Storage::Job& job,Completion& result)
{
 auto source=Source(item,item.IsClassic()&&editingClassic);
 auto& [volume,id]=source;
 result.capabilities=volume->Supports();
 Check(result.capabilities.read,"This volume does not support reading Properties");
 result.entry=volume->Stat(id);
 result.description=volume->Description();
 result.details=volume->PropertiesDetails(id);
 if(result.capabilities.attributes)result.attributes=volume->GetAttributes(id);
 Cancel(job);
 return source;
}

void ReadClassicProperties(const Location& item,Volume& volume,uint32_t id,bool editing,Completion& result)
{
 result.classicProperties=result.capabilities.classicProperties&&volume.GetClassicProperties(id,result.properties);
 if(result.classicProperties&&item.IsClassic()&&!editing)
  result.capabilities.writable=StorageLocation::ClassicPropertiesWritable(item);
}

using VolumeCoord::Mode;
MetadataPlan ReadMetadata(const Location& item,std::function<void(Storage::Job&,Completion&)> execute)
{
 return {&item,Mode::Read,false,true,std::move(execute)};
}
MetadataPlan EditMetadata(const Location& item,std::function<void(Storage::Job&,Completion&)> execute)
{
 return {&item,Mode::Write,true,true,std::move(execute)};
}

MetadataPlan DescribeMetadata(const InspectProperties& inspect)
{
 return ReadMetadata(inspect.item,[&inspect](Storage::Job& job,Completion& result){
  MetadataProgress(job,"Reading properties");
  auto [volume,id]=ReadProperties(inspect.item,false,job,result);
  ReadClassicProperties(inspect.item,*volume,id,false,result);
 });
}

MetadataPlan DescribeMetadata(const ImportAttributeValue& input)
{
 Check(!input.name.empty(),"Enter an attribute name");
 return ReadMetadata(input.item,[&input](Storage::Job& job,Completion& result){
  MetadataProgress(job,"Updating properties",input.name);
  auto [volume,id]=Source(input.item);
  const auto entry=volume->Stat(id);
  Check(!entry.directory,"Attribute value must be a file");
  auto bytes=HfsPlus::AttributeData::Snapshot(entry.forks[0].size,[&](uint64_t at,void* out,size_t count){
   Cancel(job);
   volume->Read(id,0,at,out,count);
  });
  Cancel(job);
  result.attributes[input.name]=std::move(bytes);
 });
}

MetadataPlan DescribeMetadata(const ExportAttributeValue& output)
{
 Check(output.item.IsWindows(),"Choose a Windows output filename");
 // Attribute data is a standalone export and never creates a companion file.
 return {&output.item,Mode::Write,false,false,[&output](Storage::Job& job,Completion& result){
  MetadataProgress(job,"Updating properties");
  const auto& bytes=output.value;
  const auto& path=output.item.WindowsPath();
  HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)throw OperationError(GetLastError(),"Cannot create attribute output; choose a new filename");
  Mark(result,output.item);
  try{
   HfsPlus::Bytes buffer(65536);
   for(uint64_t offset=0;offset<bytes.size();){
    Cancel(job);
    const auto count=size_t((std::min)(uint64_t(buffer.size()),bytes.size()-offset));
    bytes.Read(offset,buffer.data(),count);
    DWORD written=0;
    if(!WriteFile(file,buffer.data(),DWORD(count),&written,nullptr)||written!=count)
     throw OperationError(GetLastError(),"Attribute export write failed");
    offset+=count;
   }
   Cancel(job);
   if(!FlushFileBuffers(file))throw OperationError(GetLastError(),"Attribute export flush failed");
   CloseHandle(file);
  }catch(...){CloseHandle(file);DeleteFileW(path.c_str());throw;}
 }};
}

MetadataPlan DescribeMetadata(const EditBasicMetadata& edit)
{
 return EditMetadata(edit.item,[&edit](Storage::Job& job,Completion& result){
  MetadataProgress(job,"Updating properties");
  auto [volume,id]=ReadProperties(edit.item,false,job,result);
  Check(result.capabilities.CanEditBasicMetadata(),"This format has no writable basic metadata editor");
  const auto& now=result.entry;
  const auto& old=edit.original;
  Check(now.id==old.id&&now.name==old.name&&now.version==old.version&&now.finder==old.finder&&
        now.created==old.created&&now.modified==old.modified&&now.protectedFile==old.protectedFile&&
        now.forks[0].size==old.forks[0].size&&now.forks[1].size==old.forks[1].size,
        "Properties changed while the editor was open; reopen Properties");
  Cancel(job);
  Mark(result,edit.item);
  volume->SetBasicMetadata(id,edit.desired);
  result.entry=edit.desired;
  ReadClassicProperties(edit.item,*volume,id,false,result);
 });
}

MetadataPlan DescribeMetadata(const EditAttributes& edit)
{
 return EditMetadata(edit.item,[&edit](Storage::Job& job,Completion& result){
  MetadataProgress(job,"Updating properties");
  auto [volume,id]=ReadProperties(edit.item,false,job,result);
  Check(result.capabilities.CanEditAttributes(),"This item cannot store extended attributes");
  Check(result.attributes==edit.original,"Attributes changed while Properties was open; reopen Properties before saving");
  Cancel(job);
  Mark(result,edit.item);
  volume->SetAttributesForJob(id,edit.desired,job);
  result.attributes=edit.desired;
  ReadClassicProperties(edit.item,*volume,id,false,result);
 });
}

MetadataPlan DescribeMetadata(const ChangeCompression& edit)
{
 return EditMetadata(edit.item,[&edit](Storage::Job& job,Completion& result){
  MetadataProgress(job,"Updating properties");
  auto [volume,id]=ReadProperties(edit.item,false,job,result);
  Check(result.capabilities.CanEditCompression(),"This volume cannot change compression");
  const auto& now=result.entry;
  const auto& old=edit.original;
  Check(now.id==old.id&&now.parent==old.parent&&now.name==old.name&&now.modified==old.modified&&
        now.created==old.created&&now.finder==old.finder&&now.compressed==old.compressed&&
        now.forks[0].size==old.forks[0].size&&now.forks[1].size==old.forks[1].size&&
        result.attributes==edit.originalAttributes,
        "The file changed while Properties was open; reopen Properties");
  Cancel(job);
  Mark(result,edit.item);
  Storage::Progress progress;
  progress.phase=edit.method!=HfsPlus::Volume::CompressionMode::Uncompressed?"Compressing file":"Decompressing file";
  progress.path=Utf8(now.name);
  progress.utf8=progress.byteTransfer=true;
  progress.scanning=false;
  progress.totalBytes=now.forks[0].size+now.forks[1].size;
  volume->SetCompressionMethodForJob(id,edit.method,[&](uint64_t done,uint64_t total){
   Cancel(job);
   progress.processed=progress.bytes=done;
   progress.totalBytes=total;
   job.progress(progress);
  },job);
  result.entry=volume->Stat(id);
  ReadClassicProperties(edit.item,*volume,id,false,result);
 });
}

MetadataPlan DescribeMetadata(const EditClassicProperties& edit)
{
 return EditMetadata(edit.item,[&edit](Storage::Job& job,Completion& result){
  MetadataProgress(job,"Updating properties");
  const auto& item=edit.item;
  auto update=[&]{
   auto [volume,id]=ReadProperties(item,true,job,result);
   ReadClassicProperties(item,*volume,id,true,result);
   Check(result.classicProperties&&result.capabilities.CanEditClassicProperties(),"This item does not expose writable classic Finder properties");
   const auto current=result.properties;
   Check(!item.expectedCatalogId||uint32_t(current.cnid)==item.expectedCatalogId,"The selected HFS entry was replaced");
   Cancel(job);
   auto changed=current;
   if(auto* label=std::get_if<FinderLabel>(&edit.change)){
    Check(label->value>=0&&label->value<=7,"Invalid Finder label");
    changed.fdflags=short((changed.fdflags&~14)|(label->value<<1));
   }else if(auto* position=std::get_if<FinderPosition>(&edit.change)){
    changed.fdlocation.h=position->x;
    changed.fdlocation.v=position->y;
    changed.fdflags&=~HFS_FNDR_HASBEENINITED;
   }else{
    const auto& properties=std::get<ClassicPropertyEdit>(edit.change);
    Check(SameProperties(current,properties.original),"Properties changed while the editor was open; reopen Properties before applying changes");
    changed=properties.desired;
    Check(changed.cnid==current.cnid&&changed.parid==current.parid&&((changed.flags^current.flags)&HFS_ISDIR)==0,
          "Catalog identity and item kind cannot be changed");
    if(!(current.flags&HFS_ISDIR))
     Check(changed.u.file.dsize==current.u.file.dsize&&changed.u.file.rsize==current.u.file.rsize,
           "Fork lengths cannot be changed through Properties");
   }
   Cancel(job);
   Mark(result,item);
   volume->SetClassicProperties(id,changed);
   Storage::OutcomeEvidence::NoteRetainedChange();
   if(item.IsClassic())volume->Close();
  };
  if(item.IsClassic())StorageLocation::WithVerifiedWrite(item,update);
  else update();
 });
}

Storage::Result MakeFolder(const CreateFolder& folder,Storage::Job& job,Completion& completion)
{
 MetadataProgress(job,"Creating folder",folder.name);
 const auto& item=folder.parent;
 if(item.Volume()){
  StorageLocation::WithVerifiedWrite(item,[&]{
   auto [volume,id]=Source(item);
   const auto entry=volume->Stat(id);
   Cancel(job);
   const auto caps=volume->Supports();
   Check(entry.directory&&caps.writable&&caps.createFolders&&
         (entry.id==2||!entry.protectedFile),"Cannot create a folder here");
   MarkChild(completion,item,folder.name);
   volume->MakeDirectory(id,folder.name);
  });
 }else if(item.IsWindows()){
  StorageLocation::ValidateMedia(item);
  auto volume=OpenWindows(item.WindowsPath());
  StorageLocation::ValidateFolderIdentity(item,volume->Stat(2));
  Cancel(job);
  MarkChild(completion,item,folder.name);
  volume->MakeDirectory(2,folder.name);
  volume->Close();
 }else{
  Check(item.IsClassic(),"Invalid folder destination");
  StorageLocation::ValidateImage(VolumeCoord::Wide(item.ClassicImage()),item.expectedResource);
  StorageLocation::WithVerifiedWrite(item,[&]{
   auto volume=OpenClassic(item.ClassicImage(),item.ClassicPath(),true);
   const auto entry=volume->Stat(2);
   Cancel(job);
   Check(entry.directory,"Destination is not a folder");
   if(item.expectedCatalogId)Check(entry.resolvedId==item.expectedCatalogId,"The destination HFS folder was replaced");
   MarkChild(completion,item,folder.name);
   volume->MakeDirectory(2,folder.name);
   volume->Close();
  });
 }
 completion.complete=true;
 completion.completed=1;
 return {};
}
std::string Resource(const Location& item){return item.ResourceKey();}
void AddPath(std::vector<VolumeCoord::Access>& access,const std::wstring& path,VolumeCoord::Mode mode,bool companion=true){access.push_back({VolumeCoord::PathKey(path),mode});if(companion)access.push_back({VolumeCoord::PathKey(CompanionPath(path)),mode});}
std::u16string SourceName(const Location& item){if(!item.expectedName.empty())return item.expectedName;auto path=HostPath(item);if(!path.empty())return U16(path.substr(path.find_last_of(L"\\/")+1));if(item.IsClassic()&&!item.ClassicPath().empty())return Ansi(item.ClassicPath().substr(item.ClassicPath().find_last_of(':')+1));return {};}
bool IsClassicConversionEndpoint(const Location& item)
{
 return item.IsClassic()||(item.IsMounted()&&std::string_view(item.Volume()->Kind())=="HFS");
}

bool IsNativeMac(const Location& item)
{
    if(!item.IsMounted())return false;
    const std::string_view kind=item.Volume()->Kind();
    return kind=="HFS+"||kind=="HFSX";
}
bool ConversionEndpoint(const Location& item)
{
    return IsClassicConversionEndpoint(item)||IsNativeMac(item);
}

bool IsMountedClassic(const Location& item)
{
    return item.IsMounted()&&ExecutionFor(*item.Volume()).classicConversion;
}

bool IsClassicTransferEndpoint(const Location& item)
{
    return item.IsClassic()||IsMountedClassic(item);
}

bool ConvertsWithMountedClassic(const LegacyConversion& conversion)
{
    return (conversion.source.IsWindows()&&IsMountedClassic(conversion.destination))||
           (IsMountedClassic(conversion.source)&&conversion.destination.IsWindows());
}

bool CanUseTemporaryWorker(bool inWorker)
{
    if(inWorker)return false;
#ifdef HFV_TESTING
    return CopyProcess::enabled;
#else
    return true;
#endif
}

bool Persistent(const Location& item)
{
 return item.IsMounted()&&ExecutionFor(*item.Volume()).Persistent();
}
bool ProcessLocal(const Location& item)
{
 return item.IsMounted()&&ExecutionFor(*item.Volume()).processLocal;
}
bool WindowsEndpoint(const Location& item)
{
 return item.IsWindows()||(item.IsMounted()&&ExecutionFor(*item.Volume()).windowsPaths);
}

void ReserveEndpoint(std::vector<VolumeCoord::Access>& access,const Location& item,Mode mode,bool companions=true)
{
 const auto path=HostPath(item);
 if(path.empty())access.push_back({Resource(item),mode});
 else AddPath(access,path,mode,companions);
}

void ReserveTransferSource(std::vector<VolumeCoord::Access>& access,const Location& source,
                           bool background,bool handoff,bool windowsExport=false)
{
 // Mounted Windows exports can coexist with other readers.
 const bool sharedExport=windowsExport&&Persistent(source)&&!handoff;
 const auto mode=source.cut||(HostPath(source).empty()&&!sharedExport&&(background||handoff))
    ?Mode::Write:Mode::Read;
 ReserveEndpoint(access,source,mode);
}

TransferPlan PlanConversion(const LegacyConversion&);

TransferExecution DescribeTransfer(const OrdinaryCopy& copy,bool background,bool temporary)
{
 Check(!copy.sources.empty()&&!copy.destination.IsNone(),"Choose sources and a copy destination");
 for(const auto& source:copy.sources)Check(!source.IsNone(),"No source file location was supplied");
 const bool mountedRoute=Persistent(copy.destination)||(WindowsEndpoint(copy.destination)&&
      std::any_of(copy.sources.begin(),copy.sources.end(),Persistent));
 const bool handoff=temporary&&!mountedRoute&&!ProcessLocal(copy.destination)&&
     std::none_of(copy.sources.begin(),copy.sources.end(),ProcessLocal);
 TransferExecution plan{
  handoff?ExecutionRoute::TemporaryWorker:ExecutionRoute::Local,{},
  [&copy,background,handoff]{
   std::vector<VolumeCoord::Access> access;
   const auto path=HostPath(copy.destination);
   for(const auto& source:copy.sources)ReserveTransferSource(access,source,background,handoff,!path.empty());
   if(path.empty())access.push_back({Resource(copy.destination),Mode::Write});
   else for(const auto& source:copy.sources){
    const auto name=SourceName(source);
    if(name.empty()){AddPath(access,path,Mode::Write,false);break;}
    AddPath(access,ChildPath(path,name),Mode::Write);
   }
   return access;
  },
  [&copy](const auto& sources,Storage::Job& job,Completion& result){return RunTransfer({copy,{}},sources,job,result);},
  [&copy](Completion& result){
   Mark(result,copy.destination);
   for(const auto& source:copy.sources)if(source.cut)Mark(result,source);
  }
 };
 if(!handoff)
  for(const auto& source:copy.sources)
   plan.sources.push_back(VolumeSession::SharesMountedImage(source,copy.destination)
                         ?ForkSource::MountedWorker:ForkSource::Stream);
 return plan;
}

TransferExecution DescribeTransfer(const LegacyConversion& copy,bool background,bool temporary)
{
 Check((copy.source.IsWindows()||ConversionEndpoint(copy.source))&&
       (copy.destination.IsWindows()||ConversionEndpoint(copy.destination)),
       "Choose compatible conversion endpoints");
 Check(copy.target==ConversionTarget::Folder||!copy.destination.IsMounted(),
       "A mounted conversion destination must be a folder");
 const auto mode=copy.source.IsWindows()?copy.options.input:copy.options.output;
 const bool native=IsNativeMac(copy.source)||IsNativeMac(copy.destination);
 const bool handoff=temporary&&!ProcessLocal(copy.source)&&!ProcessLocal(copy.destination)&&!native&&
     !(ConvertsWithMountedClassic(copy)&&mode&&strchr("ambt53rR",mode));
 return {handoff?ExecutionRoute::TemporaryWorker:ExecutionRoute::Local,{},
  [&copy,background,handoff]{
   std::vector<VolumeCoord::Access> access;
   const bool sharedExport=(IsMountedClassic(copy.source)||IsNativeMac(copy.source))&&
       copy.destination.IsWindows()&&!handoff;
   if(copy.source.IsWindows())AddPath(access,copy.source.WindowsPath(),copy.move?Mode::Write:Mode::Read);
   else access.push_back({Resource(copy.source),copy.move||(!sharedExport&&(background||handoff))
       ?Mode::Write:Mode::Read});
   if(copy.destination.IsWindows())access.push_back({VolumeCoord::LegacyOutputKey(copy.destination.WindowsPath()),Mode::Write});
   else access.push_back({Resource(copy.destination),Mode::Write});
   return access;
  },
  [&copy](const auto&,Storage::Job& job,Completion& result){return RunTransfer(PlanConversion(copy),{},job,result);},
  [&copy](Completion& result){Mark(result,copy.destination);if(copy.move)Mark(result,copy.source);}
 };
}

TransferExecution DescribeTransfer(const ExportFileFork& request,bool background,bool temporary)
{
 Check(!request.source.IsNone()&&request.output.IsWindows()&&
       (request.fork==ForkSelector::Data||request.fork==ForkSelector::Resource),"Invalid fork export request");
 const bool handoff=temporary&&!Persistent(request.source)&&!ProcessLocal(request.source);
 return {handoff?ExecutionRoute::TemporaryWorker:ExecutionRoute::Local,{},
  [&request,background,handoff]{
   std::vector<VolumeCoord::Access> access;
   ReserveTransferSource(access,request.source,background,handoff,true);
   ReserveEndpoint(access,request.output,Mode::Write,false);
   return access;
  },
  [&request](const auto&,Storage::Job& job,Completion& result){return ExportFork(request,job,result);},
  [&request](Completion& result){Mark(result,request.output);}
 };
}

TransferExecution DescribeTransfer(const ImportResourceFork& request,bool background,bool temporary)
{
 Check(!request.input.IsNone()&&(request.file.IsMounted()||request.file.IsWindows()),"Invalid resource import request");
 const bool handoff=temporary&&!ProcessLocal(request.file)&&!ProcessLocal(request.input)&&
     !(Persistent(request.file)&&WindowsEndpoint(request.input));
 return {handoff?ExecutionRoute::TemporaryWorker:ExecutionRoute::Local,{},
  [&request,background,handoff]{
   std::vector<VolumeCoord::Access> access;
   ReserveTransferSource(access,request.input,background,handoff);
   ReserveEndpoint(access,request.file,Mode::Write);
   return access;
  },
  [&request](const auto&,Storage::Job& job,Completion& result){return ReplaceResource(request,job,result);},
  [&request](Completion& result){Mark(result,request.file);}
 };
}

ExecutionRoute RouteFor(bool temporary,bool staysMounted)
{
 return temporary&&!staysMounted?ExecutionRoute::TemporaryWorker:ExecutionRoute::Local;
}

void ReserveChild(std::vector<VolumeCoord::Access>& access,const Location& parent,
                  const std::u16string& name,bool companion)
{
 const auto path=HostPath(parent);
 if(path.empty())access.push_back({Resource(parent),Mode::Write});
 else AddPath(access,ChildPath(path,name),Mode::Write,companion);
}

MutationPlan DescribeMutation(const RenameEntry& request,bool temporary)
{
 Check(!request.source.IsNone(),"Select an entry to rename");
 Check(!request.name.empty(),"Enter a new name");
 Check(request.parent.IsNone()||
       (request.source.IsMounted()&&request.parent.IsMounted()&&
        request.source.Volume()==request.parent.Volume()),
       "Rename must stay within the same mounted volume");
 return {RouteFor(temporary,Persistent(request.source)||ProcessLocal(request.source)||ProcessLocal(request.parent)),
  [&request]{
   std::vector<VolumeCoord::Access> access;
   const auto path=HostPath(request.source);
   if(path.empty())access.push_back({Resource(request.source),Mode::Write});
   else{
    AddPath(access,path,Mode::Write);
    const auto parent=path.substr(0,path.find_last_of(L"\\/")+1);
    const auto explicitParent=request.parent.IsNone()?std::wstring{}:HostPath(request.parent);
    AddPath(access,ChildPath(explicitParent.empty()?parent:explicitParent,request.name),Mode::Write);
   }
   return access;
  },
  [&request](Storage::Job& job,Completion& result){return Mutate(&request,&request.source,1,job,result);}
 };
}

MutationPlan DescribeMutation(const DeleteEntries& request,bool temporary)
{
 Check(!request.selection.empty(),"Select entries to delete");
 for(const auto& item:request.selection)
  Check(!item.IsNone(),"No source file location was supplied");
 return {RouteFor(temporary,std::all_of(request.selection.begin(),request.selection.end(),Persistent)||
                  std::any_of(request.selection.begin(),request.selection.end(),ProcessLocal)),
  [&request]{
   std::vector<VolumeCoord::Access> access;
   for(const auto& item:request.selection)ReserveEndpoint(access,item,Mode::Write);
   return access;
  },
  [&request](Storage::Job& job,Completion& result){
   return Mutate(nullptr,request.selection.data(),request.selection.size(),job,result);
  }
 };
}

MutationPlan DescribeMutation(const CreateFolder& request,bool temporary)
{
 Check(!request.parent.IsNone()&&!request.name.empty(),"Choose a destination and folder name");
 return {RouteFor(temporary,Persistent(request.parent)||ProcessLocal(request.parent)),
  [&request]{
   std::vector<VolumeCoord::Access> access;
   ReserveChild(access,request.parent,request.name,false);
   return access;
  },
  [&request](Storage::Job& job,Completion& result){return MakeFolder(request,job,result);}
 };
}

Storage::Result RunHardLink(const CreateHardLink& request,Storage::Job& job,Completion& completion)
{
 StorageLocation::WithVerifiedWrite(request.parent,[&]{
  auto source=Source(request.source);
  auto target=Destination(request.parent);
  Check(source.first->Supports().hardLinks&&target.first->Supports().CanCreateHardLinks()&&
        source.first->ResourceKey()==target.first->ResourceKey(),
        "Hard links require the same writable volume with hard-link support");
  Cancel(job);
  Mark(completion,request.source);
  MarkChild(completion,request.parent,request.name);
  Storage::Progress progress;
  progress.scanning=false;
  progress.phase="Creating hard link";
  progress.totalFiles=1;
  job.progress(progress);
  target.first->MakeHardLinkForTransfer(source.second,target.second,request.name,false,job);
  progress.files=1;
  job.progress(progress);
 });
 completion.complete=true;
 completion.completed=1;
 return {};
}

MutationPlan DescribeMutation(const CreateHardLink& request,bool temporary)
{
 Check(!request.source.IsNone()&&!request.parent.IsNone()&&!request.name.empty(),
       "Choose a hard link source, destination and name");
 return {RouteFor(temporary,(Persistent(request.source)&&Persistent(request.parent))||
                  ProcessLocal(request.source)||ProcessLocal(request.parent)),
  [&request]{
   std::vector<VolumeCoord::Access> access;
   ReserveEndpoint(access,request.source,HostPath(request.source).empty()?Mode::Write:Mode::Read);
   ReserveChild(access,request.parent,request.name,true);
   return access;
  },
  [&request](Storage::Job& job,Completion& result){return RunHardLink(request,job,result);}
 };
}

// Windows path identities can change while a job is queued. Refresh only
// the bindings at execution, using the same route and lease policy.
std::vector<VolumeCoord::Access> BindResources(const ExecutionPlan& plan)
{
 if(plan.transfer)return plan.transfer->bind();
 if(plan.mutation)return plan.mutation->bind();
 std::vector<VolumeCoord::Access> access;
 const auto& metadata=plan.metadata.value();
 ReserveEndpoint(access,*metadata.item,metadata.access,metadata.includeCompanion);
 return access;
}

ExecutionPlan PlanOperation(const Request& request, bool inWorker=false){
 ExecutionPlan plan;
 const bool temporary=WorkerPolicy(request.OperationKind()).has_value()&&CanUseTemporaryWorker(inWorker);
 if(const auto* copy=request.Get<OrdinaryCopy>())plan.transfer=DescribeTransfer(*copy,request.background,temporary);
 else if(const auto* conversion=request.Conversion())plan.transfer=DescribeTransfer(*conversion,request.background,temporary);
 else if(const auto* output=request.Get<ExportFileFork>())plan.transfer=DescribeTransfer(*output,request.background,temporary);
 else if(const auto* input=request.Get<ImportResourceFork>())plan.transfer=DescribeTransfer(*input,request.background,temporary);
 else if(const auto* rename=request.Rename())plan.mutation=DescribeMutation(*rename,temporary);
 else if(const auto* deletion=request.Delete())plan.mutation=DescribeMutation(*deletion,temporary);
 else if(const auto* folder=request.Get<CreateFolder>())plan.mutation=DescribeMutation(*folder,temporary);
 else if(const auto* link=request.Get<CreateHardLink>())plan.mutation=DescribeMutation(*link,temporary);
 else if(const auto* metadata=request.Metadata()){
  plan.metadata=std::visit([](const auto& operation){
   Check(!operation.item.IsNone(),"Choose a location for the metadata operation");
   return DescribeMetadata(operation);
  },*metadata);
 }
 if(plan.transfer)plan.route=plan.transfer->route;
 else if(plan.mutation)plan.route=plan.mutation->route;
 else if(plan.metadata)
  plan.route=RouteFor(temporary&&plan.metadata->temporaryForUnmounted,
                     Persistent(*plan.metadata->item)||ProcessLocal(*plan.metadata->item));
 else throw std::logic_error("Missing file operation plan");
 plan.resources=BindResources(plan);
 return plan;
}

Reader AdaptRawClassic(Volume& source, Entry& entry, bool importing, bool resource,
                      const TypeMappings::Snapshot& mappings) {
 Check(entry.attributes.empty(),"Raw copy cannot preserve extended attributes or Mac ACLs");
 const auto dot=entry.name.rfind(u'.');
 if(importing){
  entry.finder.fill(0);
  memcpy(entry.finder.data(),"????UNIX",8);
  if(dot!=entry.name.npos && entry.name.size()-dot<30){
   const auto suffix=Wide(entry.name.substr(dot+1));
   const auto mapping=std::find_if(mappings.windowsToHfs.begin(),mappings.windowsToHfs.end(),
      [&](const auto& value){return !_wcsicmp(Wide(Ansi(value.first)).c_str(),suffix.c_str());});
   if(mapping!=mappings.windowsToHfs.end() && mapping->second.size()==9){
    memcpy(entry.finder.data(),mapping->second.data(),8);
    // The legacy resource import keeps the extension even when Strip is set.
    if(!resource && dot && mapping->second[8]=='x')entry.name.resize(dot);
   }
  }
  const auto size=entry.forks[0].size;
  entry.forks[0].size=resource?0:size;
  entry.forks[1].size=resource?size:0;
 }else{
  const std::string typeCreator(reinterpret_cast<const char*>(entry.finder.data()),8);
  const auto mapping=std::find_if(mappings.hfsToWindows.begin(),mappings.hfsToWindows.end(),
     [&](const auto& value){return value.first==typeCreator;});
  if(mapping!=mappings.hfsToWindows.end() && (dot==entry.name.npos || entry.name.size()-dot>6)){
   const auto suffix=Ansi(mapping->second);
   if(dot!=entry.name.npos)entry.name.resize(dot);
   if(suffix.empty() || suffix.front()!=u'.')entry.name+=u'.';
   entry.name+=suffix;
  }
  entry.forks[0].size=entry.forks[resource?1:0].size;
  entry.forks[1].size=0;
  entry.portableContainer=true;
 }
 const auto inputFork=!importing && resource ? 1u : 0u;
 const auto id=entry.id;
 return [&source,id,inputFork](unsigned, uint64_t at,void* buffer,size_t count){
  source.Read(id,inputFork,at,buffer,count);
 };
}

TransferPlan PlanConversion(const LegacyConversion& conversion) {
 const bool importing=conversion.source.IsWindows() && ConversionEndpoint(conversion.destination);
 const bool exporting=ConversionEndpoint(conversion.source) && conversion.destination.IsWindows();
 const bool native=IsNativeMac(conversion.source)||IsNativeMac(conversion.destination);
 const bool translating=importing||exporting;
 const char mode=importing?conversion.options.input:conversion.options.output;
 const bool raw=translating&&(mode=='r'||mode=='R');
 if(translating)Check(TransferConversion::Supported(mode)||mode=='a'||raw,"Unsupported file conversion mode");
 auto mappings=raw?TypeMappings::Capture():TypeMappings::Snapshot{};
 auto sourceLocation=conversion.source;
 sourceLocation.cut=conversion.move;
 if(sourceLocation.IsClassic()&&conversion.directory){
  auto path=sourceLocation.ClassicPath();
  Check(!path.empty(),"The classic HFS source path is empty");
  if(path.back()==':'&&path.find(':')!=path.size()-1){
   const auto generation=sourceLocation.MediaGenerationKnown()?std::optional<DWORD>(sourceLocation.MediaGeneration()):std::nullopt;
   path.pop_back();
   sourceLocation.SetClassic(sourceLocation.ClassicImage(),std::move(path));
   if(generation)sourceLocation.BindMediaGeneration(*generation);
  }
 }
 Location folder=conversion.destination;
 std::u16string outputName;
 if(conversion.target==ConversionTarget::NamedPath && folder.IsWindows()) {
  auto path=folder.WindowsPath();
  const auto flags=GetFileAttributesW(BrowserPresentation::WindowsPath(path).c_str());
  if(flags==INVALID_FILE_ATTRIBUTES){
   const auto error=GetLastError();
   if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
    throw OperationError(error,"Cannot inspect the Windows copy destination");
  }
  if(flags==INVALID_FILE_ATTRIBUTES || !(flags&FILE_ATTRIBUTE_DIRECTORY)) {
   auto end=path.find_last_of(L"\\/");
   outputName=U16(path.substr(end+1));
   const auto serial=folder.WindowsMediaSerial();
   folder.SetWindows(path.substr(0,end+1));
   folder.BindWindowsMediaSerial(serial);
  }
 } else if(conversion.target==ConversionTarget::NamedPath && folder.IsClassic()) {
  auto path=folder.ClassicPath();
  Check(!path.empty(),"The classic HFS destination path is empty");
  if(path.back()!=':') {
   auto image=folder.ClassicImage();
   const auto end=path.find_last_of(':');
   const auto parentPath=path.substr(0,end+1);
   StorageLocation::ValidateImage(VolumeCoord::Wide(image),folder.expectedResource);
   auto parent=OpenClassic(image,parentPath,false);
   StorageLocation::ValidateMedia(folder);
   Entry child;
   auto leaf=Ansi(path.substr(end+1));
   const bool isDirectory=parent->FindChild(2,leaf,child)&&child.directory;
   parent->Close();
   if(!isDirectory){
    outputName=std::move(leaf);
    const auto generation=folder.MediaGenerationKnown()?std::optional<DWORD>(folder.MediaGeneration()):std::nullopt;
    folder.SetClassic(std::move(image),parentPath);
    if(generation)folder.BindMediaGeneration(*generation);
   }
  }
 }
 TransferPlan plan{OrdinaryCopy{{std::move(sourceLocation)},std::move(folder)},{}};
 if(!translating&&outputName.empty())return plan;
 const bool appledouble=conversion.options.appledouble;
 plan.adapter=[translating,importing,exporting,native,mode,raw,appledouble,mappings=std::move(mappings),outputName=std::move(outputName)]
     (Volume& volume,Entry& entry,Storage::Job& active,bool root) -> Reader {
  if(!translating){
   if(root&&!outputName.empty())entry.name=outputName;
   return Reader{};
  }
  char selected=mode;
  bool paired=false;
  if(importing && (selected=='a' || raw) && appledouble && !entry.directory) {
   const auto path=volume.HostPath(entry.id);
   const auto slash=path.find_last_of(L"\\/");
   if(slash!=path.npos)paired=WindowsResourceCompanion(path.substr(0,slash+1)+L"%"+path.substr(slash+1));
  }
  if(selected=='a' && !entry.directory) {
   if(importing) {
    const auto dot=entry.name.rfind(u'.');
    const auto suffix=dot==entry.name.npos ? std::wstring{} : Wide(entry.name.substr(dot));
    if(paired) selected='&';
    else if(native){
     const auto format=!_wcsicmp(suffix.c_str(),L".hqx")?'b':!_wcsicmp(suffix.c_str(),L".bin")?'m':'&';
     selected=format!='&'&&TransferConversion::RecognizesContainer(format,volume,entry)?format:'&';
    }
    else if(!_wcsicmp(suffix.c_str(),L".hqx")) selected='b';
    else if(!_wcsicmp(suffix.c_str(),L".bin")) selected='m';
    else if(!_wcsicmp(suffix.c_str(),L".txt") || !_wcsicmp(suffix.c_str(),L".c") ||
            !_wcsicmp(suffix.c_str(),L".h")) selected='t';
    else {
     selected='r';
     memcpy(entry.finder.data(),"????UNIX",8);
    }
   } else if(native)selected='&';
   else if(!memcmp(entry.finder.data(),"TEXT",4) || !memcmp(entry.finder.data(),"ttro",4)) selected='t';
   else selected=entry.forks[1].size ? 'm' : 'r';
  }
  const bool explicitName=root&&!outputName.empty();
  if(explicitName&&raw&&exporting)entry.name=outputName;
  Reader reader;
  if(raw && !entry.directory && !paired) reader=AdaptRawClassic(volume,entry,importing,mode=='R',mappings);
  else if(TransferConversion::Supported(selected)) reader=TransferConversion::Adapt(selected,importing,volume,entry,active);
  if(explicitName&&!(raw&&exporting))entry.name=outputName;
  else if(exporting) {
   for(auto& c:entry.name) {
    if(c<32) c=u'-';
    switch(c) {
    case u'\\': case u'/': case u':': case u'*': case u'?':
    case u'"': case u'<': case u'>': case u'|': c=u'-'; break;
    }
   }
  }
  return reader;
 };
 return plan;
}

Storage::Result Execute(const ExecutionPlan& plan,Storage::Job& job,Completion& completion){
 HfsPlus::AttributeReadScope attributeReads([&]{Cancel(job);});
 Cancel(job);
 if(plan.transfer)return plan.transfer->run(plan.transfer->sources,job,completion);
 if(plan.mutation)return plan.mutation->run(job,completion);
 if(plan.metadata){
  const auto started=GetTickCount64();
  for(;;){
   Cancel(job);
   try{
    auto execute=[&]{plan.metadata->execute(job,completion);};
    if(plan.metadata->access==Mode::Write&&plan.metadata->item->IsMounted())
     StorageLocation::WithVerifiedWrite(*plan.metadata->item,execute);
    else execute();
    break;
   }catch(const VolumeCoord::Busy&){
    // A shared lease excludes writes, but some local engines can briefly
    // reject a second reader while servicing a browser catalog call.
    if(plan.metadata->access!=Mode::Read||GetTickCount64()-started>=2000)throw;
    completion={};
    Sleep(10);
   }
  }
  completion.complete=true;
  completion.completed=1;
  return {};
 }
 throw std::logic_error("Missing file operation plan");
}
}
Operation::Operation(std::shared_ptr<State> state,std::shared_ptr<Storage::Job> job):state_(std::move(state)),job_(std::move(job)){}
Completion Operation::Snapshot()const{std::lock_guard<std::mutex> lock(state_->mutex);return state_->completion;}
namespace {
void FinalizeOutcome(Storage::Result& result, Completion& completion, Storage::Job& job,
                     Storage::OperationCompletion& outcome) {
 const auto progress=job.progress();
 outcome.Finish(result,(std::max)(completion.completed,progress.files),progress.skipped);
 completion.completed=result.completed;
}

#ifdef HFV_TESTING
std::atomic<HANDLE> publicationEntered{nullptr}, publicationRelease{nullptr};
#endif

void PublishAffected(const Completion& completion, VolumeCoord::Lease* reservation,
                     VolumeCoord::OperationLease* supplemental=nullptr) {
 std::vector<std::string> changed;
 for(const auto& item:completion.affected)changed.push_back(item.resourceKey);
#ifdef HFV_TESTING
 if(!changed.empty()){
  if(const auto entered=publicationEntered.load()){
   SetEvent(entered);
   WaitForSingleObject(publicationRelease.load(),15000);
  }
 }
#endif
 if(reservation)reservation->ReleaseAndPublish(changed,supplemental);
 else VolumeCoord::Changed(changed);
}
}
namespace {
std::string LaunchVersion(Volume& volume,uint32_t id,Storage::Job& job)
{
 const auto entry=volume.Stat(id);
 Check(!entry.directory,"The launcher selection must be a file");
 auto version=MacBrowser::Snapshot(volume,id,job);
 uint64_t hash=14695981039346656037ULL;
 std::vector<uint8_t> buffer(65536);
 for(unsigned fork=0;fork<2;++fork){
  for(uint64_t offset=0;offset<entry.forks[fork].size;){
   Cancel(job);
   auto count=size_t((std::min)(uint64_t(buffer.size()),entry.forks[fork].size-offset));
   volume.Read(id,fork,offset,buffer.data(),count);
   for(size_t i=0;i<count;++i){hash^=buffer[i];hash*=1099511628211ULL;}
   offset+=count;
  }
 }
 hfsdirent classic{};
 if(volume.GetClassicProperties(id,classic))
  version+=":"+std::to_string(classic.bkdate)+":"+std::to_string(classic.flags);
 return version+":"+std::to_string(hash);
}
}

std::string CaptureLaunchVersion(const Location& item,Storage::Job& job)
{
 LegacyEngine::Scope engine;
 VolumeCoord::Lease lease({{item.ResourceKey(),VolumeCoord::Mode::Read}});
 auto original=Source(item);
 auto version=LaunchVersion(*original.first,original.second,job);
 if(!item.Volume())original.first->Close();
 return version;
}

Storage::Result ApplyLaunchEdit(const Location& item,const std::string& version,
                                const std::wstring& editedFile,Storage::Job& job)
{
 Storage::OperationCompletion outcome;
 Completion completion;
 std::optional<VolumeCoord::Lease> reservation;
 auto result=outcome.Run([&]() -> Storage::Result {
  Check(item.IsClassic()&&!version.empty(),"The original launcher document could not be verified");
  LegacyEngine::Scope engine;
  reservation.emplace(std::vector<VolumeCoord::Access>{{item.ResourceKey(),VolumeCoord::Mode::Write},
                           {VolumeCoord::PathKey(editedFile),VolumeCoord::Mode::Read}});
  HANDLE file=CreateFileW(editedFile.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
  if(file==INVALID_HANDLE_VALUE)throw OperationError(GetLastError(),"Cannot read the edited temporary document");
  struct CloseFile{HANDLE file;~CloseFile(){CloseHandle(file);}} close{file};
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER size{};
  Check(GetFileInformationByHandle(file,&info)&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&
        GetFileSizeEx(file,&size)&&size.QuadPart>=0,"Invalid edited temporary document");
  StorageLocation::WithVerifiedWrite(item,[&]{
   auto original=Source(item,true);
   auto& volume=*original.first;
   const auto id=original.second;
   Check(LaunchVersion(volume,id,job)==version,"The original document changed after launch");
   auto replacement=volume.Stat(id);
   Check(volume.Writable()&&!replacement.protectedFile,"The original document is read-only");
   replacement.forks[0].size=uint64_t(size.QuadPart);
   replacement.modified=uint64_t(time(nullptr))+2082844800ULL;
   Cancel(job);
   Mark(completion,item);
   // The classic replacement stages both forks and preserves Finder metadata.
   volume.PutResourceReplacement(id,replacement,[&](unsigned fork,uint64_t offset,void* bytes,size_t count){
    Cancel(job);
    if(fork){volume.Read(id,1,offset,bytes,count);return;}
    LARGE_INTEGER position{};position.QuadPart=LONGLONG(offset);
    DWORD received=0;
    if(!SetFilePointerEx(file,position,nullptr,FILE_BEGIN)||
       !ReadFile(file,bytes,DWORD(count),&received,nullptr)||received!=count)
     throw OperationError(GetLastError(),"Could not read the entire edited data fork");
   },{});
   volume.Close();
  });
  completion.completed=1;
  return {};
 },ERROR_WRITE_FAULT,"Write-back cancelled");
 FinalizeOutcome(result,completion,job,outcome);
 PublishAffected(completion,reservation?&*reservation:nullptr);
 return result;
}

Storage::Result ExecuteInWorker(const Request& request,Storage::Job& job,Completion& completion){
 Storage::OperationCompletion outcome;
 auto result=outcome.Run([&]{
  const auto plan=PlanOperation(request,true);
  LegacyEngine::Scope engine;
  VolumeCoord::Lease access(plan.resources);
  return Execute(plan,job,completion);
 });
 FinalizeOutcome(result,completion,job,outcome);
 return result;
}
#ifdef HFV_TESTING
void PausePublicationForTest(HANDLE entered,HANDLE release) {
 if(entered)publicationRelease=release;
 publicationEntered=entered;
}

PlanAccessForTest InspectPlanForTest(const Request& request,const std::string& key,bool inWorker){
 const auto plan=PlanOperation(request,inWorker);
 const auto local=plan.transfer?std::count(plan.transfer->sources.begin(),plan.transfer->sources.end(),
     MacBrowser::ForkSource::MountedWorker):0;
 PlanAccessForTest result{plan.route==ExecutionRoute::TemporaryWorker,local!=0,false,false,
                          size_t(local),plan.transfer?plan.transfer->sources.size()-local:0};
 for(const auto& access:plan.resources)if(access.key==key){
  result.read|=access.mode==VolumeCoord::Mode::Read;
  result.write|=access.mode==VolumeCoord::Mode::Write;
 }
 return result;
}
#endif
std::vector<Request> PlanTransfers(OrdinaryCopy copy,const TransferSettings& settings)
{
    std::vector<Request> requests;
    OrdinaryCopy ordinary{{},copy.destination};
    const bool destinationClassic=IsClassicTransferEndpoint(copy.destination);
    const bool destinationWindows=copy.destination.IsWindows();
    const auto flush=[&]{
        if(ordinary.sources.empty())return;
        requests.emplace_back(std::move(ordinary));
        ordinary=OrdinaryCopy{{},copy.destination};
    };
    for(auto& source:copy.sources){
        const bool importing=source.IsWindows()&&destinationClassic;
        const bool exporting=IsClassicTransferEndpoint(source)&&destinationWindows;
        // Native transfers keep their ordinary fork-preserving behavior unless
        // a container mode is configured, or Automatic detects an import.
        const bool nativeImport=source.IsWindows()&&IsNativeMac(copy.destination)&&!settings.askImport&&
            (TransferConversion::ContainerMode(settings.conversion.input)||settings.conversion.input=='a');
        const bool nativeExport=IsNativeMac(source)&&destinationWindows&&!settings.askExport&&
            TransferConversion::ContainerMode(settings.conversion.output);
        if(!importing&&!exporting&&!nativeImport&&!nativeExport){
            ordinary.sources.push_back(std::move(source));
            continue;
        }
        flush();
        const bool directory=source.directory;
        const bool move=source.cut;
        requests.emplace_back(LegacyConversion{std::move(source),copy.destination,directory,move,
            importing?settings.askImport:settings.askExport,settings.conversion,ConversionTarget::Folder});
    }
    flush();
    return requests;
}

namespace {
class PreparedOperation {
 const Request request_;
 ExecutionPlan plan_;
 Storage::Result preparationError_;
 std::optional<VolumeCoord::Lease> reservation_;

 Storage::Result RunReserved(Storage::Job& job,Completion& completion,
                             std::optional<VolumeCoord::OperationLease>& executionAccess)
 {
  const auto route=plan_.route;
  LegacyEngine::Scope engine;
  // Bind again while keeping the queued reservation. The executor still
  // checks the typed selection identities before changing any entry.
  const auto current=BindResources(plan_);
  executionAccess.emplace(current);
  if(route==ExecutionRoute::Local)
   return FileOperations::Execute(plan_,job,completion);

  std::vector<std::string> keys,writes;
  for(const auto& item:current){
   keys.push_back(item.key);
   if(item.mode==VolumeCoord::Mode::Write)writes.push_back(item.key);
  }
  if(plan_.transfer)plan_.transfer->markHandoff(completion);
  return CopyProcess::Run(request_,job,completion,keys,writes);
 }

public:
 explicit PreparedOperation(Request request):request_(std::move(request)){}
 bool Background() const {return request_.background;}

 void Prepare(Storage::Job& job)
 {
  try{Cancel(job);plan_=PlanOperation(request_);}
  catch(const Storage::Cancelled&){preparationError_={1,ERROR_CANCELLED,"Cancelled while identifying Windows paths"};}
  catch(const std::exception& error){preparationError_={1,ERROR_UNHANDLED_EXCEPTION,error.what()};}
  catch(...){preparationError_={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected path planning failure"};}
 }

 bool TryReserve(const Storage::Job& job)
 {
  if(job.cancelled()||preparationError_.code)return true;
  try{reservation_.emplace(plan_.resources);return true;}
  catch(const VolumeCoord::Busy&){return !Background();}
 }

 Storage::Result Run(Storage::Job& job,Completion& completion)
 {
  Storage::OperationCompletion outcome;
  std::optional<VolumeCoord::OperationLease> executionAccess;
  auto result=outcome.Run([&]{
   Cancel(job);
   if(preparationError_.code)return std::move(preparationError_);
   if(!reservation_)throw VolumeCoord::Busy();
   return RunReserved(job,completion,executionAccess);
  });

  FinalizeOutcome(result,completion,job,outcome);
  PublishAffected(completion,reservation_?&*reservation_:nullptr,executionAccess?&*executionAccess:nullptr);
  executionAccess.reset();
  reservation_.reset();
  return result;
 }
};
}

Operation Submit(Request request){
 auto state=std::make_shared<Operation::State>();
 auto operation=std::make_shared<PreparedOperation>(std::move(request));
 auto& queue=operation->Background()?Storage::copyWorker():Storage::worker();
 auto job=queue.submitPrepared(
  [operation](Storage::Job& job){operation->Prepare(job);},
  [operation](const std::shared_ptr<Storage::Job>& job){return operation->TryReserve(*job);},
  [state,operation](Storage::Job& job){
   Completion completion;
   auto result=operation->Run(job,completion);
   std::lock_guard<std::mutex> lock(state->mutex);
   state->completion=std::move(completion);
   return result;
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
