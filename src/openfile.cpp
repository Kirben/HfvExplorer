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

#include <vector>

namespace {
struct DeleteHandle {
 HANDLE value=INVALID_HANDLE_VALUE;
 ~DeleteHandle(){DWORD error=GetLastError();if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);SetLastError(error);}
};
int delete_host_tree(const std::wstring& path,unsigned depth,Storage::Job* job,
                     const MacBrowser::ObjectIdentity* selected=nullptr) {
 if(job&&job->cancelled()){SetLastError(ERROR_CANCELLED);return 0;}
 if(depth>128||path.size()>=32764){SetLastError(ERROR_FILENAME_EXCED_RANGE);return 0;}
 DeleteHandle directory;
 // Pin the actual entry without following a junction, and prevent replacement
 // or reparse-point changes while enumerating its children.
 directory.value=CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
   FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(directory.value==INVALID_HANDLE_VALUE)return 0;
 BY_HANDLE_FILE_INFORMATION info;
 if(!GetFileInformationByHandle(directory.value,&info))return 0;
 if(selected&&(MacBrowser::WindowsIdentity(info)!=*selected||
               (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))){
  SetLastError(ERROR_FILE_INVALID);
  return 0;
 }
 if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){SetLastError(ERROR_DIRECTORY);return 0;}
 if(!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
  WIN32_FIND_DATAW entry;HANDLE search=FindFirstFileW((path+L"\\*").c_str(),&entry);
  if(search==INVALID_HANDLE_VALUE) {if(GetLastError()!=ERROR_FILE_NOT_FOUND)return 0;}
  else {
   int ok=1;DWORD error=ERROR_SUCCESS;
   do {
    if(!wcscmp(entry.cFileName,L".")||!wcscmp(entry.cFileName,L".."))continue;
    if(job&&job->cancelled()){ok=0;error=ERROR_CANCELLED;break;}
    auto child=path+L"\\"+entry.cFileName;
    if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)ok=delete_host_tree(child,depth+1,job);
    else {ok=DeleteFileW(child.c_str())!=FALSE;if(ok)Storage::OutcomeEvidence::NoteRetainedChange();}
    if(!ok){error=GetLastError();break;}
   }while(FindNextFileW(search,&entry));
   if(ok&&GetLastError()!=ERROR_NO_MORE_FILES){ok=0;error=GetLastError();}
   FindClose(search);if(!ok){SetLastError(error);return 0;}
  }
 }
 // Remove precisely the pinned entry, including when it is a junction.
 FILE_DISPOSITION_INFO disposition{TRUE};
 const bool removed=SetFileInformationByHandle(directory.value,FileDispositionInfo,
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
 return delete_host_tree(full,0,job,selected);
}
}
int DeleteHostSelectionJob(const std::wstring& path,bool directory,
                           const MacBrowser::EntryIdentity& expected,Storage::Job* job)
{
 if(job&&job->cancelled()){SetLastError(ERROR_CANCELLED);return 0;}
 if(path.empty()||expected.empty()){SetLastError(ERROR_FILE_INVALID);return 0;}
 const auto full=BrowserPresentation::WindowsPath(path);
 const auto slash=full.find_last_of(L'\\');
 if(slash==full.npos){SetLastError(ERROR_INVALID_NAME);return 0;}

 const auto pin=[](DeleteHandle& file,const std::wstring& name,const MacBrowser::ObjectIdentity& identity){
  file.value=CreateFileW(name.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  if(file.value==INVALID_HANDLE_VALUE)return false;
  BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(file.value,&info))return false;
  if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
      MacBrowser::WindowsIdentity(info)!=identity){
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
 if(!leaf.empty()&&leaf.front()!=L'%'&&!expected.companion.key.empty()&&
    !pin(companion,full.substr(0,slash+1)+L"%"+leaf,expected.companion))return 0;

 DeleteHandle primary;
 if(directory){
  if(!DeleteHostDirectory(full,job,&expected.primary))return 0;
 }else if(!pin(primary,full,expected.primary)||!remove(primary.value))return 0;
 Storage::OutcomeEvidence::NoteRetainedChange();
 return companion.value==INVALID_HANDLE_VALUE||remove(companion.value);
}
