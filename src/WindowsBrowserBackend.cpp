// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "HFVExplorerDoc.h"
#include "HFVExplorerListView.h"
#include "BrowserDirectory.h"
#include "BrowserPresentation.h"
#include "HostFileSizes.h"
#include "charset.h"
#include "aspecial.h"
#include "utils.h"
#include <set>
#include <stdexcept>
extern HICON get_dos_icon(CHFVExplorerApp*,char*,int);
extern int is_afp_directory(char*);
namespace {
struct Find {
 HANDLE handle=INVALID_HANDLE_VALUE;
 ~Find(){if(handle!=INVALID_HANDLE_VALUE)FindClose(handle);}
};
std::string key(std::string name){if(!name.empty())CharUpperBuffA(&name[0],DWORD(name.size()));return name;}
}
BrowserDirectory::Snapshot CHFVExplorerDoc::read_windows_browser_directory(int volume,const char* directory,bool listing,Storage::Job& job){
 BrowserDirectory::ObserveRead(directory);
 BrowserDirectory::Snapshot snapshot;auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 std::string prefix=directory;if(prefix.empty())throw std::runtime_error("Select a Windows folder");if(prefix.back()!='\\')prefix+='\\';
 bool afp=is_afp_directory(const_cast<char*>(directory))!=0;
 char root[]={directory[0],':',char(92),0};bool writable=GetDriveTypeA(root)!=DRIVE_CDROM;
 std::set<std::string> ignored;
 snapshot.capabilitiesKnown=true;snapshot.capabilities.create=snapshot.capabilities.rename=snapshot.capabilities.remove=writable;
 auto append=[&](const char* display,const std::string& path,bool folder,const WIN32_FIND_DATAA& info,HICON small,HICON large,const char* type,const char* creator,bool companions,bool own=false){
  BrowserDirectory::ObserveRead(path.c_str());
  BrowserDirectory::Row row;row.storageType=LIST_FAT;row.volume=volume;row.path=path;
  row.entry.name=BrowserDirectory::FromAnsi(display);row.entry.directory=folder;
  row.created=BrowserDirectory::Ticks(info.ftCreationTime);row.modified=BrowserDirectory::Ticks(info.ftLastWriteTime);
  if(folder){row.countKnown=false;}
  else row.entry.forks[0].size=HostDirectoryEntrySize(info);
  memcpy(row.entry.finder.data(),type,(std::min)(strlen(type),size_t(4)));
  memcpy(row.entry.finder.data()+4,creator,(std::min)(strlen(creator),size_t(4)));
  if(companions&&!folder){auto normalized=MacBrowser::DescribeWindowsFile(MacBrowser::Wide(BrowserDirectory::FromAnsi(path.c_str())));
   normalized.name=row.entry.name;row.entry=std::move(normalized);
  }
  row.icon16=small;row.icon32=large;
  if(own&&(small||large)){row.ownedIcons=std::make_shared<BrowserDirectory::OwnedIcons>();row.ownedIcons->small=small;row.ownedIcons->large=large;}row.capabilities.create=row.capabilities.rename=row.capabilities.remove=writable;
  snapshot.rows.push_back(std::move(row));
 };
 // Keep the classic AppleDouble/AFP icon and filename conventions in this backend.
 // The view receives only a normalized row and its operation binding.
 {
  Find find;WIN32_FIND_DATAA info{};find.handle=FindFirstFileA((prefix+"%*.*").c_str(),&info);
  if(find.handle==INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Cannot enumerate Windows resource companions");
  if(find.handle!=INVALID_HANDLE_VALUE)do{
   if(job.cancelled())throw Storage::Cancelled();HICON small=nullptr,large=nullptr;char name[MAX_PATH],type[20]{},creator[20]{};int show=0;
   if(get_apple_double(prefix.c_str(),info.cFileName,&small,&large,name,type,creator,&show)){
    ignored.insert(key(std::string("%")+name));ignored.insert(key(name));
    if(show){std::string path=prefix+name;bool folder=isdirectory(path.c_str())!=0;
     remove_ardi_special_chars((unsigned char*)name);mac_to_pc_charset((unsigned char*)name);
     WIN32_FILE_ATTRIBUTE_DATA data{};WIN32_FIND_DATAA actual=info;
     if(GetFileAttributesExA(path.c_str(),GetFileExInfoStandard,&data)){actual.ftCreationTime=data.ftCreationTime;actual.ftLastWriteTime=data.ftLastWriteTime;actual.nFileSizeHigh=data.nFileSizeHigh;actual.nFileSizeLow=data.nFileSizeLow;}
     append(name,path,folder,actual,small,large,type,creator,true);
    }
   }
  }while(FindNextFileA(find.handle,&info));
  if(find.handle!=INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Windows companion enumeration failed");
 }
 DWORD deadline=app.m_dosicons!=DI_NO_ICONS&&app.m_fat_icon_time_limit>0?GetTickCount()+DWORD(app.m_fat_icon_time_limit)*1000:0;
 Find find;WIN32_FIND_DATAA info{};find.handle=FindFirstFileA((prefix+"*.*").c_str(),&info);
 if(find.handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return snapshot;throw std::runtime_error("Cannot enumerate Windows folder");}
 do{
  if(job.cancelled())throw Storage::Cancelled();bool folder=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;bool owned=false;
  if((folder&&(!strcmp(info.cFileName,".")||!strcmp(info.cFileName,"..")))||ignored.count(key(info.cFileName))||(!m_pc_show_invisibles&&(info.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN)))continue;
  dos_lfn_fdata(&info);std::string path=prefix+info.cFileName;HICON small=nullptr,large=nullptr;char type[20]{},creator[20]{},name[MAX_PATH];int show=1;
  if(!folder&&listing){
   if(afp)get_afp(prefix.c_str(),info.cFileName,&small,&large,name,type,creator,&show);
   if(!large){int mode=deadline&&LONG(GetTickCount()-deadline)>=0?DI_NO_ICONS:app.m_dosicons;small=get_dos_icon(&app,const_cast<char*>(path.c_str()),mode);if(small==(HICON)1)small=nullptr;large=small;owned=true;}
  }
  append(info.cFileName,path,folder,info,small,large,type,creator,false,owned);
  Storage::Progress progress;progress.phase="Reading folder";progress.path=path;job.progress(progress);
 }while(FindNextFileA(find.handle,&info));
 if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Windows folder enumeration failed");
 return snapshot;
}
void CHFVExplorerDoc::apply_browser_directory(const BrowserDirectory::Snapshot& snapshot,bool listing){
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());auto tree=app.m_tree;auto list=app.m_list;
 BrowserPresentation::Batch treeBatch(tree->GetTreeCtrl().m_hWnd),listBatch(list->GetListCtrl().m_hWnd,true);
 if(listing&&snapshot.capabilitiesKnown)list->SetBrowserCapabilities(snapshot.capabilities);
 HTREEITEM root=tree->GetTreeCtrl().GetSelectedItem();
 for(const auto& row:snapshot.rows){
  auto name=MacBrowser::Wide(row.entry.name);int n=WideCharToMultiByte(CP_ACP,0,name.c_str(),-1,nullptr,0,nullptr,nullptr);std::string ansi(n,0);
  if(n)WideCharToMultiByte(CP_ACP,0,name.c_str(),-1,&ansi[0],n,nullptr,nullptr);
  HTREEITEM child=nullptr;
  if(row.entry.directory){
   if(row.storageType==LIST_HFS)child=tree->tree_insert(m_volumes[row.volume].m_rootitem,row.volume,const_cast<char*>(ansi.c_str()),row.id,row.parent);
   else child=tree->tree_fat_insert(root,row.volume,const_cast<char*>(ansi.c_str()),const_cast<char*>(row.path.c_str()));
  }
  if(listing)list->InsertBrowserRow(row,child);
 }
}
