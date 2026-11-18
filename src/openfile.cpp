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
#include "utils.h"
#include "StorageWorker.h"

#include <vector>
#include <set>

namespace {
struct DeleteHandle {
 HANDLE value=INVALID_HANDLE_VALUE;
 ~DeleteHandle(){DWORD error=GetLastError();if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);SetLastError(error);}
 void close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
};
int delete_host_tree(const CString& path,unsigned depth,Storage::Job* job) {
 if(job&&job->cancelled()){SetLastError(ERROR_CANCELLED);return 0;}
 if(depth>128||path.GetLength()>=MAX_PATH-3){SetLastError(ERROR_FILENAME_EXCED_RANGE);return 0;}
 DeleteHandle directory;
 // Pin the actual entry without following a junction, and prevent replacement
 // or reparse-point changes while enumerating its children.
 directory.value=CreateFileA(path,0,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
   FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(directory.value==INVALID_HANDLE_VALUE)return 0;
 BY_HANDLE_FILE_INFORMATION info;
 if(!GetFileInformationByHandle(directory.value,&info))return 0;
 if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){SetLastError(ERROR_DIRECTORY);return 0;}
 if(!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
  WIN32_FIND_DATAA entry;HANDLE search=FindFirstFileA(path+"\\*",&entry);
  if(search==INVALID_HANDLE_VALUE) {if(GetLastError()!=ERROR_FILE_NOT_FOUND)return 0;}
  else {
   int ok=1;DWORD error=ERROR_SUCCESS;
   do {
    if(!strcmp(entry.cFileName,".")||!strcmp(entry.cFileName,".."))continue;
    if(job&&job->cancelled()){ok=0;error=ERROR_CANCELLED;break;}
    CString child=path+"\\"+entry.cFileName;
    if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)ok=delete_host_tree(child,depth+1,job);
    else {ok=DeleteFileA(child)!=FALSE;if(ok)Storage::OutcomeEvidence::NoteRetainedChange();}
    if(!ok){error=GetLastError();break;}
   }while(FindNextFileA(search,&entry));
   if(ok&&GetLastError()!=ERROR_NO_MORE_FILES){ok=0;error=GetLastError();}
   FindClose(search);if(!ok){SetLastError(error);return 0;}
  }
 }
 directory.close();
 const bool removed=RemoveDirectoryA(path)!=FALSE; // A reparse point itself, never its target.
 if(removed)Storage::OutcomeEvidence::NoteRetainedChange();
 return removed;
}
}
int DeleteFATDirJob(CString &dir,Storage::Job* job)
{
 char path[MAX_PATH];DWORD length=GetFullPathNameA(dir,sizeof(path),path,nullptr);
 if(!length||length>=sizeof(path)||!strncmp(path,"\\\\?\\",4)||!strncmp(path,"\\\\.\\",4)) {SetLastError(ERROR_INVALID_NAME);return 0;}
 while(length>3&&path[length-1]=='\\')path[--length]=0;
 if(length==3&&path[1]==':'&&path[2]=='\\'){SetLastError(ERROR_ACCESS_DENIED);return 0;}
 if(path[0]=='\\'&&path[1]=='\\') {
  char* server=strchr(path+2,'\\');char* share=server?strchr(server+1,'\\'):nullptr;
  if(!share){SetLastError(ERROR_ACCESS_DENIED);return 0;}
 }
 return delete_host_tree(CString(path),0,job);
}

int DeleteFATDir(CString& dir){return DeleteFATDirJob(dir,nullptr);}
int DeleteHostSelectionJob(const CString& path,bool directory,Storage::Job* job)
{
 if(job&&job->cancelled()){SetLastError(ERROR_CANCELLED);return 0;}
 if(path.IsEmpty()){SetLastError(ERROR_INVALID_NAME);return 0;}
 if(!(directory?DeleteFATDirJob(const_cast<CString&>(path),job):DeleteFileA(path)))return 0;
 Storage::OutcomeEvidence::NoteRetainedChange();
 int slash=path.ReverseFind('\\');CString name=path.Mid(slash+1);
 // Selecting a metadata file never authorizes deleting the associated data file.
 if(name.IsEmpty()||name[0]=='%')return 1;
 CString companion=path.Left(slash+1)+"%"+name;
 DeleteHandle file;
 file.value=CreateFileA(companion,GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
   FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(file.value==INVALID_HANDLE_VALUE)return 1; // Unverified entries are retained.
 BY_HANDLE_FILE_INFORMATION info;LARGE_INTEGER size;BYTE header[26];DWORD got;
 if(!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
    !GetFileSizeEx(file.value,&size)||size.QuadPart<26||!ReadFile(file.value,header,sizeof(header),&got,nullptr)||got!=sizeof(header))return 1;
 auto word=[](const BYTE* p){return unsigned(p[0])*256+p[1];};
 auto number=[&](const BYTE* p){return DWORD(word(p))*65536+word(p+2);};
 if(number(header)!=0x00051607||number(header+4)!=0x00020000)return 1;
 unsigned count=word(header+24);ULONGLONG table=26ULL+count*12ULL;
 if(!count||table>ULONGLONG(size.QuadPart))return 1;
 bool metadata=false;std::set<DWORD> ids;
 std::vector<BYTE> entries(count*12);
 if(!ReadFile(file.value,entries.data(),DWORD(entries.size()),&got,nullptr)||got!=entries.size())return 1;
 for(unsigned i=0;i<count;++i) {
  const BYTE* entry=entries.data()+i*12;
  DWORD id=number(entry),offset=number(entry+4),length=number(entry+8);
  if(!id||!ids.insert(id).second||offset<table||offset>ULONGLONG(size.QuadPart)||length>ULONGLONG(size.QuadPart)-offset)return 1;
  if(id==2||(id==9&&length>=32))metadata=true;
 }
 if(!metadata)return 1;
 FILE_DISPOSITION_INFO removeVerified={TRUE};
 return SetFileInformationByHandle(file.value,FileDispositionInfo,&removeVerified,sizeof(removeVerified))!=FALSE;
}
int DeleteHostSelection(const CString& path,bool directory){return DeleteHostSelectionJob(path,directory,nullptr);}
