// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "FindFiles.h"
#include "StorageLocation.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include "BrowserPresentation.h"
#include "charset.h"
#include <fcntl.h>
#include <set>
#include <algorithm>
#include <cerrno>
extern "C" {
#include "hfs/libhfs/hfs.h"
}
namespace {
std::vector<char> pathBuffer(const std::string& path){if(path.empty()||path.size()>=MAX_PATH)throw std::runtime_error("A search path is empty or exceeds the supported length");return std::vector<char>(path.c_str(),path.c_str()+path.size()+1);}
std::string display(std::string text){auto b=pathBuffer(text);mac_to_pc_charset(reinterpret_cast<unsigned char*>(b.data()));return b.data();}
struct Folder {std::string path;unsigned depth=0;long id=0;};
class Search {
 const FindRequest& request_;FindResults& output_;Storage::Job& job_;DWORD last_=0;
 std::optional<DWORD> windowsMediaSerial_;
public:
 Search(const FindRequest& request,FindResults& output,Storage::Job& job):request_(request),output_(output),job_(job){}
 void progress(const std::string& path,bool force=false){
  if(job_.cancelled())throw Storage::Cancelled();
  DWORD now=GetTickCount();if(!force&&now-last_<100)return;last_=now;
  Storage::Progress p;p.utf8=bool(request_.location.native);p.phase="Finding files";p.path=path;p.detail=std::to_string(output_.checked)+" entries checked; "+std::to_string(output_.hits.size())+" matches";job_.progress(p);
 }
 bool hit(const std::string& name,const std::string& parent,const std::string& full,bool folder,unsigned long long bytes,uint32_t parentId=0,uint32_t id=0){
  ++output_.checked;progress(parent);
  if((!folder||request_.includeFolders)&&FindNameMatches(name,request_.pattern,request_.caseSensitive)){
   if(output_.hits.size()>=request_.limit){output_.truncated=true;return false;}
   FindHit result{name,parent,full,bytes,folder};
   if(request_.location.mac){
    auto origin=request_.location.origin.IsClassic()?request_.location.origin:
        FileOperations::Location::Classic(request_.location.volume,parent);
    result.destination=origin;
    result.destination.SetClassic(origin.ClassicImage(),parent);
    result.reveal=origin;
    result.reveal.SetClassic(origin.ClassicImage(),full);
    result.destination.expectedCatalogId=parentId;
    result.reveal.expectedCatalogId=id;
    if(origin.MediaGenerationKnown()){
     result.destination.BindMediaGeneration(origin.MediaGeneration());
     result.reveal.BindMediaGeneration(origin.MediaGeneration());
    }
   }else{
    result.destination=FileOperations::Location::Windows(VolumeCoord::Wide(parent));
    result.reveal=FileOperations::Location::Windows(VolumeCoord::Wide(full));
    result.destination.BindWindowsMediaSerial(windowsMediaSerial_);
    result.reveal.BindWindowsMediaSerial(windowsMediaSerial_);
    try{result.reveal.expectedIdentity=MacBrowser::DescribeWindowsFile(result.reveal.WindowsPath()).identity;}
    catch(const std::exception&){++output_.unreadable;return true;}
   }
   result.destination.directory=true;
   result.reveal.directory=folder;
   output_.hits.push_back(std::move(result));
  }return true;
 }
 void queued(std::vector<Folder>& pending,Folder folder){
  if(folder.depth>128||folder.path.size()>=MAX_PATH||pending.size()>=100000)throw std::runtime_error("Search stopped at the supported folder depth, path length or pending-folder limit");
  pending.push_back(std::move(folder));
 }
 void mac(){
  auto volumePath=pathBuffer(request_.location.volume);hfsvol* volume=hfs_mount(volumePath.data(),1,O_RDONLY);
  if(!volume)throw std::runtime_error(hfs_error?hfs_error:"Could not open the Macintosh volume");
  struct Close{hfsvol* value;~Close(){hfs_umount(value);}} close={volume};
  auto root=pathBuffer(request_.location.directory);pc_to_mac_charset(reinterpret_cast<unsigned char*>(root.data()));
  std::string start=root.data();if(start.find(':')==std::string::npos)start+=':';
  auto startPath=pathBuffer(start.substr(start.find(':')));hfsdirent entry={};
  if(hfs_stat(volume,startPath.data(),&entry)<0||!(entry.flags&HFS_ISDIR))throw std::runtime_error("The selected search location is not an accessible Macintosh folder");
  std::vector<Folder> pending={{start,0,entry.cnid}};std::set<long> visited;
  while(!pending.empty()){
   Folder folder=std::move(pending.back());pending.pop_back();progress(display(folder.path),true);
   if(!visited.insert(folder.id).second)throw std::runtime_error("Search stopped because the Macintosh catalog repeats a folder ID");
   auto path=pathBuffer(folder.path.substr(folder.path.find(':')));hfsdir* directory=hfs_opendir(volume,path.data());
   if(!directory)throw std::runtime_error(hfs_error?hfs_error:"Could not read a Macintosh folder");
   struct CloseDir{hfsdir* value;~CloseDir(){hfs_closedir(value);}} closeDir={directory};
   for(;;){
    errno=0;if(hfs_readdir(directory,&entry)<0){if(errno==ENOENT)break;throw std::runtime_error(hfs_error?hfs_error:"Could not read the Macintosh catalog");}
    std::string full=folder.path+(folder.path.back()==':'?"":":")+entry.name;bool isDirectory=(entry.flags&HFS_ISDIR)!=0;
    if(!hit(display(entry.name),display(folder.path),display(full),isDirectory,isDirectory?0:static_cast<unsigned long long>(entry.u.file.dsize)+entry.u.file.rsize,uint32_t(folder.id),uint32_t(entry.cnid)))return;
    if(isDirectory&&request_.recursive)queued(pending,{full,folder.depth+1,entry.cnid});
   }
  }
 }
 void native(){
  auto volume=request_.location.native;output_.utf8=true;
  StorageLocation::ValidateMedia(FileOperations::Location::Mounted(volume,request_.location.nativeFolder));
  struct NativeFolder {uint32_t id;std::string path;std::vector<uint32_t> route;std::set<uint32_t> ancestors;};
  auto entry=volume->Stat(request_.location.nativeFolder);
  auto route=request_.location.nativePath;if(route.empty())route.push_back(request_.location.nativeFolder);
  std::vector<NativeFolder> pending={{entry.id,request_.location.directory,route,{entry.resolvedId?entry.resolvedId:entry.id}}};
  while(!pending.empty()){
   auto folder=std::move(pending.back());pending.pop_back();progress(folder.path,true);
   for(auto& item:volume->List(folder.id)){
    if(job_.cancelled())throw Storage::Cancelled();++output_.checked;
    auto name=BrowserPresentation::Utf8(item.name),path=folder.path+":"+name;auto nodes=folder.route;nodes.push_back(item.id);
    if((!item.directory||request_.includeFolders)&&FindNameMatches(MacBrowser::Wide(item.name),BrowserPresentation::FromUtf8(request_.pattern),request_.caseSensitive)){
     if(output_.hits.size()>=request_.limit){output_.truncated=true;return;}
     FindHit result{name,folder.path,path,item.directory?0:item.forks[0].size+item.forks[1].size,item.directory,nodes};
     result.destination=FileOperations::Location::Mounted(volume,item.directory?item.id:folder.id);
     result.destination.directory=true;
     auto& selection=item.directory?result.destination:result.reveal;
     if(!item.directory)selection=FileOperations::Location::Mounted(volume,item.id);
     selection.expectedName=item.name;
     selection.expectedParent=item.parent;
     output_.hits.push_back(std::move(result));
    }
    if(item.directory&&request_.recursive){auto resolved=item.resolvedId?item.resolvedId:item.id;
     if(!item.readError.empty()||folder.ancestors.count(resolved)){++output_.unreadable;continue;}
     if(nodes.size()>128||pending.size()>=100000)throw std::runtime_error("Search folder limit reached");
     auto ancestors=folder.ancestors;ancestors.insert(resolved);pending.push_back({item.id,path,nodes,std::move(ancestors)});
    }
   }
  }
 }
 void windows(){
  const std::string root=request_.location.directory;
  auto media=FileOperations::Location::Windows(VolumeCoord::Wide(root));
  if(request_.location.checkWindowsMedia){
   windowsMediaSerial_=StorageLocation::WindowsSerial(media.WindowsPath());
   if(!windowsMediaSerial_)throw Storage::MediaChanged("Cannot identify the Windows search media.");
   media.BindWindowsMediaSerial(windowsMediaSerial_);
  }
  DWORD attributes=GetFileAttributesA(root.c_str());
  if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("The selected search location is not an accessible Windows folder");
  std::vector<Folder> pending={{root,0,0}};
  while(!pending.empty()){
   Folder folder=std::move(pending.back());pending.pop_back();progress(folder.path,true);
   std::string prefix=folder.path+(folder.path.back()=='\\'?"":"\\"),mask=prefix+"*";
   if(mask.size()>=MAX_PATH)throw std::runtime_error("A Windows search path exceeds the supported length");
   WIN32_FIND_DATAA entry={};HANDLE handle=FindFirstFileA(mask.c_str(),&entry);
   if(handle==INVALID_HANDLE_VALUE){
    DWORD error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)continue;
    if(folder.depth==0)throw std::runtime_error("Could not enumerate the selected Windows folder (Windows error "+std::to_string(error)+")");
    ++output_.unreadable;continue;
   }
   struct Close{HANDLE value;~Close(){FindClose(value);}} close={handle};
   for(;;){
    if(strcmp(entry.cFileName,".")&&strcmp(entry.cFileName,"..")){
     bool isDirectory=(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;std::string full=prefix+entry.cFileName;
     if(full.size()>=MAX_PATH)throw std::runtime_error("Search stopped because a Windows path exceeds the supported length");
     unsigned long long size=(static_cast<unsigned long long>(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow;
     if(!hit(entry.cFileName,folder.path,full,isDirectory,isDirectory?0:size)){
      StorageLocation::ValidateMedia(media);
      return;
     }
     if(isDirectory&&request_.recursive&&!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))queued(pending,{full,folder.depth+1,0});
    }
    if(!FindNextFileA(handle,&entry)){DWORD error=GetLastError();if(error!=ERROR_NO_MORE_FILES)throw std::runtime_error("Windows folder enumeration failed (Windows error "+std::to_string(error)+")");break;}
   }
  }
  StorageLocation::ValidateMedia(media);
 }
};
}
bool FindNameMatches(std::wstring name,std::wstring pattern,bool sensitive){
 if(pattern.empty())return false;
 if(!sensitive){if(!name.empty())CharUpperBuffW(&name[0],DWORD(name.size()));CharUpperBuffW(&pattern[0],DWORD(pattern.size()));}
 if(pattern==L"*.*")pattern=L"*";
 if(pattern.find_first_of(L"*?")==std::wstring::npos)return name.find(pattern)!=std::wstring::npos;
 size_t n=0,p=0,star=std::wstring::npos,retry=0;
 while(n<name.size()){
  if(p<pattern.size()&&(pattern[p]==L'?'||pattern[p]==name[n])){++n;++p;}
  else if(p<pattern.size()&&pattern[p]==L'*'){star=p++;retry=n;}
  else if(star!=std::wstring::npos){p=star+1;n=++retry;}else return false;
 }
 while(p<pattern.size()&&pattern[p]==L'*')++p;return p==pattern.size();
}
bool FindNameMatches(std::string name,std::string pattern,bool caseSensitive){
 if(pattern.empty())return false;
 if(!caseSensitive){if(!name.empty())CharUpperBuffA(&name[0],DWORD(name.size()));CharUpperBuffA(&pattern[0],DWORD(pattern.size()));}
 if(pattern=="*.*")pattern="*";
 if(pattern.find_first_of("*?")==std::string::npos)return name.find(pattern)!=std::string::npos;
 size_t n=0,p=0,star=std::string::npos,retry=0;
 while(n<name.size()){
  if(p<pattern.size()&&(pattern[p]=='?'||pattern[p]==name[n])){++n;++p;}
  else if(p<pattern.size()&&pattern[p]=='*'){star=p++;retry=n;}
  else if(star!=std::string::npos){p=star+1;n=++retry;}
  else return false;
 }
 while(p<pattern.size()&&pattern[p]=='*')++p;return p==pattern.size();
}
std::function<Storage::Result(Storage::Job&)> MakeFindFilesJob(FindRequest request,std::shared_ptr<FindResults> results){
 return [=](Storage::Job& job)->Storage::Result {
  if(!results||request.pattern.empty()||request.pattern.size()>(request.location.native?1020:255)||!request.limit)return {1,ERROR_INVALID_PARAMETER,"Enter a name or wildcard pattern of 1 to 255 characters."};
  if(job.cancelled())throw Storage::Cancelled();
  *results={};Search search(request,*results,job);
  try{if(request.location.native)search.native();else if(request.location.mac)search.mac();else search.windows();}
  catch(const Storage::Cancelled&){throw;}
  catch(const Storage::MediaChanged& error){return {1,ERROR_MEDIA_CHANGED,error.what()};}
  catch(const std::exception& error){return {1,ERROR_READ_FAULT,error.what()};}
  std::sort(results->hits.begin(),results->hits.end(),[](const FindHit& a,const FindHit& b){int compared=_stricmp(a.name.c_str(),b.name.c_str());return compared?compared<0:a.path<b.path;});
  search.progress(request.location.directory,true);return {};
 };
}
