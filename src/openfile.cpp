/*
 * HFVExplorer
 * Copyright (C) 1997-1998 by Anygraaf Oy
 * Author: Lauri Pesonen, email: lpesonen@clinet.fi or lauri.pesonen@anygraaf.fi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "stdafx.h"
#include "BrowserPresentation.h"
#include "StorageWorker.h"
#include "WindowsEntryIdentity.h"
#include "WindowsMoveUndo.h"
#include "MacBrowserModel.h"
#include <set>

#include <vector>
#include <array>
#include <cstddef>
#ifdef HFV_TESTING
std::function<void()> windowsDeleteEnumeratedForTest;
#endif

namespace {
struct DeleteHandle {
 HANDLE value=INVALID_HANDLE_VALUE;
 ~DeleteHandle(){DWORD error=GetLastError();if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);SetLastError(error);}
};
struct DeleteChild {
 std::wstring name;
 uint64_t id, created, modified, length;
 DWORD attributes;
 std::vector<DeleteChild> children;
};
uint64_t FileTimeValue(FILETIME value)
{
 return (uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;
}
bool SnapshotDeleteChildren(HANDLE directory,Storage::Job* job,std::vector<DeleteChild>& children)
{
 alignas(8) std::array<BYTE,8192> buffer{};
 constexpr size_t header=offsetof(FILE_ID_BOTH_DIR_INFO,FileName);
 for(;;){
  if(job&&job->cancelled()){SetLastError(ERROR_CANCELLED);return false;}
  if(!GetFileInformationByHandleEx(directory,FileIdBothDirectoryInfo,buffer.data(),DWORD(buffer.size())))
   return GetLastError()==ERROR_NO_MORE_FILES;
  size_t offset=0;
  for(;;){
   const auto remaining=buffer.size()-offset;
   if(remaining<header){SetLastError(ERROR_INVALID_DATA);return false;}
   const auto& entry=*reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data()+offset);
   const auto next=entry.NextEntryOffset;
   const auto extent=next?next:remaining;
   if(extent<header||extent>remaining||(next%8)!=0||!entry.FileNameLength||
      entry.FileNameLength%sizeof(wchar_t)||entry.FileNameLength>extent-header){
    SetLastError(ERROR_INVALID_DATA);return false;
   }
   std::wstring name(entry.FileName,entry.FileNameLength/sizeof(wchar_t));
   if(name!=L"."&&name!=L".."){
    if(!entry.FileId.QuadPart||name.find_first_of(L"\\/:",0)!=name.npos||
       name.find(L'\0')!=name.npos){SetLastError(ERROR_FILE_INVALID);return false;}
    children.push_back({std::move(name),uint64_t(entry.FileId.QuadPart),
                        uint64_t(entry.CreationTime.QuadPart),uint64_t(entry.LastWriteTime.QuadPart),
                        uint64_t(entry.EndOfFile.QuadPart),entry.FileAttributes});
   }
   if(!next)break;
   offset+=next;
  }
 }
}
DWORD UnjournaledDeleteError(const std::wstring& folder,const std::vector<DeleteChild>& children)
{
 // Use the same percent-name convention as the Windows volume backend.
 // Match names in the inventory so a newly created file is never added to
 // the deletion plan and an unrelated percent-prefixed file can remain ordinary.
 const auto less=[](const std::wstring& a,const std::wstring& b){
  return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_LESS_THAN;
 };
 std::set<std::wstring,decltype(less)> names(less);
 for(const auto& entry:children)names.insert(entry.name);
 for(const auto& entry:children){
  if(entry.name.size()<2||entry.name.front()!=L'%'||
     (entry.attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
     names.find(entry.name.substr(1))==names.end())continue;
  try{
   if(MacBrowser::WindowsResourceCompanion(folder+L"\\"+entry.name))return ERROR_NOT_SUPPORTED;
  }catch(const std::exception&){
   return ERROR_FILE_INVALID;
  }
 }
 return ERROR_SUCCESS;
}

enum class DeleteTreePhase { Inventory, Remove };
int delete_host_tree(const std::wstring& path,unsigned depth,Storage::Job* job,
                     std::vector<DeleteChild>& children,DeleteTreePhase phase,
                     const MacBrowser::ObjectIdentity* selected=nullptr,
                     const DeleteChild* child=nullptr,DWORD volumeSerial=0) {
 if(job&&job->cancelled()){SetLastError(ERROR_CANCELLED);return 0;}
 if(depth>128||path.size()>=32764){SetLastError(ERROR_FILENAME_EXCED_RANGE);return 0;}
 DeleteHandle pinned;
 const DWORD listAccess=!child||(child->attributes&FILE_ATTRIBUTE_DIRECTORY)?FILE_LIST_DIRECTORY:0;
 // Keep the parent pinned while resolving child names. Open links as
 // entries so the handle can never address their targets.
 pinned.value=CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES|listAccess,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
   FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(pinned.value==INVALID_HANDLE_VALUE)return 0;
 BY_HANDLE_FILE_INFORMATION info{};
 if(!GetFileInformationByHandle(pinned.value,&info))return 0;
 const bool directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
 if(selected&&(!directory||MacBrowser::WindowsIdentity(info)!=*selected||
               (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))){
  SetLastError(ERROR_FILE_INVALID);return 0;
 }
 if(child){
  constexpr DWORD kind=FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT;
  const auto id=(uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
  const auto length=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
  if(info.dwVolumeSerialNumber!=volumeSerial||id!=child->id||
     (info.dwFileAttributes&kind)!=(child->attributes&kind)||
     FileTimeValue(info.ftCreationTime)!=child->created||
     (!directory&&(FileTimeValue(info.ftLastWriteTime)!=child->modified||length!=child->length))){
   SetLastError(ERROR_FILE_INVALID);return 0;
  }
 }
 if(directory&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
  if(phase==DeleteTreePhase::Inventory){
   if(!SnapshotDeleteChildren(pinned.value,job,children))return 0;
   const auto error=UnjournaledDeleteError(path,children);
   if(error!=ERROR_SUCCESS){SetLastError(error);return 0;}
  }
#ifdef HFV_TESTING
  if(phase==DeleteTreePhase::Remove&&windowsDeleteEnumeratedForTest)windowsDeleteEnumeratedForTest();
#endif
  for(auto& entry:children)
   if(!delete_host_tree(path+L"\\"+entry.name,depth+1,job,entry.children,phase,
                        nullptr,&entry,info.dwVolumeSerialNumber))return 0;
 }
 if(phase==DeleteTreePhase::Inventory)return 1;
 if(job&&job->cancelled()){SetLastError(ERROR_CANCELLED);return 0;}
 FILE_DISPOSITION_INFO disposition{TRUE};
 const bool removed=SetFileInformationByHandle(pinned.value,FileDispositionInfo,
                                               &disposition,sizeof(disposition))!=FALSE;
 if(removed)Storage::OutcomeEvidence::NoteRetainedChange();
 return removed;
}
int DeleteHostDirectory(const std::wstring& path,Storage::Job* job,const MacBrowser::ObjectIdentity* selected)
{
 auto full=path;
 while(full.size()>7&&full.back()==L'\\')full.pop_back();
 const bool unc=_wcsnicmp(full.c_str(),L"\\\\?\\UNC\\",8)==0;
 if(unc){
  const auto server=full.find(L'\\',8);
  if(server==full.npos||full.find(L'\\',server+1)==full.npos){
   SetLastError(ERROR_ACCESS_DENIED);return 0;
  }
 }else if(full.size()<=7||full[5]!=L':'||full[6]!=L'\\'){
  SetLastError(ERROR_ACCESS_DENIED);return 0;
 }
 DWORD error=ERROR_SUCCESS;
 int removed=0;
 {
  std::vector<DeleteChild> children;
  if(delete_host_tree(full,0,job,children,DeleteTreePhase::Inventory,selected))
   removed=delete_host_tree(full,0,job,children,DeleteTreePhase::Remove,selected);
  if(!removed)error=GetLastError();
 }
 if(!removed)SetLastError(error);
 return removed;
}
enum class DeleteMode { Selection, CutEntry };
int DeleteHostEntry(const std::wstring& path,bool directory,const MacBrowser::EntryIdentity& expected,
                    Storage::Job* job,DeleteMode mode,const MacBrowser::WindowsCutEntry* cut=nullptr)
{
 if(job&&job->cancelled()){SetLastError(ERROR_CANCELLED);return 0;}
 if(path.empty()||expected.empty()){SetLastError(ERROR_FILE_INVALID);return 0;}
 const auto full=BrowserPresentation::WindowsPath(path);
 const auto slash=full.find_last_of(L'\\');
 if(slash==full.npos){SetLastError(ERROR_INVALID_NAME);return 0;}

 const auto pin=[](DeleteHandle& file,const std::wstring& name,const MacBrowser::ObjectIdentity& identity,
                   bool directory=false){
  file.value=CreateFileW(name.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
  if(file.value==INVALID_HANDLE_VALUE)return false;
  BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(file.value,&info))return false;
  const auto actual=MacBrowser::WindowsIdentity(info);
  if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
     ((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)!=directory||
     actual.key!=identity.key||(!directory&&actual.version!=identity.version)){
   SetLastError(ERROR_FILE_INVALID);
   return false;
  }
  return true;
 };
 const auto remove=[](HANDLE file){
  FILE_DISPOSITION_INFO disposition{TRUE};
  return SetFileInformationByHandle(file,FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE;
 };

 // ResolveEntry already validated the AppleDouble contents. Hold that exact
 // companion through deletion, or leave both entries intact if it changed.
 DeleteHandle companion;
 const auto leaf=full.substr(slash+1);
 if(!leaf.empty()&&(leaf.front()!=L'%'||mode==DeleteMode::CutEntry)&&!expected.companion.key.empty()&&
    !pin(companion,full.substr(0,slash+1)+L"%"+leaf,expected.companion))return 0;

 DeleteHandle primary;
 if(directory&&mode==DeleteMode::Selection){
  if(!DeleteHostDirectory(full,job,&expected.primary))return 0;
 }else{
  if(!pin(primary,full,expected.primary,directory))return 0;
  if(cut&&cut->primaryStreams&&WindowsMoveUndo::CaptureNamedStreams(full)!=*cut->primaryStreams){
   SetLastError(ERROR_FILE_INVALID);
   return 0;
  }
  if(!remove(primary.value))return 0;
 }
 Storage::OutcomeEvidence::NoteRetainedChange();
 return companion.value==INVALID_HANDLE_VALUE||remove(companion.value);
}
}
int DeleteHostSelectionJob(const std::wstring& path,bool directory,
                           const MacBrowser::EntryIdentity& expected,Storage::Job* job)
{
 return DeleteHostEntry(path,directory,expected,job,DeleteMode::Selection);
}

int DeleteHostCutEntry(const MacBrowser::WindowsCutEntry& entry,Storage::Job& job)
{
 // The OS refuses to remove a directory with children. A recursive Cut must
 // never acquire permission over an entry that was absent from its snapshot.
 return DeleteHostEntry(entry.path,entry.directory,entry.identity,&job,DeleteMode::CutEntry,&entry);
}
