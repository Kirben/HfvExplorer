// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "FindFiles.h"
#include "StorageLocation.h"
#include "MacBrowserModel.h"
#include "VolumeCoordinator.h"
#include "BrowserPresentation.h"
#include "WindowsFolderEntries.h"
#include "BrowserDirectory.h"
#include "charset.h"
#include <fcntl.h>
#include <set>
#include <algorithm>
#include <cerrno>
extern "C" {
#include "hfs/ClassicHfs.h"
}
namespace {
std::vector<char> pathBuffer(const std::string& path){if(path.empty()||path.size()>=MAX_PATH)throw std::runtime_error("A search path is empty or exceeds the supported length");return std::vector<char>(path.c_str(),path.c_str()+path.size()+1);}
std::string display(std::string text){auto b=pathBuffer(text);mac_to_pc_charset(reinterpret_cast<unsigned char*>(b.data()));return b.data();}
using Location=FileOperations::Location;

std::string Utf8(const std::wstring& text){return BrowserPresentation::Utf8(MacBrowser::U16(text));}

struct Folder {
 std::string path;
 Location location;
 uint32_t cursor=2;
 unsigned depth=0;
 std::vector<uint32_t> route;
 std::set<uint32_t> ancestors;
};

struct Child {
 std::string name;
 Folder folder;
 uint64_t bytes=0;
 enum class Recursion { Allowed, Skip, Unreadable } recursion=Recursion::Allowed;
};

class Search {
 const FindRequest& request_;
 FindResults& output_;
 Storage::Job& job_;
 DWORD last_=0;
 std::wstring widePattern_;
public:
 Search(const FindRequest& request,FindResults& output,Storage::Job& job)
 :request_(request),output_(output),job_(job),
  widePattern_(request.UsesUnicode()?BrowserPresentation::FromUtf8(request.pattern):std::wstring())
 {
  output_.utf8=request.UsesUnicode();
 }

 void progress(const std::string& path,bool force=false){
  if(job_.cancelled())throw Storage::Cancelled();
  DWORD now=GetTickCount();if(!force&&now-last_<100)return;last_=now;
  Storage::Progress p;p.utf8=output_.utf8;p.phase="Finding files";p.path=path;p.detail=std::to_string(output_.checked)+" entries checked; "+std::to_string(output_.hits.size())+" matches";job_.progress(p);
 }

private:
 bool match(const std::string& name)const{
  return output_.utf8?FindNameMatches(BrowserPresentation::FromUtf8(name),widePattern_,request_.caseSensitive):
      FindNameMatches(name,request_.pattern,request_.caseSensitive);
 }

 bool addHit(const Folder& parent,Child& child){
  const bool directory=child.folder.location.directory;
  if((directory&&!request_.includeFolders)||!match(child.name))return true;
  if(output_.hits.size()>=request_.limit){output_.truncated=true;return false;}

  auto reveal=child.folder.location;
  if(reveal.IsWindows()){
   try{reveal.expectedIdentity=MacBrowser::DescribeWindowsFile(reveal.WindowsPath()).identity;}
   catch(const std::exception&){++output_.unreadable;return true;}
   if(reveal.expectedIdentity.empty()){++output_.unreadable;return true;}
  }
  FindHit hit{child.name,parent.path,child.folder.path,child.bytes,directory,child.folder.route};
  if(directory&&reveal.IsMounted())hit.destination=std::move(reveal);
  else{hit.destination=parent.location;hit.reveal=std::move(reveal);}
  hit.mount=request_.scope.mount;
  output_.hits.push_back(std::move(hit));
  return true;
 }

