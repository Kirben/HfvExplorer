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
  Storage::Progress p;p.utf8=output_.utf8;p.phase="Finding files";p.path=path;p.detail=std::to_string(output_.checked)+" entries checked; "+std::to_string(output_.hits.size())+" matches";job_.progress(p);
 }
 bool hit(const std::string& name,const std::string& parent,const std::string& full,bool folder,unsigned long long bytes,uint32_t parentId=0,uint32_t id=0){
  ++output_.checked;progress(parent);
  if((!folder||request_.includeFolders)&&FindNameMatches(name,request_.pattern,request_.caseSensitive)){
   if(output_.hits.size()>=request_.limit){output_.truncated=true;return false;}
   FindHit result{name,parent,full,bytes,folder};
   const auto origin=request_.location.origin.IsClassic()?request_.location.origin:
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
  auto start=request_.location.directory;
  if(start.find(':')==std::string::npos)start+=':';
  const auto origin=request_.location.origin.IsClassic()?request_.location.origin:
      FileOperations::Location::Classic(request_.location.volume,start);
  StorageLocation::ValidateImage(VolumeCoord::Wide(origin.ClassicImage()),origin.expectedResource);
  auto volume=MacBrowser::OpenClassic(origin.ClassicImage(),start,false);
  StorageLocation::ValidateMedia(origin);
  std::vector<Folder> pending={{start,0,2}};
  std::set<uint32_t> visited;
  while(!pending.empty()){
   Folder folder=std::move(pending.back());pending.pop_back();progress(folder.path,true);
   const auto parent=volume->Stat(uint32_t(folder.id)).resolvedId;
   if(!visited.insert(parent).second)throw std::runtime_error("Search stopped because the Macintosh catalog repeats a folder ID");
   for(const auto& entry:volume->List(uint32_t(folder.id))){
    hfsdirent properties{};
    if(!volume->GetClassicProperties(entry.id,properties))
     throw HfsPlus::Error("Could not read classic HFS catalog properties");
    const auto name=display(properties.name);
    const auto full=folder.path+(folder.path.back()==':'?"":":")+name;
    if(!hit(name,folder.path,full,entry.directory,entry.forks[0].size+entry.forks[1].size,parent,entry.resolvedId)){
     StorageLocation::ValidateMedia(origin,StorageLocation::FloppyProbe::FreshHandle);
     return;
    }
    if(entry.directory&&request_.recursive)queued(pending,{full,folder.depth+1,long(entry.id)});
   }
  }
  StorageLocation::ValidateMedia(origin,StorageLocation::FloppyProbe::FreshHandle);
  volume->Close();
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
  output_.utf8=true;
  const auto root=request_.location.origin.IsWindows()?request_.location.origin.WindowsPath():
      VolumeCoord::Wide(request_.location.directory);
  auto media=FileOperations::Location::Windows(root);
  if(request_.location.checkWindowsMedia){
   windowsMediaSerial_=StorageLocation::WindowsSerial(root);
   if(!windowsMediaSerial_)throw Storage::MediaChanged("Cannot identify the Windows search media.");
   media.BindWindowsMediaSerial(windowsMediaSerial_);
  }
  const auto attributes=GetFileAttributesW(BrowserPresentation::WindowsPath(root).c_str());
  if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("The selected search location is not an accessible Windows folder");
  const auto utf8=[](const std::wstring& text){return BrowserPresentation::Utf8(MacBrowser::U16(text));};
  const auto pattern=BrowserPresentation::FromUtf8(request_.pattern);
  struct WindowsFolder {std::wstring path;unsigned depth;};
  std::vector<WindowsFolder> pending={{root,0}};
  while(!pending.empty()){
   auto folder=std::move(pending.back());pending.pop_back();
   const auto parent=utf8(folder.path);
   progress(parent,true);
   auto prefix=folder.path;
   if(prefix.back()!=L'\\'&&prefix.back()!=L'/')prefix+=L'\\';
   const auto mask=BrowserPresentation::WindowsPath(prefix+L"*");
   WIN32_FIND_DATAW entry={};HANDLE handle=FindFirstFileW(mask.c_str(),&entry);
   if(handle==INVALID_HANDLE_VALUE){
    DWORD error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)continue;
    if(folder.depth==0)throw std::runtime_error("Could not enumerate the selected Windows folder (Windows error "+std::to_string(error)+")");
    ++output_.unreadable;continue;
   }
   struct Close{HANDLE value;~Close(){FindClose(value);}} close={handle};
   do {
    const std::wstring name=entry.cFileName;
    if(name==L"."||name==L"..")continue;
    ++output_.checked;progress(parent);
    const bool isDirectory=(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    const auto full=prefix+name;
    if((!isDirectory||request_.includeFolders)&&FindNameMatches(name,pattern,request_.caseSensitive)){
     if(output_.hits.size()>=request_.limit){output_.truncated=true;StorageLocation::ValidateMedia(media);return;}
     const auto size=(uint64_t(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow;
     FindHit hit{utf8(name),parent,utf8(full),isDirectory?0:size,isDirectory};
     hit.destination=FileOperations::Location::Windows(folder.path);
     hit.destination.BindWindowsMediaSerial(windowsMediaSerial_);
     hit.destination.directory=true;
     hit.reveal=FileOperations::Location::Windows(full);
     hit.reveal.BindWindowsMediaSerial(windowsMediaSerial_);
     hit.reveal.directory=isDirectory;
     try{hit.reveal.expectedIdentity=MacBrowser::DescribeWindowsFile(full).identity;}
     catch(const std::exception&){++output_.unreadable;}
     if(!hit.reveal.expectedIdentity.empty())output_.hits.push_back(std::move(hit));
    }
    if(isDirectory&&request_.recursive&&!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
     if(folder.depth>=128||pending.size()>=100000)throw std::runtime_error("Search folder limit reached");
     pending.push_back({full,folder.depth+1});
    }
   }while(FindNextFileW(handle,&entry));
   if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Windows folder enumeration failed (Windows error "+std::to_string(GetLastError())+")");
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
  if(!results||request.pattern.empty()||request.pattern.size()>(request.location.UsesUnicode()?1020:255)||!request.limit)return {1,ERROR_INVALID_PARAMETER,"Enter a name or wildcard pattern of 1 to 255 characters."};
  if(job.cancelled())throw Storage::Cancelled();
  *results={};Search search(request,*results,job);
  try{if(request.location.native)search.native();else if(request.location.mac)search.mac();else search.windows();}
  catch(const Storage::Cancelled&){throw;}
  catch(const Storage::MediaChanged& error){return {1,ERROR_MEDIA_CHANGED,error.what()};}
  catch(const std::exception& error){return {1,ERROR_READ_FAULT,error.what()};}
  std::sort(results->hits.begin(),results->hits.end(),[&](const FindHit& a,const FindHit& b){int compared=results->utf8?_wcsicmp(BrowserPresentation::FromUtf8(a.name).c_str(),BrowserPresentation::FromUtf8(b.name).c_str()):_stricmp(a.name.c_str(),b.name.c_str());return compared?compared<0:a.path<b.path;});
  search.progress(request.location.directory,true);return {};
 };
}
