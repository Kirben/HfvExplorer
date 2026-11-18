// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "AttributeReadScope.h"
#include "FileOperations.h"
#include "CopyProcess.h"
#include "TransferConversion.h"
#include "TypeMappingSnapshot.h"
#include "LegacyEngine.h"
#include "HfsPlusDisk.h"
#include "VolumeCoordinator.h"
#include "DirectFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "RenameName.h"
#include "charset.h"
#include <algorithm>
#include <set>
#include <fcntl.h>
#include <atomic>
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
std::wstring HostPath(const Location& item){return item.HostPath();}
std::wstring ChildPath(std::wstring root,const std::u16string& name){if(!root.empty()&&root.back()!=L'\\')root+=L'\\';return root+MacBrowser::Wide(name);}
std::wstring CompanionPath(const std::wstring& path){auto at=path.find_last_of(L"\\/");return path.substr(0,at+1)+L"%"+path.substr(at+1);}
std::string FolderIdentity(std::string);
void Mark(Completion& c,const Location& item){if(!item.IsNone())c.affected.push_back({item.ResourceKey()});}
void MarkChild(Completion& c,const Location& folder,const std::u16string& name){auto path=HostPath(folder);if(path.empty()){Mark(c,folder);return;}Location child;child.SetWindows(ChildPath(path,name));Mark(c,child);}
int CancelClassic(void* job){return static_cast<Storage::Job*>(job)->cancelled()?1:0;}
void Cancel(Storage::Job& job){if(job.cancelled())throw Storage::Cancelled();}
std::u16string Ansi(const std::string& s){int n=MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),nullptr,0);std::wstring w(n,0);if(n)MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),&w[0],n);return U16(w);}
std::string Utf8(const std::u16string& text){auto w=Wide(text);int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);std::string out(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),&out[0],n,nullptr,nullptr);return out;}
bool Absolute(const std::string& p){return p.size()>=3&&((isalpha((unsigned char)p[0])&&p[1]==':'&&p[2]=='\\')||(p[0]=='\\'&&p[1]=='\\'));}
bool Absolute(const std::wstring& p){return p.size()>=3&&((iswalpha(p[0])&&p[1]==L':'&&(p[2]==L'\\'||p[2]==L'/'))||(p[0]==L'\\'&&p[1]==L'\\'));}
void CheckMedia(const Location& item){
 if(!item.MediaGenerationKnown())return;DWORD generation=0;
 bool valid=item.Media()==ClassicMedia::Floppy?physical_floppy_generation(item.Drive(),&generation)!=0:physical_cd_generation(item.Drive(),&generation)!=0;
 if(!valid||generation!=item.MediaGeneration())throw Failure(ERROR_MEDIA_CHANGED,"Media changed after selection; select the volume again");
}
std::pair<std::shared_ptr<Volume>,uint32_t> Source(const Location& value,bool mutate=false){
 if(!value.expectedResource.empty())Check(VolumeCoord::ImageKey(value.ClassicImage())==value.expectedResource,"The selected source image was replaced");
 if(value.Volume()){CheckMedia(value);auto e=value.Volume()->Stat(value.Id());
  if(!value.expectedIdentity.empty())Check(e.identity==value.expectedIdentity,"The selected entry was replaced before the operation started");
  if(!value.expectedName.empty())Check(e.name==value.expectedName,"The selected entry was renamed before the operation started");
  if(value.expectedParent)Check(e.parent==value.expectedParent,"The selected entry was moved before the operation started");
  return {value.Volume(),value.Id()};}
 std::shared_ptr<Volume> volume;std::u16string name;
 if(value.IsWindows()){
  Check(Absolute(value.WindowsPath()),"Use an absolute Windows source path");
  // Resolve only the captured entry. Enumerating its parent also examines
  // unrelated junctions (for example Documents' legacy library links).
  auto source=ResolveWindowsFile(value.WindowsPath());auto entry=source.first->Stat(source.second);
  if(!value.expectedIdentity.empty())Check(entry.identity==value.expectedIdentity,"The selected source was replaced or changed");return source;
 }
 else{auto pos=value.ClassicPath().find_last_of(':');Check(pos!=std::string::npos,"Use an absolute classic HFS path");volume=OpenClassic(value.ClassicImage(),value.ClassicPath().substr(0,pos+1),mutate);CheckMedia(value);name=Ansi(value.ClassicPath().substr(pos+1));}
 for(auto& e:volume->List())if(HfsPlus::CompareNames(e.name,name,volume->CaseSensitive())==0){if(value.expectedCatalogId)Check(e.resolvedId==value.expectedCatalogId,"The selected classic HFS entry was replaced");if(!value.expectedIdentity.empty())Check(e.identity==value.expectedIdentity,"The selected source was replaced or changed");return {volume,e.id};}
 throw HfsPlus::Error("The selected source no longer exists");
}
std::pair<std::shared_ptr<Volume>,uint32_t> Destination(const Location& value){
 if(value.Volume())return {value.Volume(),value.Id()};
 if(value.IsWindows()){Check(Absolute(value.WindowsPath()),"Use an absolute Windows destination path");auto volume=OpenWindows(value.WindowsPath());if(!value.expectedIdentity.empty())Check(FolderIdentity(volume->Stat(2).identity)==FolderIdentity(value.expectedIdentity),"The destination folder was replaced");return {volume,2};}
 return {OpenClassic(value.ClassicImage(),value.ClassicPath(),true),2};
}
std::unique_ptr<DirectFloppy> WritableFloppy(const Location& target,bool verify=false){
 int drive=target.IsPhysical()&&target.Media()==ClassicMedia::Floppy?target.Drive():-1;if(drive<0)return {};
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
 for(size_t i=0;i<sources.size();++i)if(sources[i].cut){Cancel(job);bool physical=sources[i].IsClassic()&&physical_floppy_path(sources[i].ClassicImage().c_str())>=0;
  fresh[i]=Source(sources[i],!physical);Check(MacBrowser::Snapshot(*fresh[i].first,fresh[i].second,job)==snapshots[i],"Source changed during the copy; source items were retained");if(physical)fresh[i].first->Close();
 }
 for(size_t i=0;i<sources.size();++i)if(sources[i].cut){Cancel(job);auto disk=WritableFloppy(sources[i],true);
  if(disk){fresh[i]=Source(sources[i],true);Check(MacBrowser::Snapshot(*fresh[i].first,fresh[i].second,job)==snapshots[i],"Source floppy changed before move cleanup; source items were retained");}
  Mark(completion,sources[i]);RemoveTree(*fresh[i].first,fresh[i].second,job);if(!sources[i].Volume())fresh[i].first->Close();Finish(disk);
 }
}
bool MoveInPlace(const OrdinaryCopy& request,const std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>>& sources,Volume& target,uint32_t parent,Storage::Job& job,Completion& completion){
 if(!target.Supports().rename)return false;
 for(size_t i=0;i<sources.size();++i)if(!request.sources[i].cut||!target.CanMoveFrom(*sources[i].first))return false;
 auto existing=target.List(parent);std::vector<Entry> entries;
 for(size_t i=0;i<sources.size();++i){auto& source=*sources[i].first;auto id=sources[i].second;Check(id!=2,"Select files or folders rather than a volume root");source.ValidateCopyTo(id,target,parent);auto entry=source.Stat(id);
  for(auto& e:existing)if(HfsPlus::CompareNames(e.name,entry.name,target.CaseSensitive())==0)return false;
  for(auto& e:entries)if(HfsPlus::CompareNames(e.name,entry.name,target.CaseSensitive())==0)return false;
  if(request.sources[i].Volume()&&entry.directory){std::set<uint32_t> visited;for(uint32_t at=parent;at!=1;at=target.Stat(at).parent){Check(at!=id&&visited.insert(at).second,"Cannot move a folder into itself or its descendants");}}
  Check(!entry.protectedFile&&entry.readError.empty()&&!(HfsPlus::Disk::U16(entry.finder.data()+8)&HFS_FNDR_NAMELOCKED),"The selected entry cannot be moved");entries.push_back(entry);
 }
 // Classic adapters use root-relative paths. Avoid invalidating a later
 // captured descendant path by renaming its selected ancestor first.
 for(size_t i=0;i<sources.size();++i)for(size_t j=0;j<sources.size();++j)if(i!=j&&request.sources[i].IsClassic()&&request.sources[j].IsClassic()){auto a=request.sources[i].ClassicPath(),b=request.sources[j].ClassicPath();if(b.size()>a.size()&&b[a.size()]==':'&&!_strnicmp(a.c_str(),b.c_str(),a.size()))return false;}
 Storage::Progress progress;progress.phase="Moving";progress.scanning=false;progress.fileCopy=progress.utf8=true;progress.totalFiles=unsigned(sources.size());job.progress(progress);
 for(size_t i=0;i<sources.size();++i){Cancel(job);Mark(completion,request.sources[i]);progress.path=Utf8(entries[i].name);job.progress(progress);target.MoveHere(*sources[i].first,sources[i].second,parent,entries[i]);Storage::OutcomeEvidence::NoteRetainedChange();++progress.files;job.progress(progress);}
 return true;
}
Storage::Result CopyRequest(const OrdinaryCopy& request,Storage::Job& job,Completion& completion){
 Check(!request.sources.empty(),"Select a source file or folder");HFSIFACE_get_dir_flush();auto disk=WritableFloppy(request.destination,std::any_of(request.sources.begin(),request.sources.end(),[](const Location& p){return p.cut;}));
 auto destination=Destination(request.destination);Check(destination.first->Supports().create,"The destination does not support file creation");
 std::vector<std::pair<std::shared_ptr<Volume>,uint32_t>> opened;std::vector<std::string> snapshots;
 for(auto& item:request.sources){Cancel(job);auto source=Source(item);if(item.cut&&item.Volume())Check(source.first->Writable(),"A cut source is read-only");opened.push_back(source);}
 for(size_t i=0;i<opened.size();++i)MarkChild(completion,request.destination,opened[i].first->Stat(opened[i].second).name);
 if(MoveInPlace(request,opened,*destination.first,destination.second,job,completion)){
  if(!request.destination.Volume())destination.first->Close();for(size_t i=0;i<request.sources.size();++i)if(!request.sources[i].Volume())opened[i].first->Close();Finish(disk);completion.complete=true;completion.completed=unsigned(opened.size());Storage::Result result;result.moved=true;return result;
 }
 for(size_t i=0;i<request.sources.size();++i)snapshots.push_back(request.sources[i].cut?MacBrowser::Snapshot(*opened[i].first,opened[i].second,job,bool(request.sources[i].Volume())):std::string());
 bool complete;try{
  if(CopyProcess::CopiesWithinMountedImage(request)){
   std::vector<uint32_t> ids;ids.reserve(opened.size());
   for(const auto& source:opened)ids.push_back(source.second);
   complete=CopyProcess::CopyWithinMountedImage(*destination.first,ids,destination.second,job);
  }else complete=CopyMany(opened,*destination.first,destination.second,job);
 }catch(...){if(disk&&disk->Error())throw Failure(disk->Error(),"Floppy write or verification failed (Windows error "+std::to_string(disk->Error())+"). The disk may be incomplete");throw;}
 if(!request.destination.Volume())destination.first->Close();
 for(size_t i=0;i<request.sources.size();++i)if(!request.sources[i].Volume())opened[i].first->Close();
 Finish(disk);disk.reset(); // Includes all final unmount writes; never delete sources before this.
 if(complete)DeleteMovedSources(request.sources,snapshots,job,completion);
 completion.complete=complete;completion.completed=job.progress().files;
 Storage::Result result;result.moved=complete&&std::any_of(request.sources.begin(),request.sources.end(),[](const Location& p){return p.cut;});return result;
}
Storage::Result Mutate(const RenameEntry* rename, const Location* items, size_t count,Storage::Job& job,Completion& completion){
 Storage::Progress progress;progress.fileCopy=false;progress.phase=rename?"Renaming":"Deleting";progress.scanning=false;progress.totalFiles=unsigned(count);progress.utf8=true;job.progress(progress);
 HFSIFACE_get_dir_flush();int silent=HFSIFACE_silencer(1);HFSIFACE_delete_cancel(CancelClassic,&job);struct Silent{int previous;~Silent(){HFSIFACE_delete_cancel(nullptr,nullptr);HFSIFACE_silencer(previous);}}scope{silent};
 for(size_t index=0;index<count;++index){const auto& item=items[index];Cancel(job);progress.path=item.Volume()?Utf8(item.expectedName):item.IsWindows()?Utf8(U16(item.WindowsPath())):Utf8(Ansi(item.ClassicPath()));job.progress(progress);
  if(item.Volume()){auto source=Source(item);auto e=source.first->Stat(source.second);Cancel(job);Check(e.id!=2,"Cannot rename or delete a volume root");
   if(rename){Check(rename->parent.IsNone()||(rename->parent.IsMounted()&&rename->parent.Volume()==source.first),"Rename must stay within the same mounted volume");Check(source.first->Supports().rename&&!e.protectedFile,"The selected entry cannot be renamed");if(rename->checkNameLock&&!strcmp(source.first->Kind(),"HFS"))Check(!(HfsPlus::Disk::U16(e.finder.data()+8)&HFS_FNDR_NAMELOCKED),"Remove the Finder name lock before renaming this entry");Mark(completion,item);if(!HostPath(item).empty()){auto path=HostPath(item);Location parent;parent.SetWindows(path.substr(0,path.find_last_of(L"\\/")+1));MarkChild(completion,parent,rename->name);}source.first->Rename(source.second,rename->parent.IsNone()?e.parent:rename->parent.Id(),rename->name);}
   else{Check(source.first->Supports().remove,"The selected entry cannot be deleted");Mark(completion,item);RemoveTree(*source.first,source.second,job,true);}
  }else if(item.IsWindows()){
   auto source=Source(item,true);Cancel(job);source.first->Close();
   std::string path; // ANSI legacy path is retained verbatim.
   int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,item.WindowsPath().c_str(),-1,nullptr,0,nullptr,nullptr);std::vector<char> converted(n);BOOL lost=FALSE;if(n)WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,item.WindowsPath().c_str(),-1,converted.data(),n,nullptr,&lost);Check(n&&!lost,"Windows path cannot be represented by the legacy backend");path=converted.data();Check(Absolute(path),"Use an absolute Windows path");
   Mark(completion,item);
   if(rename){Location parent;parent.SetWindows(item.WindowsPath().substr(0,item.WindowsPath().find_last_of(L"\\/")+1));MarkChild(completion,parent,rename->name);auto entry=source.first->Stat(source.second);Cancel(job);source.first->Rename(source.second,entry.parent,rename->name);
   }else if(!DeleteHostSelectionJob(CString(path.c_str()),item.directory,&job)){Cancel(job);throw Failure(GetLastError(),"Could not completely delete the selected Windows entry; remaining files were retained");}
  }else{
   Check(item.IsClassic()&&!item.ClassicImage().empty(),"Invalid classic HFS source");if(!item.expectedResource.empty())Check(VolumeCoord::ImageKey(item.ClassicImage())==item.expectedResource,"The selected source image was replaced");auto disk=WritableFloppy(item);auto image=item.ClassicImage(),path=item.ClassicPath();hfsdirent entry{};
   if(HFSIFACE_get_properties(&image[0],&path[0],&entry))throw HfsPlus::Error(ClassicError("Cannot read the selected HFS entry"));
   Cancel(job);if(item.expectedCatalogId)Check(uint32_t(entry.cnid)==item.expectedCatalogId,"The selected classic HFS entry was replaced");Check(entry.cnid!=2,"Cannot rename or delete a volume root");
   Mark(completion,item);int code;
   if(rename){Check(!rename->checkNameLock||!(entry.fdflags&HFS_FNDR_NAMELOCKED),"Remove the Finder name lock before renaming this entry");auto wide=Wide(rename->name);char leaf[32];BOOL lost=FALSE;int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,wide.c_str(),-1,leaf,ARRAYSIZE(leaf),nullptr,&lost);Check(n&&!lost&&HfvValidRenameLeaf(leaf,true),"Invalid classic HFS filename");auto to=path.substr(0,path.find_last_of(':')+1)+leaf;code=HFSIFACE_rename(&image[0],&path[0],&to[0]);}
   else code=item.directory?HFSIFACE_rmdir(&image[0],&path[0]):HFSIFACE_delete(&image[0],&path[0]);
   std::string error=ClassicError("Classic HFS operation failed");Finish(disk);if(code){Cancel(job);throw HfsPlus::Error(error);}
  }
  ++completion.completed;++progress.files;job.progress(progress);
 }
 completion.complete=true;return {};
}
Storage::Result ExportFork(const ExportFileFork& request,Storage::Job& job,Completion& completion){
 const auto fork=static_cast<unsigned>(request.fork);auto source=Source(request.source);auto entry=source.first->Stat(source.second);Check(!entry.directory,"Select a file for fork export");
 source.first->Read(source.second,fork,0,nullptr,0);auto& path=request.output.WindowsPath();
 HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw Failure(GetLastError(),"Cannot create output; choose a new filename");
 try{Mark(completion,request.output);std::vector<uint8_t> bytes(1024*1024);Storage::Progress progress;progress.fileCopy=progress.byteTransfer=progress.utf8=true;progress.scanning=false;progress.phase="Exporting fork";progress.totalFiles=1;progress.path=Utf8(entry.name);progress.totalBytes=progress.fileTotal=entry.forks[fork].size;
  for(uint64_t at=0;at<entry.forks[fork].size;){Cancel(job);size_t n=size_t((std::min)(uint64_t(bytes.size()),entry.forks[fork].size-at));source.first->Read(source.second,fork,at,bytes.data(),n);DWORD written;if(!WriteFile(file,bytes.data(),DWORD(n),&written,nullptr)||written!=n)throw Failure(GetLastError(),"Fork export write failed");at+=n;progress.bytes=progress.fileBytes=progress.processed=at;job.progress(progress);}
  Cancel(job);
  if(!FlushFileBuffers(file))throw Failure(GetLastError(),"Fork export flush failed");
  CloseHandle(file);
 }catch(...){
  FILE_DISPOSITION_INFO remove{TRUE};
  const bool deleted=SetFileInformationByHandle(file,FileDispositionInfo,&remove,sizeof(remove))!=FALSE;
  const auto error=GetLastError();
  CloseHandle(file);
  if(!deleted){Storage::OutcomeEvidence::NoteRetainedChange();throw Failure(error,"Could not remove the incomplete fork export");}
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
 volume->PutResourceReplacement(id,replacement,[&](unsigned fork,uint64_t at,void* data,size_t n){
  Cancel(job);
  if(fork)from.first->Read(from.second,0,at,data,n);
  else volume->Read(id,0,at,data,n);
  report(fork,at,n);
 },[&](uint64_t at,size_t n){report(0,at,n);});
 if(!request.input.Volume())from.first->Close();completion.complete=true;completion.completed=1;return {};
}

bool SameProperties(const hfsdirent& a,const hfsdirent& b){
 if(strcmp(a.name,b.name)||a.flags!=b.flags||a.cnid!=b.cnid||a.parid!=b.parid||a.crdate!=b.crdate||a.mddate!=b.mddate||a.bkdate!=b.bkdate||a.fdflags!=b.fdflags||a.fdlocation.h!=b.fdlocation.h||a.fdlocation.v!=b.fdlocation.v)return false;
 if(a.flags&HFS_ISDIR)return a.u.dir.valence==b.u.dir.valence&&a.u.dir.rect.top==b.u.dir.rect.top&&a.u.dir.rect.left==b.u.dir.rect.left&&a.u.dir.rect.bottom==b.u.dir.rect.bottom&&a.u.dir.rect.right==b.u.dir.rect.right;
 return a.u.file.dsize==b.u.file.dsize&&a.u.file.rsize==b.u.file.rsize&&!memcmp(a.u.file.type,b.u.file.type,4)&&!memcmp(a.u.file.creator,b.u.file.creator,4);
}
std::string FolderIdentity(std::string value){for(int n=0;n<2;++n){auto at=value.find_last_of(':');if(at==value.npos)return {};value.resize(at);}return value;}
Storage::Result Metadata(const Request& request,Storage::Job& job,Completion& completion){
 const auto* metadata = request.Metadata();
 const auto* folder = request.Get<CreateFolder>();
 Storage::Progress p;p.phase=request.OperationKind()==Kind::Inspect?"Reading properties":request.OperationKind()==Kind::CreateFolder?"Creating folder":"Updating properties";p.utf8=true;if(folder)p.path=Utf8(folder->name);if(metadata)if(auto* input=std::get_if<ImportAttributeValue>(metadata))p.path=Utf8(input->name);job.progress(p);
 if(auto* input=metadata?std::get_if<ImportAttributeValue>(metadata):nullptr){
  auto source=Source(input->item);auto e=source.first->Stat(source.second);Check(!e.directory,"Attribute value must be a file");
  auto bytes=HfsPlus::AttributeData::Snapshot(e.forks[0].size,[&](uint64_t at,void* p,size_t n){Cancel(job);source.first->Read(source.second,0,at,p,n);});Cancel(job);completion.attributes[input->name]=std::move(bytes);
 }else if(auto* output=metadata?std::get_if<ExportAttributeValue>(metadata):nullptr){
  const auto& bytes=output->value;const auto& path=output->item.WindowsPath();HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw Failure(GetLastError(),"Cannot create attribute output; choose a new filename");Mark(completion,output->item);
  try{HfsPlus::Bytes buffer(65536);for(uint64_t at=0;at<bytes.size();){Cancel(job);auto n=size_t((std::min)(uint64_t(buffer.size()),bytes.size()-at));bytes.Read(at,buffer.data(),n);DWORD written=0;if(!WriteFile(file,buffer.data(),DWORD(n),&written,nullptr)||written!=n)throw Failure(GetLastError(),"Attribute export write failed");at+=n;}Cancel(job);if(!FlushFileBuffers(file))throw Failure(GetLastError(),"Attribute export flush failed");CloseHandle(file);}catch(...){CloseHandle(file);DeleteFileW(path.c_str());throw;}
 }else if(folder){
  const auto& item=folder->parent;
  if(item.Volume()){auto source=Source(item);auto e=source.first->Stat(source.second);Cancel(job);Check(e.directory&&source.first->Supports().create&&source.first->Supports().createFolders&&(e.id==2||!e.protectedFile),"Cannot create a folder here");MarkChild(completion,item,folder->name);source.first->MakeDirectory(source.second,folder->name);}
  else if(item.IsWindows()){auto volume=OpenWindows(item.WindowsPath());auto e=volume->Stat(2);if(!item.expectedIdentity.empty())Check(!FolderIdentity(e.identity).empty()&&FolderIdentity(e.identity)==FolderIdentity(item.expectedIdentity),"The destination folder changed");Cancel(job);MarkChild(completion,item,folder->name);volume->MakeDirectory(2,folder->name);volume->Close();}
  else{Check(item.IsClassic(),"Invalid folder destination");if(!item.expectedResource.empty())Check(VolumeCoord::ImageKey(item.ClassicImage())==item.expectedResource,"The destination image was replaced");auto disk=WritableFloppy(item);CheckMedia(item);auto volume=OpenClassic(item.ClassicImage(),item.ClassicPath(),true);auto e=volume->Stat(2);Cancel(job);Check(e.directory,"Destination is not a folder");if(item.expectedCatalogId)Check(e.resolvedId==item.expectedCatalogId,"The destination HFS folder was replaced");MarkChild(completion,item,folder->name);volume->MakeDirectory(2,folder->name);volume->Close();Finish(disk);}
 }else{
  const auto& item=std::visit([](const auto& value) -> const Location& { return value.item; }, *metadata);auto source=Source(item);completion.entry=source.first->Stat(source.second);completion.description=source.first->Description();completion.writable=source.first->Writable();completion.supportsAttributes=source.first->SupportsAttributes();completion.basicMetadata=source.first->SupportsBasicMetadata();completion.supportsCompression=source.first->SupportsCompression();
  if(completion.supportsAttributes)completion.attributes=source.first->GetAttributes(source.second);Cancel(job);
  if(auto* edit=std::get_if<ChangeCompression>(metadata)){
   Check(completion.supportsCompression&&completion.writable,"This volume cannot change compression");auto& now=completion.entry;auto& old=edit->original;
   Check(now.id==old.id&&now.parent==old.parent&&now.name==old.name&&now.modified==old.modified&&now.created==old.created&&now.finder==old.finder&&now.compressed==old.compressed&&now.forks[0].size==old.forks[0].size&&now.forks[1].size==old.forks[1].size&&completion.attributes==edit->originalAttributes,"The file changed while Properties was open; reopen Properties");
   Cancel(job);Mark(completion,item);p.phase=edit->method!=HfsPlus::Volume::CompressionMode::Uncompressed?"Compressing file":"Decompressing file";p.path=Utf8(now.name);p.byteTransfer=true;p.scanning=false;p.totalBytes=now.forks[0].size+now.forks[1].size;
   source.first->SetCompressionMethod(source.second,edit->method,[&](uint64_t done,uint64_t total){Cancel(job);p.processed=p.bytes=done;p.totalBytes=total;job.progress(p);});completion.entry=source.first->Stat(source.second);
  }
  if(auto* edit=std::get_if<EditBasicMetadata>(metadata)){
   Check(completion.basicMetadata&&completion.writable,"This format has no writable basic metadata editor");auto& now=completion.entry;auto& old=edit->original;
   Check(now.id==old.id&&now.name==old.name&&now.version==old.version&&now.finder==old.finder&&now.created==old.created&&now.modified==old.modified&&now.protectedFile==old.protectedFile&&now.forks[0].size==old.forks[0].size&&now.forks[1].size==old.forks[1].size,"Properties changed while the editor was open; reopen Properties");Cancel(job);Mark(completion,item);source.first->SetBasicMetadata(source.second,edit->desired);completion.entry=edit->desired;
  }
  if(auto* edit=std::get_if<EditAttributes>(metadata)){Check(completion.supportsAttributes&&completion.writable,"This item cannot store extended attributes");Check(completion.attributes==edit->original,"Attributes changed while Properties was open; reopen Properties before saving");Cancel(job);Mark(completion,item);source.first->SetAttributes(source.second,edit->desired);completion.attributes=edit->desired;}

  if(item.IsMounted()) completion.classicProperties=source.first->GetClassicProperties(source.second,completion.properties);
  if(item.IsClassic()&&!item.Volume()){source.first->Close();auto image=item.ClassicImage(),path=item.ClassicPath();Check(HFSIFACE_get_properties(&image[0],&path[0],&completion.properties)==0,"Cannot read classic HFS properties");completion.classicProperties=true;completion.writable=physical_floppy_path(item.ClassicImage().c_str())>=0?true:!readonly_media_path(item.ClassicImage().c_str())&&(GetFileAttributesA(item.ClassicImage().c_str())&FILE_ATTRIBUTE_READONLY)==0;Cancel(job);}
  if(auto* edit=std::get_if<EditClassicProperties>(metadata)){
   Check(completion.classicProperties && completion.writable,"This item does not expose writable classic Finder properties");
   auto current=completion.properties;
   Check(!item.expectedCatalogId||uint32_t(current.cnid)==item.expectedCatalogId,"The selected HFS entry was replaced");Cancel(job);
   auto changed=current;
   if(auto* label=std::get_if<FinderLabel>(&edit->change)){Check(label->value>=0&&label->value<=7,"Invalid Finder label");changed.fdflags=short((changed.fdflags&~14)|(label->value<<1));}
   else if(auto* position=std::get_if<FinderPosition>(&edit->change)){changed.fdlocation.h=position->x;changed.fdlocation.v=position->y;changed.fdflags&=~HFS_FNDR_HASBEENINITED;}
   else{const auto& properties=std::get<ClassicPropertyEdit>(edit->change);Check(SameProperties(current,properties.original),"Properties changed while the editor was open; reopen Properties before applying changes");changed=properties.desired;
    Check(changed.cnid==current.cnid&&changed.parid==current.parid&&((changed.flags^current.flags)&HFS_ISDIR)==0,"Catalog identity and item kind cannot be changed");
    if(!(current.flags&HFS_ISDIR))Check(changed.u.file.dsize==current.u.file.dsize&&changed.u.file.rsize==current.u.file.rsize,"Fork lengths cannot be changed through Properties");
   }
   if(item.IsMounted()){
    Cancel(job);Mark(completion,item);source.first->SetClassicProperties(source.second,changed);
    completion.complete=true;completion.completed=1;return {};
   }
   auto disk=WritableFloppy(item);auto image=item.ClassicImage(),path=item.ClassicPath();
   auto to=path;char name[32]{};strcpy_s(name,changed.name);if(std::holds_alternative<ClassicPropertyEdit>(edit->change)){mac_to_pc_charset((unsigned char*)name);Check(HfvValidRenameLeaf(name,true),"Invalid classic HFS name");to=path.substr(0,path.find_last_of(':')+1)+name;}
   std::vector<char> macPath(path.begin(),path.end()),macTo(to.begin(),to.end());macPath.push_back(0);macTo.push_back(0);pc_to_mac_charset((unsigned char*)macPath.data());pc_to_mac_charset((unsigned char*)macTo.data());
   hfsvol* volume=hfs_mount(&image[0],0,O_RDWR);Check(volume!=nullptr,"Cannot open classic HFS for editing");
   try{Cancel(job);Mark(completion,item);int code=hfs_editattrs(volume,macPath.data(),macTo.data(),&changed);auto error=code?ClassicError("Cannot update properties; rollback was attempted"):std::string();int closed=hfs_umount(volume);volume=nullptr;if(closed&&error.empty())error=ClassicError("Cannot close the updated HFS volume");Check(code==0&&closed==0,error.c_str());Finish(disk);
   }catch(...){if(volume)hfs_umount(volume);throw;}

  }
 }
 completion.complete=true;completion.completed=1;return {};
}
std::string Resource(const Location& item){return item.ResourceKey();}
void AddPath(std::vector<VolumeCoord::Access>& access,const std::wstring& path,VolumeCoord::Mode mode,bool companion=true){access.push_back({VolumeCoord::PathKey(path),mode});if(companion)access.push_back({VolumeCoord::PathKey(CompanionPath(path)),mode});}
std::u16string SourceName(const Location& item){if(!item.expectedName.empty())return item.expectedName;auto path=HostPath(item);if(!path.empty())return U16(path.substr(path.find_last_of(L"\\/")+1));if(item.IsClassic()&&!item.ClassicPath().empty())return Ansi(item.ClassicPath().substr(item.ClassicPath().find_last_of(':')+1));return {};}
void ValidateRequest(const Request& r){
 Check(int(r.OperationKind())>=0&&int(r.OperationKind())<=int(Kind::SetCompression),"Unknown file operation");
 if (const auto* rename = r.Rename()) {
  Check(!rename->source.IsNone(), "Select an entry to rename");
  Check(!rename->name.empty(), "Enter a new name");
  Check(rename->parent.IsNone() ||
        (rename->source.IsMounted() && rename->parent.IsMounted() &&
         rename->source.Volume() == rename->parent.Volume()),
        "Rename must stay within the same mounted volume");
  return;
 }
 if (const auto* deletion = r.Delete()) {
  Check(!deletion->selection.empty(), "Select entries to delete");
  for (const auto& item : deletion->selection)
   Check(!item.IsNone(), "No source file location was supplied");
  return;
 }
 if (const auto* metadata = r.Metadata()) {
  const auto& item = std::visit([](const auto& value) -> const Location& { return value.item; }, *metadata);
  Check(!item.IsNone(), "Choose a location for the metadata operation");
  if (std::holds_alternative<ExportAttributeValue>(*metadata))
   Check(item.IsWindows(), "Choose a Windows output filename");
  if (auto* input = std::get_if<ImportAttributeValue>(metadata))
   Check(!input->name.empty(), "Enter an attribute name");
  return;
 }
 if (const auto* copy = r.Get<OrdinaryCopy>()) {
  Check(!copy->sources.empty() && !copy->destination.IsNone(), "Choose sources and a copy destination");
  for (const auto& source : copy->sources) Check(!source.IsNone(), "No source file location was supplied");
 } else if (const auto* legacy = r.Conversion()) {
  legacy->source.LegacyVolume(); legacy->source.LegacyPath();
  legacy->destination.LegacyVolume(); legacy->destination.LegacyPath();
  if (legacy->selection)
   Check(!legacy->selection->source.IsNone() &&
         (legacy->selection->folder.IsWindows() || legacy->selection->folder.IsClassic() ||
          (legacy->selection->folder.IsMounted() && !strcmp(legacy->selection->folder.Volume()->Kind(),"HFS"))),
         "Invalid conversion selection");
 } else if (const auto* folder = r.Get<CreateFolder>()) {
  Check(!folder->parent.IsNone() && !folder->name.empty(), "Choose a destination and folder name");
 } else if (const auto* exportFork = r.Get<ExportFileFork>()) {
  Check(!exportFork->source.IsNone() && exportFork->output.IsWindows() &&
        (exportFork->fork == ForkSelector::Data || exportFork->fork == ForkSelector::Resource), "Invalid fork export request");
 } else if (const auto* importFork = r.Get<ImportResourceFork>()) {
  Check(!importFork->input.IsNone() && (importFork->file.IsMounted() || importFork->file.IsWindows()),
        "Invalid resource import request");
 } else if (const auto* link = r.Get<CreateHardLink>()) {
  Check(!link->source.IsNone() && !link->parent.IsNone() && !link->name.empty(), "Choose a hard link source, destination and name");
 }
}
std::vector<VolumeCoord::Access> Resources(const Request& request){
 using VolumeCoord::Mode;std::vector<VolumeCoord::Access> access;
 auto reserveMutation = [&](const Location& item) {
  auto path = HostPath(item);
  if (path.empty()) access.push_back({Resource(item), Mode::Write});
  else AddPath(access, path, Mode::Write);
  return path;
 };
 if (const auto* rename = request.Rename()) {
  auto path = reserveMutation(rename->source);
  if (!path.empty()) {
   auto parent = path.substr(0, path.find_last_of(L"\\/") + 1);
   auto explicitParent = rename->parent.IsNone() ? std::wstring{} : HostPath(rename->parent);
   AddPath(access, ChildPath(explicitParent.empty() ? parent : explicitParent, rename->name), Mode::Write);
  }
  return access;
 }
 if (const auto* deletion = request.Delete()) {
  for (const auto& item : deletion->selection) reserveMutation(item);
  return access;
 }
 if(request.OperationKind()==Kind::LegacyCopy){
  if(request.Legacy().source.IsWindows())AddPath(access,request.Legacy().source.WindowsPath(),request.Legacy().move?Mode::Write:Mode::Read);else access.push_back({request.Legacy().source.ResourceKey(),(request.Legacy().move||request.background||CopyProcess::UsesWorker(request))?Mode::Write:Mode::Read});
  if(request.Legacy().destination.IsWindows())access.push_back({VolumeCoord::LegacyOutputKey(request.Legacy().destination.WindowsPath()),Mode::Write});else access.push_back({request.Legacy().destination.ResourceKey(),Mode::Write});return access;
 }
 if (const auto* metadata = request.Metadata()) {
  const auto& item = std::visit([](const auto& value) -> const Location& { return value.item; }, *metadata);
  const auto kind = request.OperationKind();
  const bool read = kind == Kind::Inspect || kind == Kind::ReadAttributeFile;
  auto path = HostPath(item);
  auto mode = read ? Mode::Read : Mode::Write;
  if (path.empty()) access.push_back({Resource(item), read && request.background ? Mode::Write : mode});
  else AddPath(access, path, mode, kind != Kind::ExportAttribute);
  return access;
 }
 auto reserve = [&](const Location& item, Mode mode, bool companions = true) {
  auto path = HostPath(item);
  if (path.empty()) access.push_back({Resource(item), mode});
  else AddPath(access, path, mode, companions);
 };
 auto reserveChild = [&](const Location& parent, const std::u16string& name, bool companions) {
  auto path = HostPath(parent);
  if (path.empty()) access.push_back({Resource(parent), Mode::Write});
  else AddPath(access, ChildPath(path, name), Mode::Write, companions);
 };
 auto reserveCopySource = [&](const Location& source) {
  const auto mode = source.cut || (HostPath(source).empty() &&
      (request.background || CopyProcess::UsesWorker(request))) ? Mode::Write : Mode::Read;
  reserve(source, mode);
 };
 if (const auto* copy = request.Get<OrdinaryCopy>()) {
  for (const auto& source : copy->sources) reserveCopySource(source);
  auto path = HostPath(copy->destination);
  if (path.empty()) access.push_back({Resource(copy->destination), Mode::Write});
  else for (const auto& source : copy->sources) {
   auto name = SourceName(source);
   if (name.empty()) { AddPath(access, path, Mode::Write, false); break; }
   AddPath(access, ChildPath(path, name), Mode::Write);
  }
 } else if (const auto* folder = request.Get<CreateFolder>()) {
  reserveChild(folder->parent, folder->name, false);
 } else if (const auto* link = request.Get<CreateHardLink>()) {
  reserve(link->source, HostPath(link->source).empty() ? Mode::Write : Mode::Read);
  reserveChild(link->parent, link->name, true);
 } else if (const auto* exportFork = request.Get<ExportFileFork>()) {
  reserveCopySource(exportFork->source);
  reserve(exportFork->output, Mode::Write, false);
 } else if (const auto* importFork = request.Get<ImportResourceFork>()) {
  reserveCopySource(importFork->input);
  reserve(importFork->file, Mode::Write);
 }
 return access;
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

std::optional<Storage::Result> CopyConverted(const LegacyConversion& conversion, Storage::Job& job,
                                              Completion& completion) {
 const bool importing=conversion.source.IsWindows() && conversion.destination.IsClassic();
 const bool exporting=conversion.source.IsClassic() && conversion.destination.IsWindows();
 if(!importing && !exporting) return {};
 char mode=importing ? conversion.options.input : conversion.options.output;
 const bool mounted=CopyProcess::ConvertsWithMountedClassic(conversion);
 const bool raw=mounted && (mode=='r' || mode=='R');
 if(!TransferConversion::Supported(mode) && mode!='a' && !raw) return {};
 const auto mappings=raw?TypeMappings::Capture():TypeMappings::Snapshot{};
 auto sourceLocation=conversion.selection ? conversion.selection->source : conversion.source;
 sourceLocation.cut=conversion.move;
 auto source=Source(sourceLocation);
 Location folder=conversion.selection ? conversion.selection->folder : conversion.destination;
 std::u16string outputName;
 if(!conversion.selection && exporting) {
  auto path=folder.WindowsPath();
  auto flags=GetFileAttributesW(path.c_str());
  if(flags==INVALID_FILE_ATTRIBUTES || !(flags&FILE_ATTRIBUTE_DIRECTORY)) {
   auto end=path.find_last_of(L"\\/");
   outputName=U16(path.substr(end+1));
   folder.SetWindows(path.substr(0,end+1));
  }
 } else if(!conversion.selection && importing) {
  auto path=folder.ClassicPath();
  if(path.back()!=':') {
   auto image=folder.ClassicImage();
   hfsdirent entry{};
   if(HFSIFACE_get_properties(&image[0],&path[0],&entry)!=0 || !(entry.flags&HFS_ISDIR)) {
    const auto end=path.find_last_of(':');
    outputName=Ansi(path.substr(end+1));
    folder.SetClassic(std::move(image),path.substr(0,end+1));
    if(conversion.destination.MediaGenerationKnown())
     folder.BindMediaGeneration(conversion.destination.MediaGeneration());
   }
  }
 }
 auto disk=WritableFloppy(folder,conversion.move || conversion.options.verifyFloppy);
 CheckMedia(folder);
 auto target=Destination(folder);
 std::vector<std::string> snapshots;
 if(conversion.move) snapshots.push_back(MacBrowser::Snapshot(*source.first,source.second,job,bool(sourceLocation.Volume())));
 TransferAdapter adapt=[&](Volume& volume,Entry& entry,Storage::Job& active,bool root) {
  char selected=mode;
  bool paired=false;
  if(importing && (selected=='a' || raw) && conversion.options.appledouble && !entry.directory) {
   const auto path=volume.HostPath(entry.id);
   const auto slash=path.find_last_of(L"\\/");
   if(slash!=path.npos)paired=WindowsResourceCompanion(path.substr(0,slash+1)+L"%"+path.substr(slash+1));
  }
  if(selected=='a' && !entry.directory) {
   if(importing) {
    const auto dot=entry.name.rfind(u'.');
    const auto suffix=dot==entry.name.npos ? std::wstring{} : Wide(entry.name.substr(dot));
    if(paired) selected='&';
    else if(!_wcsicmp(suffix.c_str(),L".hqx")) selected='b';
    else if(!_wcsicmp(suffix.c_str(),L".bin")) selected='m';
    else if(!_wcsicmp(suffix.c_str(),L".txt") || !_wcsicmp(suffix.c_str(),L".c") ||
            !_wcsicmp(suffix.c_str(),L".h")) selected='t';
    else {
     selected='r';
     memcpy(entry.finder.data(),"????UNIX",8);
    }
   } else if(!memcmp(entry.finder.data(),"TEXT",4) || !memcmp(entry.finder.data(),"ttro",4)) selected='t';
   else selected=entry.forks[1].size ? 'm' : 'r';
  }
  Reader reader;
  if(raw && !entry.directory && !paired) reader=AdaptRawClassic(volume,entry,importing,mode=='R',mappings);
  else if(TransferConversion::Supported(selected)) reader=TransferConversion::Adapt(selected,importing,volume,entry,active);
  if(root && !outputName.empty()) entry.name=outputName;
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
 Mark(completion,folder);
 bool complete=CopyMany({source},*target.first,target.second,job,adapt);
 if(!folder.Volume())target.first->Close();
 if(!sourceLocation.Volume())source.first->Close();
 Finish(disk);
 disk.reset();
 if(complete && conversion.move) DeleteMovedSources({sourceLocation},snapshots,job,completion);
 completion.complete=complete;
 completion.completed=job.progress().files;
 Storage::Result result;
 result.moved=complete && conversion.move;
 return result;
}

Storage::Result Execute(const Request& request,Storage::Job& job,Completion& completion){
 HfsPlus::AttributeReadScope attributeReads([&]{Cancel(job);});Cancel(job);
 Cancel(job);
 if(const auto* link=request.Get<CreateHardLink>()){auto source=Source(link->source);auto target=Destination(link->parent);Check(source.first->SupportsHardLinks()&&target.first->SupportsHardLinks()&&source.first->ResourceKey()==target.first->ResourceKey(),"Hard links require the same writable HFS+/HFSX volume");Cancel(job);Mark(completion,link->source);MarkChild(completion,link->parent,link->name);Storage::Progress progress;progress.scanning=false;progress.phase="Creating hard link";progress.totalFiles=1;job.progress(progress);target.first->MakeHardLink(source.second,target.second,link->name);progress.files=1;job.progress(progress);completion.complete=true;completion.completed=1;return {};}
 if(request.OperationKind()==Kind::CreateFolder||request.Metadata())return Metadata(request,job,completion);
 if(const auto* operation=request.Get<OrdinaryCopy>())return CopyRequest(*operation,job,completion);
 if(const auto* operation=request.Get<ExportFileFork>())return ExportFork(*operation,job,completion);
 if(const auto* operation=request.Get<ImportResourceFork>())return ReplaceResource(*operation,job,completion);
 if (const auto* rename = request.Rename())
  return Mutate(rename, &rename->source, 1, job, completion);
 if (const auto* deletion = request.Delete())
  return Mutate(nullptr, deletion->selection.data(), deletion->selection.size(), job, completion);
 const auto& legacy = request.Legacy();
 if(auto converted=CopyConverted(legacy,job,completion))return *converted;
 // Legacy containers cannot carry arbitrary attributes/ACLs. Preflight the
 // complete selection before converters can create partial output.
 auto noLostMetadata=[&](MacBrowser::Volume& volume,uint32_t root){
  if(!volume.SupportsAttributes())return;
  std::set<uint32_t> ancestors;
  std::function<void(uint32_t,unsigned)> walk=[&](uint32_t id,unsigned depth){
   Cancel(job);Check(depth<=128&&ancestors.insert(id).second,"Recursive conversion source");
   Check(volume.GetAttributes(id).empty(),"Legacy conversion cannot preserve extended attributes or Mac ACLs; use an HFS+/HFSX or Windows both-fork copy");
   auto entry=volume.Stat(id);Check(!legacy.destination.IsClassic()||!entry.datesValid||(entry.created<=UINT32_MAX&&entry.modified<=UINT32_MAX),"Classic HFS conversion cannot preserve dates after February 2040; source retained");if(entry.directory)for(auto& child:volume.List(id))walk(child.id,depth+1);
   ancestors.erase(id);
  };
  walk(root,0);
 };
 if(!legacy.selection&&request.Legacy().source.IsWindows()){
  auto source=MacBrowser::ResolveWindowsFile(request.Legacy().source.WindowsPath());
  noLostMetadata(*source.first,source.second);source.first->Close();
 }
 const bool mountedClassic=legacy.selection &&
    ((legacy.source.IsClassic() && legacy.selection->source.IsMounted()) ||
     (legacy.destination.IsClassic() && legacy.selection->folder.IsMounted()));
 if(legacy.selection){
  // Captured paths/identities never depend on another window's volume indexes.
  auto source=Source(legacy.selection->source);noLostMetadata(*source.first,source.second);
  if(legacy.move&&mountedClassic)Check(source.first->Writable(),"A cut source is read-only");
  auto& folder=legacy.selection->folder;
  auto target=folder.IsMounted()?Destination(folder):
    std::make_pair(folder.IsWindows()?OpenWindows(folder.WindowsPath()):OpenClassic(folder.ClassicImage(),folder.ClassicPath(),false),uint32_t(2));
  if(folder.IsMounted())Check(target.first->Writable(),"The destination is read-only");
  CheckMedia(folder);source.first->ValidateCopyTo(source.second,*target.first,target.second);
  source.first->Close();target.first->Close();
 }
  Mark(completion,request.Legacy().destination);if(request.Legacy().move)Mark(completion,request.Legacy().source);
 auto sourceLocation=!legacy.selection?request.Legacy().source:legacy.selection->source;sourceLocation.cut=request.Legacy().move;
 bool physicalMove=request.Legacy().move&&((sourceLocation.IsClassic()&&physical_floppy_path(sourceLocation.ClassicImage().c_str())>=0)||physical_floppy_path(request.Legacy().destination.LegacyVolume().c_str())>=0);
 std::vector<std::string> snapshots;auto options=request.Legacy().options;options.targetGenerationKnown=request.Legacy().destination.MediaGenerationKnown();options.targetGeneration=options.targetGenerationKnown?request.Legacy().destination.MediaGeneration():0;
 if(physicalMove){auto source=Source(sourceLocation);snapshots.push_back(MacBrowser::Snapshot(*source.first,source.second,job));if(!sourceLocation.Volume())source.first->Close();options.verifyFloppy=true;}
 std::shared_ptr<Volume> mediaGuard;if(request.Legacy().source.MediaGenerationKnown()){mediaGuard=OpenClassic(request.Legacy().source.ClassicImage(),":",false);CheckMedia(request.Legacy().source);}
 struct ClassicSessionMounts {
  int previous;
  explicit ClassicSessionMounts(bool enabled):previous(hfs_session_mounts(enabled)){}
  ~ClassicSessionMounts(){hfs_session_mounts(previous);}
 } classicMounts(mountedClassic);
 auto result=MakeCopyJob(request.Legacy().source.LegacyVolume(),request.Legacy().source.LegacyPath(),request.Legacy().destination.LegacyVolume(),request.Legacy().destination.LegacyPath(),request.Legacy().directory,physicalMove?0:request.Legacy().move,options)(job);
 if(mediaGuard)mediaGuard->Close();
 completion.completed=job.progress().files;bool copied=!result.code&&!job.progress().skipped;
 if(physicalMove&&copied){DeleteMovedSources({sourceLocation},snapshots,job,completion);result.moved=true;}
 completion.complete=copied;
 return result;
}
}
Operation::Operation(std::shared_ptr<State> state,std::shared_ptr<Storage::Job> job):state_(std::move(state)),job_(std::move(job)){}
Completion Operation::Snapshot()const{std::lock_guard<std::mutex> lock(state_->mutex);return state_->completion;}
namespace {
void FinalizeOutcome(Storage::Result& result, Completion& completion, Storage::Job& job,
                     const Storage::OutcomeEvidence& evidence) {
 const auto progress=job.progress();
 Storage::OutcomeEvidence classic;
 Storage::OutcomeEvidence::NoteRollback(static_cast<Storage::Rollback>(hfs_take_rollback()));
 if(HFSIFACE_take_delete_retained_change())Storage::OutcomeEvidence::NoteRetainedChange();
 result.Finalize(evidence,completion.completed,progress.skipped);
 result.Finalize(classic,progress.files,progress.skipped);
 completion.completed=result.completed;
}
}
Storage::Result ExecuteInWorker(const Request& request,Storage::Job& job,Completion& completion){
 Storage::OutcomeEvidence evidence;
 hfs_take_rollback(); HFSIFACE_take_delete_retained_change(); // Discard anything outside this operation on the executor thread.
 Storage::Result result;
 try{ValidateRequest(request);LegacyEngine::Scope engine;VolumeCoord::Lease access(Resources(request));VolumeCoord::OperationLease current(Resources(request));result=Execute(request,job,completion);}
 catch(const Storage::Cancelled&){result={1,ERROR_CANCELLED,"Cancelled"};}
 catch(const Failure& e){result={1,e.error,e.what()};}
 catch(const std::exception& e){result={1,ERROR_UNHANDLED_EXCEPTION,e.what()};}
 catch(...){result={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected copy-worker failure"};}
 FinalizeOutcome(result,completion,job,evidence);
 return result;
}
Operation Submit(Request request){
 auto state=std::make_shared<Operation::State>();auto values=std::make_shared<const Request>(std::move(request));
 auto& queue=values->background?Storage::copyWorker():Storage::worker();
 auto access=std::make_shared<std::unique_ptr<VolumeCoord::Lease>>();
 struct Plan {std::atomic<bool> ready{false};std::vector<VolumeCoord::Access> resources;Storage::Result error;};auto plan=std::make_shared<Plan>();
 auto planning=queue.submit([plan,values](Storage::Job& job){try{if(job.cancelled())throw Storage::Cancelled();ValidateRequest(*values);plan->resources=Resources(*values);}catch(const Storage::Cancelled&){plan->error={1,ERROR_CANCELLED,"Cancelled while identifying Windows paths"};}catch(const std::exception& error){plan->error={1,ERROR_UNHANDLED_EXCEPTION,error.what()};}catch(...){plan->error={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected path planning failure"};}plan->ready.store(true,std::memory_order_release);return Storage::Result{};});
 auto job=queue.submit([state,values,access,plan](Storage::Job& job){Completion completion;Storage::Result result;Storage::OutcomeEvidence evidence;hfs_take_rollback();HFSIFACE_take_delete_retained_change();
  try{Cancel(job);if(plan->error.code){result=plan->error;}else{if(!*access)throw VolumeCoord::Busy();LegacyEngine::Scope engine;
   VolumeCoord::OperationLease currentPaths(Resources(*values));if(CopyProcess::UsesWorker(*values)){std::vector<std::string> keys,writes;for(auto& item:plan->resources){keys.push_back(item.key);if(item.mode==VolumeCoord::Mode::Write)writes.push_back(item.key);}if(auto* copy=values->Get<OrdinaryCopy>()){Mark(completion,copy->destination);for(auto& item:copy->sources)if(item.cut)Mark(completion,item);}else if(auto* legacy=values->Conversion()){Mark(completion,legacy->destination);if(legacy->move)Mark(completion,legacy->source);}else if(auto* output=values->Get<ExportFileFork>())Mark(completion,output->output);else if(auto* input=values->Get<ImportResourceFork>())Mark(completion,input->file);result=CopyProcess::Run(*values,job,completion,keys,writes);}else result=Execute(*values,job,completion);}
  }
  catch(const VolumeCoord::Busy& e){result={1,ERROR_BUSY,e.what()};}catch(const Storage::Cancelled&){result={1,ERROR_CANCELLED,"Cancelled"};}
  catch(const Failure& e){result={1,e.error,e.what()};}catch(const std::exception& e){result={1,ERROR_UNHANDLED_EXCEPTION,e.what()};}
  catch(...){result={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected file-operation failure"};}
  FinalizeOutcome(result,completion,job,evidence);
  std::vector<std::string> changed;
  for(auto& item:completion.affected)changed.push_back(item.resourceKey);
  access->reset();VolumeCoord::Changed(changed);
  {std::lock_guard<std::mutex> lock(state->mutex);state->completion=std::move(completion);}return result;
 },[access,plan,values](const std::shared_ptr<Storage::Job>& job){
  if(!plan->ready.load(std::memory_order_acquire))return false;
  if(job->cancelled()||plan->error.code)return true;
  try{access->reset(new VolumeCoord::Lease(plan->resources));return true;}catch(const VolumeCoord::Busy&){return !values->background;}
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