 template<class Read>
 void walk(Folder root,Read read){
  std::vector<Folder> pending;
  pending.push_back(std::move(root));
  while(!pending.empty()){
   auto folder=std::move(pending.back());pending.pop_back();
   progress(folder.path,true);
   if(!read(folder,[&](Child child){
    progress(folder.path);
    ++output_.checked;
    if(!addHit(folder,child))return false;
    if(!child.folder.location.directory||!request_.recursive)return true;
    if(child.recursion==Child::Recursion::Unreadable)++output_.unreadable;
    if(child.recursion!=Child::Recursion::Allowed)return true;
    if(folder.depth>=128||pending.size()>=100000||child.folder.route.size()>128||
       (child.folder.location.IsClassic()&&child.folder.path.size()>=MAX_PATH))
     throw std::runtime_error("Search folder limit reached");
    child.folder.depth=folder.depth+1;
    pending.push_back(std::move(child.folder));
    return true;
   }))return;
  }
 }

public:
 void mac(){
  auto origin=request_.Origin();
  auto start=origin.ClassicPath();
  if(start.find(':')==std::string::npos)start+=':';
  StorageLocation::ValidateImage(VolumeCoord::Wide(origin.ClassicImage()),origin.expectedResource);
  auto volume=MacBrowser::OpenClassic(origin.ClassicImage(),start,false,origin.expectedResource);
  StorageLocation::ValidateMedia(origin);
  const auto root=volume->Stat(2);
  if(origin.expectedCatalogId&&root.resolvedId!=origin.expectedCatalogId)
   throw Storage::MediaChanged("The selected classic HFS folder was replaced.");
  origin.directory=true;
  std::set<uint32_t> visited;
  walk({start,origin},[&](Folder& folder,auto emit){
   auto parent=volume->Stat(folder.cursor).resolvedId;
   if(!visited.insert(parent).second)throw std::runtime_error("Search stopped because the Macintosh catalog repeats a folder ID");
   folder.location.expectedCatalogId=parent;
   for(const auto& entry:volume->List(folder.cursor)){
    if(job_.cancelled())throw Storage::Cancelled();
    hfsdirent properties{};
    if(!volume->GetClassicProperties(entry.id,properties))
     throw HfsPlus::Error("Could not read classic HFS catalog properties");
    auto name=display(properties.name);
    auto full=folder.path+(folder.path.back()==':'?"":":")+name;
    auto location=origin;
    location.SetClassic(origin.ClassicImage(),full);
    if(origin.MediaGenerationKnown())location.BindMediaGeneration(origin.MediaGeneration());
    location.expectedCatalogId=entry.resolvedId;
    location.directory=entry.directory;
    if(!emit({std::move(name),{std::move(full),std::move(location),entry.id},BrowserDirectory::Bytes(entry)}))return false;
   }
   return true;
  });
  StorageLocation::ValidateMedia(origin,StorageLocation::FloppyProbe::FreshHandle);
  volume->Close();
 }

 void native(){
  auto origin=request_.Origin();
  auto volume=origin.Volume();
  StorageLocation::ValidateMedia(origin);
  const auto root=volume->Stat(origin.Id());
  if(!root.directory||!StorageLocation::MatchesEntry(origin,root))
   throw Storage::MediaChanged("The selected search folder changed.");
  origin.directory=true;
  auto route=request_.wholeVolume?std::vector<uint32_t>{2}:request_.scope.catalogRoute;
  if(route.empty())route={origin.Id()};
  std::string label;
  for(const auto id:route){
   if(job_.cancelled())throw Storage::Cancelled();
   if(!label.empty())label+=':';
   label+=BrowserPresentation::Utf8(id==root.id?root.name:volume->Stat(id).name);
  }
  walk({std::move(label),origin,root.id,0,std::move(route),{root.resolvedId?root.resolvedId:root.id}},
      [&](const Folder& folder,auto emit){
   for(const auto& entry:volume->List(folder.cursor)){
    auto name=BrowserPresentation::Utf8(entry.name);
    auto location=Location::Mounted(volume,entry.id);
    location.directory=entry.directory;
    location.expectedName=entry.name;
    location.expectedParent=entry.parent;
    Child child{name,{folder.path+":"+name,std::move(location),entry.id,0,folder.route},entry.directory?0:BrowserDirectory::Bytes(entry)};
    child.folder.route.push_back(entry.id);
    if(entry.directory&&request_.recursive){
     const auto resolved=entry.resolvedId?entry.resolvedId:entry.id;
     if(!entry.readError.empty()||folder.ancestors.count(resolved))child.recursion=Child::Recursion::Unreadable;
     else{child.folder.ancestors=folder.ancestors;child.folder.ancestors.insert(resolved);}
    }
    if(!emit(std::move(child)))return false;
   }
   return true;
  });
  StorageLocation::ValidateMedia(origin);
 }

