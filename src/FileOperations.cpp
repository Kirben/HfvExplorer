// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "AttributeReadScope.h"
#include "FileOperations.h"
#include "CopyProcess.h"
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
std::wstring HostPath(const Location& item){if(item.volume)return item.volume->HostPath(item.id);return item.type==LIST_FAT?item.windowsPath:std::wstring();}
std::wstring ChildPath(std::wstring root,const std::u16string& name){if(!root.empty()&&root.back()!=L'\\')root+=L'\\';return root+MacBrowser::Wide(name);}
std::wstring CompanionPath(const std::wstring& path){auto at=path.find_last_of(L"\\/");return path.substr(0,at+1)+L"%"+path.substr(at+1);}
std::string FolderIdentity(std::string);
void Mark(Completion& c,const Location& item){Change value;value.type=item.type;value.volume=item.volume;value.windowsPath=item.windowsPath;value.classicImage=item.classicImage;value.classicPath=item.classicPath;auto path=HostPath(item);if(!path.empty()){value.type=LIST_FAT;value.windowsPath=path;value.volume.reset();}c.affected.push_back(std::move(value));}
void MarkChild(Completion& c,const Location& folder,const std::u16string& name){auto path=HostPath(folder);if(path.empty()){Mark(c,folder);return;}Location child;child.type=LIST_FAT;child.windowsPath=ChildPath(path,name);Mark(c,child);}
int CancelClassic(void* job){return static_cast<Storage::Job*>(job)->cancelled()?1:0;}
void Cancel(Storage::Job& job){if(job.cancelled())throw Storage::Cancelled();}
std::u16string Ansi(const std::string& s){int n=MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),nullptr,0);std::wstring w(n,0);if(n)MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),&w[0],n);return U16(w);}
std::string Utf8(const std::u16string& text){auto w=Wide(text);int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);std::string out(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),&out[0],n,nullptr,nullptr);return out;}
bool Absolute(const std::string& p){return p.size()>=3&&((isalpha((unsigned char)p[0])&&p[1]==':'&&p[2]=='\\')||(p[0]=='\\'&&p[1]=='\\'));}
bool Absolute(const std::wstring& p){return p.size()>=3&&((iswalpha(p[0])&&p[1]==L':'&&(p[2]==L'\\'||p[2]==L'/'))||(p[0]==L'\\'&&p[1]==L'\\'));}
void CheckMedia(const Location& item){
 if(!item.mediaGenerationKnown)return;DWORD generation=0;
 int floppy=physical_floppy_path(item.classicImage.c_str());bool valid=floppy>=0?physical_floppy_generation(floppy,&generation)!=0:readonly_media_path(item.classicImage.c_str())&&physical_cd_generation(toupper((unsigned char)item.classicImage[0])-'A',&generation);
 if(!valid||generation!=item.mediaGeneration)throw Failure(ERROR_MEDIA_CHANGED,"Media changed after selection; select the volume again");
}
std::pair<std::shared_ptr<Volume>,uint32_t> Source(const Location& value,bool mutate=false){
 if(!value.expectedResource.empty())Check(VolumeCoord::ImageKey(value.classicImage)==value.expectedResource,"The selected source image was replaced");
 if(value.volume){CheckMedia(value);auto e=value.volume->Stat(value.id);
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
 if(value.type==LIST_FAT){Check(Absolute(value.windowsPath),"Use an absolute Windows destination path");auto volume=OpenWindows(value.windowsPath);if(!value.expectedIdentity.empty())Check(FolderIdentity(volume->Stat(2).identity)==FolderIdentity(value.expectedIdentity),"The destination folder was replaced");return {volume,2};}
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
  Check(!entry.protectedFile&&entry.readError.empty()&&!(HfsPlus::Disk::U16(entry.finder.data()+8)&HFS_FNDR_NAMELOCKED),"The selected entry cannot be moved");entries.push_back(entry);
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
 for(size_t i=0;i<opened.size();++i)MarkChild(completion,request.target,opened[i].first->Stat(opened[i].second).name);
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
   if(request.kind==Kind::Rename){Check(!request.target.volume||request.target.volume==source.first,"Rename must stay within the same volume");Check(source.first->Supports().rename&&!e.protectedFile,"The selected entry cannot be renamed");Mark(completion,item);if(!HostPath(item).empty()){auto path=HostPath(item);Location parent;parent.type=LIST_FAT;parent.windowsPath=path.substr(0,path.find_last_of(L"\\/"));MarkChild(completion,parent,request.name);}source.first->Rename(source.second,request.target.id,request.name);}
   else{Check(source.first->Supports().remove,"The selected entry cannot be deleted");Mark(completion,item);RemoveTree(*source.first,source.second,job,true);}
  }else if(item.type==LIST_FAT){
   auto source=Source(item,true);Cancel(job);source.first->Close();
   std::string path; // ANSI legacy path is retained verbatim.
   int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,item.windowsPath.c_str(),-1,nullptr,0,nullptr,nullptr);std::vector<char> converted(n);BOOL lost=FALSE;if(n)WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,item.windowsPath.c_str(),-1,converted.data(),n,nullptr,&lost);Check(n&&!lost,"Windows path cannot be represented by the legacy backend");path=converted.data();Check(Absolute(path),"Use an absolute Windows path");
   Mark(completion,item);
   if(request.kind==Kind::Rename){Location parent;parent.type=LIST_FAT;parent.windowsPath=item.windowsPath.substr(0,item.windowsPath.find_last_of(L"\\/"));MarkChild(completion,parent,request.name);auto entry=source.first->Stat(source.second);Cancel(job);source.first->Rename(source.second,entry.parent,request.name);
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
 replacement.forks[1].size=from.first->Stat(from.second).forks[0].size;replacement.modified=uint64_t(time(nullptr))+2082844800ULL;Cancel(job);
 Storage::Progress progress;progress.fileCopy=progress.byteTransfer=progress.utf8=true;progress.scanning=false;progress.phase="Importing resource fork";progress.totalFiles=1;progress.path=Utf8(original.name);progress.totalBytes=progress.fileTotal=replacement.forks[0].size+replacement.forks[1].size;
 uint64_t readThrough[2]={};Mark(completion,request.target);
 volume->Put(original.parent,replacement,[&](unsigned fork,uint64_t at,void* data,size_t n){Cancel(job);if(fork)from.first->Read(from.second,0,at,data,n);else volume->Read(id,0,at,data,n);Cancel(job);auto end=at+n,added=end>readThrough[fork]?end-readThrough[fork]:0;readThrough[fork]=(std::max)(readThrough[fork],end);progress.bytes+=added;progress.fileBytes+=added;progress.processed+=added;job.progress(progress);},true);
 if(!request.sources[0].volume)from.first->Close();completion.complete=true;completion.completed=1;return {};
}