 void windows(){
  auto origin=request_.Origin();
  const auto& root=origin.WindowsPath();
  if(request_.scope.checkWindowsMedia&&!origin.WindowsMediaSerial()){
   auto serial=StorageLocation::WindowsSerial(root);
   if(!serial)throw Storage::MediaChanged("Cannot identify the Windows search media.");
   origin.BindWindowsMediaSerial(serial);
  }
  StorageLocation::ValidateMedia(origin);
  const auto attributes=GetFileAttributesW(BrowserPresentation::WindowsPath(root).c_str());
  if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY))
   throw std::runtime_error("The selected search location is not an accessible Windows folder");
  if(!origin.expectedIdentity.empty())StorageLocation::ValidateFolderIdentity(origin,MacBrowser::DescribeWindowsFile(root));
  origin.directory=true;
  walk({Utf8(root),origin},[&](const Folder& folder,auto emit){
   auto prefix=folder.location.WindowsPath();
   if(prefix.back()!=L'\\'&&prefix.back()!=L'/')prefix+=L'\\';
   WindowsFolderEntries entries(BrowserPresentation::WindowsPath(prefix+L"*"));
   if(!entries){
    if(entries.Error()==ERROR_FILE_NOT_FOUND)return true;
    if(folder.depth==0)throw std::runtime_error("Could not enumerate the selected Windows folder (Windows error "+std::to_string(entries.Error())+")");
    ++output_.unreadable;return true;
   }
   do{
    const auto& entry=entries.Entry();
    const std::wstring name=entry.cFileName;
    if(name==L"."||name==L"..")continue;
    const auto full=prefix+name;
    auto location=Location::Windows(full);
    location.BindWindowsMediaSerial(origin.WindowsMediaSerial());
    location.directory=(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    const uint64_t bytes=location.directory?0:(uint64_t(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow;
    const auto recursion=(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)?Child::Recursion::Skip:Child::Recursion::Allowed;
    if(!emit({Utf8(name),{Utf8(full),std::move(location)},bytes,recursion}))return false;
   }while(entries.Next());
   if(entries.Error()!=ERROR_NO_MORE_FILES)
    throw std::runtime_error("Windows folder enumeration failed (Windows error "+std::to_string(entries.Error())+")");
   return true;
  });
  StorageLocation::ValidateMedia(origin);
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
  if(!results||request.pattern.empty()||request.pattern.size()>(request.UsesUnicode()?1020:255)||!request.limit)return {1,ERROR_INVALID_PARAMETER,"Enter a name or wildcard pattern of 1 to 255 characters."};
  if(request.Origin().IsNone())return {1,ERROR_INVALID_PARAMETER,"Select a folder or volume to search."};
  if(job.cancelled())throw Storage::Cancelled();
  *results={};Search search(request,*results,job);
  try{
   const auto& origin=request.Origin();
   const VolumeCoord::Lease read({{origin.ResourceKey(),origin.IsWindows()?VolumeCoord::Mode::Browse:VolumeCoord::Mode::Read}});
   if(origin.IsMounted())search.native();
   else if(origin.IsClassic())search.mac();
   else search.windows();
  }
  catch(const Storage::Cancelled&){throw;}
  catch(const Storage::MediaChanged& error){return {1,ERROR_MEDIA_CHANGED,error.what()};}
  catch(const std::exception& error){return {1,ERROR_READ_FAULT,error.what()};}
  std::sort(results->hits.begin(),results->hits.end(),[&](const FindHit& a,const FindHit& b){int compared=results->utf8?_wcsicmp(BrowserPresentation::FromUtf8(a.name).c_str(),BrowserPresentation::FromUtf8(b.name).c_str()):_stricmp(a.name.c_str(),b.name.c_str());return compared?compared<0:a.path<b.path;});
  return {};
 };
}