bool SameProperties(const hfsdirent& a,const hfsdirent& b){
 if(strcmp(a.name,b.name)||a.flags!=b.flags||a.cnid!=b.cnid||a.parid!=b.parid||a.crdate!=b.crdate||a.mddate!=b.mddate||a.bkdate!=b.bkdate||a.fdflags!=b.fdflags||a.fdlocation.h!=b.fdlocation.h||a.fdlocation.v!=b.fdlocation.v)return false;
 if(a.flags&HFS_ISDIR)return a.u.dir.valence==b.u.dir.valence&&a.u.dir.rect.top==b.u.dir.rect.top&&a.u.dir.rect.left==b.u.dir.rect.left&&a.u.dir.rect.bottom==b.u.dir.rect.bottom&&a.u.dir.rect.right==b.u.dir.rect.right;
 return a.u.file.dsize==b.u.file.dsize&&a.u.file.rsize==b.u.file.rsize&&!memcmp(a.u.file.type,b.u.file.type,4)&&!memcmp(a.u.file.creator,b.u.file.creator,4);
}
std::string FolderIdentity(std::string value){for(int n=0;n<2;++n){auto at=value.find_last_of(':');if(at==value.npos)return {};value.resize(at);}return value;}
Storage::Result Metadata(const Request& request,Storage::Job& job,Completion& completion){
 Storage::Progress p;p.phase=request.kind==Kind::Inspect?"Reading properties":request.kind==Kind::CreateFolder?"Creating folder":"Updating properties";p.utf8=true;p.path=Utf8(request.name);job.progress(p);
 if(request.kind==Kind::ReadAttributeFile){
  Check(request.sources.size()==1,"Select one binary attribute value file");auto source=Source(request.sources[0]);auto e=source.first->Stat(source.second);Check(!e.directory,"Attribute value must be a file");
  auto bytes=HfsPlus::AttributeData::Snapshot(e.forks[0].size,[&](uint64_t at,void* p,size_t n){Cancel(job);source.first->Read(source.second,0,at,p,n);});Cancel(job);completion.attributes[request.name]=std::move(bytes);
 }else if(request.kind==Kind::ExportAttribute){
  Check(Absolute(request.target.windowsPath)&&request.attributes.count(request.name),"Choose an attribute and a new output filename");auto& bytes=request.attributes.at(request.name);auto& path=request.target.windowsPath;HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw Failure(GetLastError(),"Cannot create attribute output; choose a new filename");Mark(completion,request.target);
  try{HfsPlus::Bytes buffer(65536);for(uint64_t at=0;at<bytes.size();){Cancel(job);auto n=size_t((std::min)(uint64_t(buffer.size()),bytes.size()-at));bytes.Read(at,buffer.data(),n);DWORD written=0;if(!WriteFile(file,buffer.data(),DWORD(n),&written,nullptr)||written!=n)throw Failure(GetLastError(),"Attribute export write failed");at+=n;}Cancel(job);if(!FlushFileBuffers(file))throw Failure(GetLastError(),"Attribute export flush failed");CloseHandle(file);}catch(...){CloseHandle(file);DeleteFileW(path.c_str());throw;}
 }else if(request.kind==Kind::CreateFolder){
  const auto& item=request.target;Check(!request.name.empty(),"Enter a folder name");
  if(item.volume){auto source=Source(item);auto e=source.first->Stat(source.second);Cancel(job);Check(e.directory&&source.first->Supports().create&&source.first->Supports().createFolders&&(e.id==2||!e.protectedFile),"Cannot create a folder here");MarkChild(completion,item,request.name);source.first->MakeDirectory(source.second,request.name);}
  else if(item.type==LIST_FAT){auto volume=OpenWindows(item.windowsPath);auto e=volume->Stat(2);if(!item.expectedIdentity.empty())Check(!FolderIdentity(e.identity).empty()&&FolderIdentity(e.identity)==FolderIdentity(item.expectedIdentity),"The destination folder changed");Cancel(job);MarkChild(completion,item,request.name);volume->MakeDirectory(2,request.name);volume->Close();}
  else{Check(item.type==LIST_HFS,"Invalid folder destination");if(!item.expectedResource.empty())Check(VolumeCoord::ImageKey(item.classicImage)==item.expectedResource,"The destination image was replaced");auto disk=WritableFloppy(item);CheckMedia(item);auto volume=OpenClassic(item.classicImage,item.classicPath,true);auto e=volume->Stat(2);Cancel(job);Check(e.directory,"Destination is not a folder");if(item.expectedCatalogId)Check(e.resolvedId==item.expectedCatalogId,"The destination HFS folder was replaced");MarkChild(completion,item,request.name);volume->MakeDirectory(2,request.name);volume->Close();Finish(disk);}
 }else{
  Check(request.sources.size()==1,"Select one item");const auto& item=request.sources.front();auto source=Source(item);completion.entry=source.first->Stat(source.second);completion.description=source.first->Description();completion.writable=source.first->Writable();completion.supportsAttributes=source.first->SupportsAttributes();completion.basicMetadata=source.first->SupportsBasicMetadata();completion.supportsCompression=source.first->SupportsCompression();
  if(completion.supportsAttributes)completion.attributes=source.first->GetAttributes(source.second);Cancel(job);
  if(request.kind==Kind::SetCompression){
   Check(completion.supportsCompression&&completion.writable,"This volume cannot change compression");auto& now=completion.entry;auto& old=request.originalMetadata;
   Check(now.id==old.id&&now.parent==old.parent&&now.name==old.name&&now.modified==old.modified&&now.created==old.created&&now.finder==old.finder&&now.compressed==old.compressed&&now.forks[0].size==old.forks[0].size&&now.forks[1].size==old.forks[1].size&&completion.attributes==request.originalAttributes,"The file changed while Properties was open; reopen Properties");
   Cancel(job);Mark(completion,item);p.phase=request.compressed?"Compressing file":"Decompressing file";p.path=Utf8(now.name);p.byteTransfer=true;p.scanning=false;p.totalBytes=now.forks[0].size+now.forks[1].size;
   source.first->SetCompressionMethod(source.second,request.compressed?request.compressionMethod:HfsPlus::Volume::CompressionMode::Uncompressed,[&](uint64_t done,uint64_t total){Cancel(job);p.processed=p.bytes=done;p.totalBytes=total;job.progress(p);});completion.entry=source.first->Stat(source.second);
  }
  if(request.kind==Kind::SetBasicMetadata){
   Check(completion.basicMetadata&&completion.writable,"This format has no writable basic metadata editor");auto& now=completion.entry;auto& old=request.originalMetadata;
   Check(now.id==old.id&&now.name==old.name&&now.version==old.version&&now.finder==old.finder&&now.created==old.created&&now.modified==old.modified&&now.protectedFile==old.protectedFile&&now.forks[0].size==old.forks[0].size&&now.forks[1].size==old.forks[1].size,"Properties changed while the editor was open; reopen Properties");Cancel(job);Mark(completion,item);source.first->SetBasicMetadata(source.second,request.metadata);completion.entry=request.metadata;
  }
  if(request.kind==Kind::SetAttributes){Check(completion.supportsAttributes&&completion.writable,"This item cannot store extended attributes");Check(completion.attributes==request.originalAttributes,"Attributes changed while Properties was open; reopen Properties before saving");Cancel(job);Mark(completion,item);source.first->SetAttributes(source.second,request.attributes);completion.attributes=request.attributes;}

  if(item.type==LIST_HFS&&!item.volume){source.first->Close();auto image=item.classicImage,path=item.classicPath;Check(HFSIFACE_get_properties(&image[0],&path[0],&completion.properties)==0,"Cannot read classic HFS properties");completion.classicProperties=true;completion.writable=physical_floppy_path(item.classicImage.c_str())>=0?true:!readonly_media_path(item.classicImage.c_str())&&(GetFileAttributesA(item.classicImage.c_str())&FILE_ATTRIBUTE_READONLY)==0;Cancel(job);}
  if(request.kind==Kind::SetProperties){
   Check(completion.classicProperties,"This format does not expose editable classic Finder properties");auto disk=WritableFloppy(item);auto image=item.classicImage,path=item.classicPath;
   hfsdirent current{};Check(HFSIFACE_get_properties(&image[0],&path[0],&current)==0,"Cannot revalidate classic HFS properties");Check(!item.expectedCatalogId||uint32_t(current.cnid)==item.expectedCatalogId,"The selected HFS entry was replaced");Cancel(job);
   auto changed=current;
   if(request.patch==MetadataPatch::Label){Check(request.label>=0&&request.label<=7,"Invalid Finder label");changed.fdflags=short((changed.fdflags&~14)|(request.label<<1));}
   else if(request.patch==MetadataPatch::Position){changed.fdlocation.h=request.x;changed.fdlocation.v=request.y;changed.fdflags&=~HFS_FNDR_HASBEENINITED;}
   else{Check(SameProperties(current,request.originalProperties),"Properties changed while the editor was open; reopen Properties before applying changes");changed=request.properties;
    Check(changed.cnid==current.cnid&&changed.parid==current.parid&&((changed.flags^current.flags)&HFS_ISDIR)==0,"Catalog identity and item kind cannot be changed");
    if(!(current.flags&HFS_ISDIR))Check(changed.u.file.dsize==current.u.file.dsize&&changed.u.file.rsize==current.u.file.rsize,"Fork lengths cannot be changed through Properties");
   }
   auto to=path;char name[32]{};strcpy_s(name,changed.name);if(request.patch==MetadataPatch::All){mac_to_pc_charset((unsigned char*)name);Check(HfvValidRenameLeaf(name,true),"Invalid classic HFS name");to=path.substr(0,path.find_last_of(':')+1)+name;}
   std::vector<char> macPath(path.begin(),path.end()),macTo(to.begin(),to.end());macPath.push_back(0);macTo.push_back(0);pc_to_mac_charset((unsigned char*)macPath.data());pc_to_mac_charset((unsigned char*)macTo.data());
   hfsvol* volume=hfs_mount(&image[0],0,O_RDWR);Check(volume!=nullptr,"Cannot open classic HFS for editing");
   try{Cancel(job);Mark(completion,item);int code=hfs_editattrs(volume,macPath.data(),macTo.data(),&changed);auto error=code?ClassicError("Cannot update properties; rollback was attempted"):std::string();int closed=hfs_umount(volume);volume=nullptr;if(closed&&error.empty())error=ClassicError("Cannot close the updated HFS volume");Check(code==0&&closed==0,error.c_str());Finish(disk);
   }catch(...){if(volume)hfs_umount(volume);throw;}

  }
 }
 completion.complete=true;completion.completed=1;return {};
}
std::string Resource(const Location& item){auto path=HostPath(item);if(!path.empty())return VolumeCoord::PathKey(path);if(item.volume)return item.volume->ResourceKey();return VolumeCoord::ImageKey(item.classicImage);}
void AddPath(std::vector<VolumeCoord::Access>& access,const std::wstring& path,VolumeCoord::Mode mode,bool companion=true){access.push_back({VolumeCoord::PathKey(path),mode});if(companion)access.push_back({VolumeCoord::PathKey(CompanionPath(path)),mode});}
std::u16string SourceName(const Location& item){if(!item.expectedName.empty())return item.expectedName;auto path=HostPath(item);if(!path.empty())return U16(path.substr(path.find_last_of(L"\\/")+1));if(!item.classicPath.empty())return Ansi(item.classicPath.substr(item.classicPath.find_last_of(':')+1));return {};}
std::vector<VolumeCoord::Access> Resources(const Request& request){
 using VolumeCoord::Mode;std::vector<VolumeCoord::Access> access;
 if(request.kind==Kind::LegacyCopy){
  if(Absolute(request.sourcePath))AddPath(access,VolumeCoord::Wide(request.sourcePath),request.move?Mode::Write:Mode::Read);else access.push_back({VolumeCoord::ImageKey(request.sourceVolume),(request.move||request.background||CopyProcess::UsesWorker(request))?Mode::Write:Mode::Read});
  if(Absolute(request.targetPath))access.push_back({VolumeCoord::LegacyOutputKey(VolumeCoord::Wide(request.targetPath)),Mode::Write});else access.push_back({VolumeCoord::ImageKey(request.targetVolume),Mode::Write});return access;
 }
 for(const auto& item:request.sources){auto mode=request.kind==Kind::CreateHardLink||request.kind==Kind::Rename||request.kind==Kind::Delete||request.kind==Kind::SetProperties||request.kind==Kind::SetAttributes||request.kind==Kind::SetBasicMetadata||request.kind==Kind::SetCompression||item.cut||((request.background||CopyProcess::UsesWorker(request))&&item.type!=LIST_FAT)?Mode::Write:Mode::Read;auto path=HostPath(item);if(path.empty())access.push_back({Resource(item),mode});else{
   // Explicit Windows adapters are still file readers when a copy is not a move.
   if(request.kind!=Kind::Rename&&request.kind!=Kind::Delete&&request.kind!=Kind::SetProperties&&request.kind!=Kind::SetAttributes&&request.kind!=Kind::SetBasicMetadata&&request.kind!=Kind::SetCompression&&!item.cut)mode=Mode::Read;AddPath(access,path,mode);
   if(request.kind==Kind::Rename){auto parent=path.substr(0,path.find_last_of(L"\\/"));AddPath(access,ChildPath(parent,request.name),Mode::Write);}
  }}
 if(request.kind==Kind::CreateHardLink||request.kind==Kind::Copy||request.kind==Kind::ReplaceResource||request.kind==Kind::ExportFork||request.kind==Kind::ExportAttribute||request.kind==Kind::CreateFolder){auto path=HostPath(request.target);if(path.empty())access.push_back({Resource(request.target),Mode::Write});else if(request.kind==Kind::CreateFolder)AddPath(access,ChildPath(path,request.name),Mode::Write,false);else if(request.kind==Kind::CreateHardLink)AddPath(access,ChildPath(path,request.name),Mode::Write);else if(request.kind==Kind::Copy){bool known=true;for(const auto& source:request.sources){auto name=SourceName(source);if(name.empty()){known=false;break;}AddPath(access,ChildPath(path,name),Mode::Write);}if(!known)AddPath(access,path,Mode::Write,false);}else AddPath(access,path,Mode::Write,request.kind!=Kind::ExportFork&&request.kind!=Kind::ExportAttribute);
 }return access;
}

Storage::Result Execute(const Request& request,Storage::Job& job,Completion& completion){
 HfsPlus::AttributeReadScope attributeReads([&]{Cancel(job);});Cancel(job);
 Cancel(job);
 if(request.kind==Kind::CreateHardLink){Check(request.sources.size()==1,"Select one source for a hard link");auto source=Source(request.sources.front());auto target=Destination(request.target);Check(source.first->SupportsHardLinks()&&target.first->SupportsHardLinks()&&source.first->ResourceKey()==target.first->ResourceKey(),"Hard links require the same writable HFS+/HFSX volume");Cancel(job);Mark(completion,request.sources.front());MarkChild(completion,request.target,request.name);Storage::Progress progress;progress.scanning=false;progress.phase="Creating hard link";progress.totalFiles=1;job.progress(progress);target.first->MakeHardLink(source.second,target.second,request.name);progress.files=1;job.progress(progress);completion.complete=true;completion.completed=1;return {};}
 if(request.kind==Kind::CreateFolder||request.kind==Kind::Inspect||request.kind==Kind::SetProperties||request.kind==Kind::SetAttributes||request.kind==Kind::ReadAttributeFile||request.kind==Kind::ExportAttribute||request.kind==Kind::SetBasicMetadata||request.kind==Kind::SetCompression)return Metadata(request,job,completion);
 if(request.kind==Kind::Copy)return CopyRequest(request,job,completion);
 if(request.kind==Kind::ExportFork)return ExportFork(request,job,completion);
 if(request.kind==Kind::ReplaceResource)return ReplaceResource(request,job,completion);
 if(request.kind!=Kind::LegacyCopy)return Mutate(request,job,completion);
 // Legacy containers cannot carry arbitrary attributes/ACLs. Preflight the
 // complete selection before converters can create partial output.
 auto noLostMetadata=[&](MacBrowser::Volume& volume,uint32_t root){
  if(!volume.SupportsAttributes())return;
  std::set<uint32_t> ancestors;
  std::function<void(uint32_t,unsigned)> walk=[&](uint32_t id,unsigned depth){
   Cancel(job);Check(depth<=128&&ancestors.insert(id).second,"Recursive conversion source");
   Check(volume.GetAttributes(id).empty(),"Legacy conversion cannot preserve extended attributes or Mac ACLs; use an HFS+/HFSX or Windows both-fork copy");
   auto entry=volume.Stat(id);Check(request.target.type!=LIST_HFS||!entry.datesValid||(entry.created<=UINT32_MAX&&entry.modified<=UINT32_MAX),"Classic HFS conversion cannot preserve dates after February 2040; source retained");if(entry.directory)for(auto& child:volume.List(id))walk(child.id,depth+1);
   ancestors.erase(id);
  };
  walk(root,0);
 };
 if(request.sources.empty()&&Absolute(request.sourcePath)){
  auto source=MacBrowser::ResolveWindowsFile(VolumeCoord::Wide(request.sourcePath));
  noLostMetadata(*source.first,source.second);source.first->Close();
 }
 if(!request.sources.empty()){
  // Captured paths/identities never depend on another window's volume indexes.
  auto source=Source(request.sources.front());noLostMetadata(*source.first,source.second);
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
Storage::Result ExecuteCopyWorker(const Request& request,Storage::Job& job,Completion& completion){
 try{LegacyEngine::Scope engine;VolumeCoord::Lease access(Resources(request));VolumeCoord::OperationLease current(Resources(request));return Execute(request,job,completion);}
 catch(const Storage::Cancelled&){return {1,ERROR_CANCELLED,"Cancelled; completed changes were retained"};}
 catch(const Failure& e){return {1,e.error,e.what()};}
 catch(const std::exception& e){return {1,ERROR_UNHANDLED_EXCEPTION,e.what()};}
 catch(...){return {1,ERROR_UNHANDLED_EXCEPTION,"Unexpected copy-worker failure"};}
}
Operation Submit(Request request){
 auto state=std::make_shared<Operation::State>();auto values=std::make_shared<const Request>(std::move(request));
 auto& queue=values->background?Storage::copyWorker():Storage::worker();
 auto access=std::make_shared<std::unique_ptr<VolumeCoord::Lease>>();
 struct Plan {std::atomic<bool> ready{false};std::vector<VolumeCoord::Access> resources;Storage::Result error;};auto plan=std::make_shared<Plan>();
 auto planning=queue.submit([plan,values](Storage::Job& job){try{if(job.cancelled())throw Storage::Cancelled();plan->resources=Resources(*values);}catch(const Storage::Cancelled&){plan->error={1,ERROR_CANCELLED,"Cancelled while identifying Windows paths"};}catch(const std::exception& error){plan->error={1,ERROR_UNHANDLED_EXCEPTION,error.what()};}catch(...){plan->error={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected path planning failure"};}plan->ready.store(true,std::memory_order_release);return Storage::Result{};});
 auto job=queue.submit([state,values,access,plan](Storage::Job& job){Completion completion;Storage::Result result;
  try{Cancel(job);if(plan->error.code){result=plan->error;}else{if(!*access)throw VolumeCoord::Busy();LegacyEngine::Scope engine;
   VolumeCoord::OperationLease currentPaths(Resources(*values));if(CopyProcess::UsesWorker(*values)){std::vector<std::string> keys,writes;for(auto& item:plan->resources){keys.push_back(item.key);if(item.mode==VolumeCoord::Mode::Write)writes.push_back(item.key);}Mark(completion,values->target);for(auto& item:values->sources)if(item.cut)Mark(completion,item);if(values->kind==Kind::LegacyCopy){Location target;target.type=Absolute(values->targetPath)?LIST_FAT:LIST_HFS;target.windowsPath=VolumeCoord::Wide(values->targetPath);target.classicImage=values->targetVolume;Mark(completion,target);if(values->move){Location source;source.type=Absolute(values->sourcePath)?LIST_FAT:LIST_HFS;source.windowsPath=VolumeCoord::Wide(values->sourcePath);source.classicImage=values->sourceVolume;Mark(completion,source);}}result=CopyProcess::Run(*values,job,completion,keys,writes);}else result=Execute(*values,job,completion);}
  }
  catch(const VolumeCoord::Busy& e){result={1,ERROR_BUSY,e.what()};}catch(const Storage::Cancelled&){result={1,ERROR_CANCELLED,"Cancelled; completed changes were retained"};}
  catch(const Failure& e){result={1,e.error,e.what()};}catch(const std::exception& e){result={1,ERROR_UNHANDLED_EXCEPTION,e.what()};}
  catch(...){result={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected file-operation failure"};}
  std::vector<std::string> changed;
  for(auto& item:completion.affected){try{if(auto volume=item.volume.lock())changed.push_back(volume->ResourceKey());else if(item.type==LIST_FAT)changed.push_back(VolumeCoord::PathKey(item.windowsPath));else if(!item.classicImage.empty())changed.push_back(VolumeCoord::ImageKey(item.classicImage));}catch(...){/* Keep the operation result if a removed path cannot be identified. */}}
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
